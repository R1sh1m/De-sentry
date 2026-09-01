#pragma once
// A minimal, dependency-free HTTP/1.1 server, built the same way the P2P
// transport was: raw POSIX sockets, thread-per-connection. This is what
// applications on the same host talk to -- the "client" half of "every
// peer is both server and client" (ARCHITECTURE.md §3.1). It only
// implements the request/response subset this engine's REST API actually
// needs (no chunked transfer-encoding, no keep-alive, no HTTP/2) -- scope
// stated plainly rather than pretending this is a general-purpose web
// server.

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace desentry {

struct HttpRequest {
  std::string method;
  std::string path;                        // without query string
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> params;  // path params from :name segments
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;

  static HttpResponse Json(int status, std::string json_body) {
    return HttpResponse{status, "application/json", std::move(json_body)};
  }
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  HttpServer(std::string bind_addr, uint16_t port) : bind_addr_(std::move(bind_addr)), port_(port) {}
  ~HttpServer();

  void Get(const std::string& pattern, HttpHandler handler) { AddRoute("GET", pattern, std::move(handler)); }
  void Put(const std::string& pattern, HttpHandler handler) { AddRoute("PUT", pattern, std::move(handler)); }
  void Post(const std::string& pattern, HttpHandler handler) { AddRoute("POST", pattern, std::move(handler)); }
  void Del(const std::string& pattern, HttpHandler handler) { AddRoute("DELETE", pattern, std::move(handler)); }

  bool Start();
  void Stop();

 private:
  struct Route {
    std::string method;
    std::vector<std::string> segments;  // "" pattern -> {}; literal or ":name"
    HttpHandler handler;
  };

  void AddRoute(const std::string& method, const std::string& pattern, HttpHandler handler);
  void AcceptLoop();
  void HandleConnection(int fd);
  HttpResponse Dispatch(HttpRequest* req);

  std::string bind_addr_;
  uint16_t port_;
  std::vector<Route> routes_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread accept_thread_;
};

}  // namespace desentry
