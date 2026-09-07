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

void WriteHttpResponse(dsn_socket_t fd, const HttpResponse& resp) {
  std::ostringstream out;
  out << "HTTP/1.1 " << resp.status << " " << StatusText(resp.status) << "\r\n";
  out << "Content-Type: " << resp.content_type << "\r\n";
  out << "Content-Length: " << resp.body.size() << "\r\n";
  out << "Connection: close\r\n";
  out << "Server: de-sentry\r\n";
  // This API is loopback-only by default (config/node.example.json binds
  // 127.0.0.1) and carries no session/cookie auth to leak, so a permissive
  // CORS header is safe and is what lets a plain static HTML page (e.g.
  // tools/dashboard.html, opened directly as a file:// page, or a browser
  // extension/agent UI on another origin) call it straight from the
  // browser without standing up a proxy.
  out << "Access-Control-Allow-Origin: *\r\n";
  out << "Access-Control-Allow-Methods: GET, PUT, POST, DELETE, OPTIONS\r\n";
  out << "Access-Control-Allow-Headers: Content-Type\r\n";
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
  dsn_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!SocketValid(fd)) return false;
  SetSockOptInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (bind_addr_.empty() || bind_addr_ == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
    CloseSocket(fd);
    return false;
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
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
  accept_thread_ = std::thread(&HttpServer::AcceptLoop, this);
  DSN_LOG_INFO("http", "API listening on http://" << bind_addr_ << ":" << port_);
  return true;
}

void HttpServer::AcceptLoop() {
  while (running_) {
    sockaddr_in peer{};
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
    WriteHttpResponse(fd, resp);
  } else {
    WriteHttpResponse(fd, HttpResponse::Json(400, R"({"error":"malformed request"})"));
  }
  CloseSocket(fd);
}

HttpResponse HttpServer::Dispatch(HttpRequest* req) {
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
