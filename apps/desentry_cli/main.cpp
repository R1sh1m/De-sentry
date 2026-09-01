// desentry_cli -- a thin command-line client for a node's local REST API.
// This is a convenience/demo tool, not part of the engine: it just speaks
// plain HTTP to whatever --api host:port you point it at (default
// 127.0.0.1:7701), exactly like curl would, but with less typing for the
// demo script (scripts/run_cluster.sh).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool HttpCall(const std::string& host, uint16_t port, const std::string& method, const std::string& path,
              const std::string& body, std::string* out_body, int* out_status) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }

  std::ostringstream req;
  req << method << " " << path << " HTTP/1.1\r\n";
  req << "Host: " << host << "\r\n";
  req << "Content-Length: " << body.size() << "\r\n";
  req << "Content-Type: application/json\r\n";
  req << "Connection: close\r\n\r\n";
  req << body;
  std::string req_s = req.str();
  size_t sent = 0;
  while (sent < req_s.size()) {
    ssize_t n = ::send(fd, req_s.data() + sent, req_s.size() - sent, 0);
    if (n <= 0) { ::close(fd); return false; }
    sent += static_cast<size_t>(n);
  }

  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, static_cast<size_t>(n));
  ::close(fd);

  auto line_end = resp.find("\r\n");
  if (line_end == std::string::npos) return false;
  std::istringstream status_line(resp.substr(0, line_end));
  std::string http_version;
  status_line >> http_version >> *out_status;

  auto header_end = resp.find("\r\n\r\n");
  *out_body = header_end == std::string::npos ? "" : resp.substr(header_end + 4);
  return true;
}

void PrintUsage() {
  std::printf(
      "usage: desentry_cli [--api host:port] <command> [args...]\n\n"
      "commands:\n"
      "  put <collection> <key> <json>   upsert a document\n"
      "  get <collection> <key>          fetch a document\n"
      "  delete <collection> <key>       delete a document\n"
      "  list <collection> [limit]       list documents in a collection\n"
      "  schema <collection> <json>      attach a JSON-Schema validator\n"
      "  peers                           list known P2P peers\n"
      "  status                          node status\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 7701;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--api" && i + 1 < argc) {
      std::string spec = argv[++i];
      auto pos = spec.find(':');
      if (pos != std::string::npos) {
        host = spec.substr(0, pos);
        port = static_cast<uint16_t>(std::stoi(spec.substr(pos + 1)));
      }
    } else {
      args.push_back(a);
    }
  }

  if (args.empty()) { PrintUsage(); return 1; }
  const std::string& cmd = args[0];

  std::string method, path, body;
  if (cmd == "put" && args.size() >= 4) {
    method = "PUT";
    path = "/db/" + args[1] + "/" + args[2];
    body = args[3];
  } else if (cmd == "get" && args.size() >= 3) {
    method = "GET";
    path = "/db/" + args[1] + "/" + args[2];
  } else if (cmd == "delete" && args.size() >= 3) {
    method = "DELETE";
    path = "/db/" + args[1] + "/" + args[2];
  } else if (cmd == "list" && args.size() >= 2) {
    method = "GET";
    path = "/db/" + args[1];
    if (args.size() >= 3) path += "?limit=" + args[2];
  } else if (cmd == "schema" && args.size() >= 3) {
    method = "PUT";
    path = "/_schema/" + args[1];
    body = args[2];
  } else if (cmd == "peers") {
    method = "GET";
    path = "/_peers";
  } else if (cmd == "status") {
    method = "GET";
    path = "/_status";
  } else {
    PrintUsage();
    return 1;
  }

  std::string resp_body;
  int status = 0;
  if (!HttpCall(host, port, method, path, body, &resp_body, &status)) {
    std::fprintf(stderr, "error: could not reach node at %s:%u\n", host.c_str(), port);
    return 1;
  }
  std::cout << resp_body << std::endl;
  return status < 400 ? 0 : 1;
}
