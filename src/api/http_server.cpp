#include "desentry/api/http_server.h"


#include <algorithm>
#include <cstring>
#include <sstream>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {

std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = s.substr(i + 1, 2);
      char c = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
      out.push_back(c);
      i += 2;
    } else if (s[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> segs;
  std::string cur;
  for (char c : path) {
    if (c == '/') {
      if (!cur.empty()) segs.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) segs.push_back(cur);
  return segs;
}

std::string StatusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "Unknown";
  }
}

// Reads a full HTTP request (headers + body, per Content-Length) off a
// blocking socket. Returns false on a malformed/incomplete request (peer
// disconnected, garbage input) -- the caller just closes the connection,
// same treatment as any other untrusted local input.
bool ReadHttpRequest(dsn_socket_t fd, HttpRequest* req) {
  std::string buf;
  buf.reserve(4096);
  char chunk[4096];
  size_t header_end = std::string::npos;

  while (header_end == std::string::npos) {
    dsn_iolen_t n = SocketRecv(fd, chunk, sizeof(chunk));
    if (n <= 0) return false;
    buf.append(chunk, static_cast<size_t>(n));
    header_end = buf.find("\r\n\r\n");
    if (buf.size() > (1u << 20)) return false;  // 1MiB header cap -- generous, bounded
  }

  std::string header_block = buf.substr(0, header_end);
  std::string body_so_far = buf.substr(header_end + 4);

  std::istringstream hs(header_block);
  std::string request_line;
  if (!std::getline(hs, request_line)) return false;
  if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

  std::istringstream rl(request_line);
  std::string full_path, http_version;
  rl >> req->method >> full_path >> http_version;
  if (req->method.empty() || full_path.empty()) return false;

  auto qpos = full_path.find('?');
  if (qpos == std::string::npos) {
    req->path = UrlDecode(full_path);
  } else {
    req->path = UrlDecode(full_path.substr(0, qpos));
    std::string qs = full_path.substr(qpos + 1);
    std::istringstream qss(qs);
    std::string pair;
    while (std::getline(qss, pair, '&')) {
      auto eq = pair.find('=');
      if (eq == std::string::npos) {
        req->query[UrlDecode(pair)] = "";
      } else {
        req->query[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
      }
    }
  }

  std::string line;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    while (!val.empty() && val.front() == ' ') val.erase(val.begin());
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    req->headers[key] = val;
  }

  size_t content_length = 0;
  auto cl_it = req->headers.find("content-length");
  if (cl_it != req->headers.end()) {
    content_length = static_cast<size_t>(std::strtoul(cl_it->second.c_str(), nullptr, 10));
  }
  if (content_length > (64u << 20)) return false;  // 64MiB body cap

  while (body_so_far.size() < content_length) {
    dsn_iolen_t n = SocketRecv(fd, chunk, sizeof(chunk));
    if (n <= 0) return false;
    body_so_far.append(chunk, static_cast<size_t>(n));
  }
  req->body = body_so_far.substr(0, content_length);
  return true;
}

namespace {

// Origins the Tauri webview actually uses. Only these are ever echoed back;
// everything else gets no ACAO header, so a random web page cannot read the
// loopback API even when it can reach it (C-8 fix). The localhost:5273
// entries are the vite dev server (tauri.conf devUrl): without them
// `npm run tauri:dev` cannot call the API at all. They are dev-only origins
// in the sense that only a process serving that exact port can present
// them -- and the bearer token is still required for every non-OPTIONS
// call, so an echoed dev origin alone grants nothing.
bool IsAllowedOrigin(const std::string& origin) {
  return origin == "tauri://localhost" || origin == "http://tauri.localhost" ||
         origin == "https://tauri.localhost" || origin == "http://localhost:5273" ||
         origin == "http://127.0.0.1:5273";
}

// Constant-time bearer comparison: the token is a secret, and a byte-at-a-
// time early-out would oracle its prefix through timing.
bool BearerEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
  return diff == 0;
}

// Host header must name loopback (DNS-rebinding defense). Accepts
// 127.0.0.1, localhost and ::1 with optional :port, case-insensitive.
// Absent header (HTTP/1.0, some curl uses) is allowed -- the bearer token
// is the real gate when configured.
bool HostIsLoopback(const std::string& host) {
  std::string h = host;
  auto colon = h.rfind(':');
  // Strip :port, but not the colons inside [::1].
  if (!h.empty() && h.front() != '[' && colon != std::string::npos) h = h.substr(0, colon);
  if (h == "[::1]") h = "::1";
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
  return h.empty() || h == "127.0.0.1" || h == "localhost" || h == "::1";
}

}  // namespace

void WriteHttpResponse(dsn_socket_t fd, const HttpResponse& resp, const std::string& origin_echo,
                       bool strict_cors) {
  std::ostringstream out;
  out << "HTTP/1.1 " << resp.status << " " << StatusText(resp.status) << "\r\n";
  out << "Content-Type: " << resp.content_type << "\r\n";
  out << "Content-Length: " << resp.body.size() << "\r\n";
  out << "Connection: close\r\n";
  out << "Server: de-sentry\r\n";
  // Strict mode (bearer configured, i.e. app-launched): echo only an
  // allowlisted webview origin. Dev mode (no bearer): preserve the old
  // wildcard so curl/file:// dashboard flows keep working -- with a startup
  // warning that this is not a security boundary.
  if (!origin_echo.empty()) {
    out << "Access-Control-Allow-Origin: " << origin_echo << "\r\n";
    out << "Vary: Origin\r\n";
  } else if (!strict_cors) {
    out << "Access-Control-Allow-Origin: *\r\n";
  }
  out << "Access-Control-Allow-Methods: GET, PUT, POST, DELETE, OPTIONS\r\n";
  out << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
  out << "\r\n";
  out << resp.body;
  std::string s = out.str();
  size_t sent = 0;
  while (sent < s.size()) {
    dsn_iolen_t n = SocketSend(fd, s.data() + sent, s.size() - sent);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
}

}  // namespace

HttpServer::~HttpServer() { Stop(); }

void HttpServer::AddRoute(const std::string& method, const std::string& pattern, HttpHandler handler) {
  routes_.push_back(Route{method, SplitPath(pattern), std::move(handler)});
}

bool HttpServer::Start() {
  NetInit();
  // Dual-stack listen (M-11 fix): same rules as the P2P transport --
  // 127.0.0.1/0.0.0.0 stay IPv4; "::"/"::1" bind AF_INET6 dual-stack.
  sockaddr_storage addr{};
  dsn_socklen_t addr_len = 0;
  if (!ParseBindAddr(bind_addr_, port_, &addr, &addr_len)) return false;
  dsn_socket_t fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
  if (!SocketValid(fd)) return false;
  SetSockOptInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
  if (addr.ss_family == AF_INET6) TryDualStack(fd);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
    DSN_LOG_ERROR("http", "bind() failed on " << bind_addr_ << ":" << port_ << ": " << SocketErrorString());
    CloseSocket(fd);
    return false;
  }
  if (::listen(fd, 64) != 0) {
    CloseSocket(fd);
    return false;
  }

  listen_fd_ = fd;
  running_ = true;
  if (bearer_token_.empty()) {
    DSN_LOG_WARN("http", "no API bearer token configured: loopback API accepts unauthenticated "
                         "requests and CORS is permissive. Set DESENTRY_API_TOKEN (the desktop "
                         "sidecar always does) before exposing this port beyond loopback.");
  }
  accept_thread_ = std::thread(&HttpServer::AcceptLoop, this);
  DSN_LOG_INFO("http", "API listening on http://" << bind_addr_ << ":" << port_);
  return true;
}

void HttpServer::AcceptLoop() {
  while (running_) {
    sockaddr_storage peer{};
    dsn_socklen_t len = sizeof(peer);
    dsn_socket_t fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (!SocketValid(fd)) {
      if (!running_) break;
      continue;
    }
    std::thread(&HttpServer::HandleConnection, this, fd).detach();
  }
}

void HttpServer::HandleConnection(dsn_socket_t fd) {
  HttpRequest req;
  if (ReadHttpRequest(fd, &req)) {
    HttpResponse resp = Dispatch(&req);
    std::string origin;
    auto oit = req.headers.find("origin");
    if (oit != req.headers.end() && IsAllowedOrigin(oit->second)) origin = oit->second;
    WriteHttpResponse(fd, resp, origin, !bearer_token_.empty());
  } else {
    WriteHttpResponse(fd, HttpResponse::Json(400, R"({"error":"malformed request"})"), "", false);
  }
  CloseSocket(fd);
}

HttpResponse HttpServer::Dispatch(HttpRequest* req) {
  // Host validation (DNS-rebinding defense, C-8) applies when loopback-bound:
  // that is the case a browser can reach but must not address by another
  // name. A non-loopback bind (0.0.0.0/"::" in containers, LAN deployments)
  // is network-reachable by design -- its Host values are legitimately
  // diverse (service names, container hostnames) -- so the check is skipped
  // there and the bearer token (when configured) is the gate.
  if (bind_addr_ == "127.0.0.1" || bind_addr_ == "localhost" || bind_addr_ == "::1") {
    auto hit = req->headers.find("host");
    if (hit != req->headers.end() && !HostIsLoopback(hit->second)) {
      return HttpResponse::Json(403, R"({"error":"forbidden: host not allowed"})");
    }
  }
  // Bearer gate (C-8). Preflight carries no Authorization header by design,
  // so OPTIONS is answered without it; the real request still must present
  // the token.
  if (!bearer_token_.empty() && req->method != "OPTIONS") {
    auto ait = req->headers.find("authorization");
    const std::string want = "Bearer " + bearer_token_;
    if (ait == req->headers.end() || !BearerEquals(ait->second, want)) {
      return HttpResponse::Json(401, R"({"error":"unauthorized"})");
    }
  }
  // CORS preflight: browsers send this ahead of a "non-simple" cross-origin
  // request (e.g. PUT with a JSON body). No route ever registers OPTIONS,
  // so without this every preflight would 405 and the browser would then
  // refuse to send the real request at all.
  if (req->method == "OPTIONS") {
    return HttpResponse{204, "text/plain", ""};
  }

  auto req_segs = SplitPath(req->path);
  bool path_matched_any_method = false;

  for (auto& route : routes_) {
    if (route.segments.size() != req_segs.size()) continue;
    std::map<std::string, std::string> params;
    bool match = true;
    for (size_t i = 0; i < route.segments.size(); ++i) {
      if (!route.segments[i].empty() && route.segments[i][0] == ':') {
        params[route.segments[i].substr(1)] = req_segs[i];
      } else if (route.segments[i] != req_segs[i]) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    path_matched_any_method = true;
    if (route.method != req->method) continue;

    req->params = std::move(params);
    try {
      return route.handler(*req);
    } catch (const std::exception& e) {
      return HttpResponse::Json(500, std::string(R"({"error":")") + e.what() + R"("})");
    }
  }

  if (path_matched_any_method) {
    return HttpResponse::Json(405, R"({"error":"method not allowed"})");
  }
  return HttpResponse::Json(404, R"({"error":"not found"})");
}

void HttpServer::Stop() {
  if (!running_) return;
  running_ = false;
  if (SocketValid(listen_fd_)) {
    ShutdownSocket(listen_fd_);
    CloseSocket(listen_fd_);
    listen_fd_ = kInvalidSocket;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
}

}  // namespace desentry
