#include "desentry/common/platform.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
  #include <aclapi.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
#endif

namespace desentry {

namespace {
#if defined(_WIN32)
std::once_flag g_wsa_once;
bool g_wsa_ok = false;
#endif
}  // namespace

void NetInit() {
#if defined(_WIN32)
  std::call_once(g_wsa_once, [] {
    WSADATA wsa;
    g_wsa_ok = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
  });
  (void)g_wsa_ok;
#endif
}

void NetShutdown() {
  // Deliberately a no-op: WSACleanup() is refcounted and the process is
  // exiting anyway. Calling it from one component while another still holds
  // sockets is a classic Windows shutdown bug, so we simply never do.
}

void CloseSocket(dsn_socket_t s) {
  if (!SocketValid(s)) return;
#if defined(_WIN32)
  ::closesocket(s);
#else
  ::close(s);
#endif
}

void ShutdownSocket(dsn_socket_t s) {
  if (!SocketValid(s)) return;
#if defined(_WIN32)
  ::shutdown(s, SD_BOTH);
#else
  ::shutdown(s, SHUT_RDWR);
#endif
}

dsn_iolen_t SocketSend(dsn_socket_t s, const char* buf, size_t len) {
#if defined(_WIN32)
  return ::send(s, buf, static_cast<int>(len), 0);
#elif defined(MSG_NOSIGNAL)
  // Linux: never take SIGPIPE for a peer that hung up mid-write; the
  // return value already tells us.
  return ::send(s, buf, len, MSG_NOSIGNAL);
#else
  return ::send(s, buf, len, 0);
#endif
}

dsn_iolen_t SocketRecv(dsn_socket_t s, char* buf, size_t len) {
#if defined(_WIN32)
  return ::recv(s, buf, static_cast<int>(len), 0);
#else
  return ::recv(s, buf, len, 0);
#endif
}

void SetSocketTimeoutMs(dsn_socket_t s, int timeout_ms) {
#if defined(_WIN32)
  DWORD tv = static_cast<DWORD>(timeout_ms);
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void SetSockOptInt(dsn_socket_t s, int level, int optname, int value) {
#if defined(_WIN32)
  ::setsockopt(s, level, optname, reinterpret_cast<const char*>(&value), sizeof(value));
#else
  ::setsockopt(s, level, optname, &value, sizeof(value));
#endif
}

dsn_iolen_t SocketSendTo(dsn_socket_t s, const char* buf, size_t len,
                          const struct sockaddr* dest, dsn_socklen_t dest_len) {
#if defined(_WIN32)
  return ::sendto(s, buf, static_cast<int>(len), 0, dest, dest_len);
#else
  return ::sendto(s, buf, len, 0, dest, dest_len);
#endif
}

dsn_iolen_t SocketRecvFrom(dsn_socket_t s, char* buf, size_t len,
                            struct sockaddr* from, dsn_socklen_t* from_len) {
#if defined(_WIN32)
  return ::recvfrom(s, buf, static_cast<int>(len), 0, from, from_len);
#else
  return ::recvfrom(s, buf, len, 0, from, from_len);
#endif
}

bool SocketRetryable() {
#if defined(_WIN32)
  return ::WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

std::string SocketErrorString() {
#if defined(_WIN32)
  int err = ::WSAGetLastError();
  char buf[256] = {0};
  ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(err), 0, buf, sizeof(buf) - 1, nullptr);
  std::string msg(buf);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  return msg.empty() ? ("winsock error " + std::to_string(err)) : msg;
#else
  return std::string(std::strerror(errno));
#endif
}

bool SocketPair(dsn_socket_t out[2]) {
  NetInit();
#if !defined(_WIN32)
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  out[0] = fds[0];
  out[1] = fds[1];
  return true;
#else
  // Windows has no AF_UNIX socketpair, so this is the standard loopback
  // dance: listen on an ephemeral port bound to 127.0.0.1 only, connect to
  // the address the OS chose, accept, and hand back both ends. Binding to
  // loopback means the transient listener is never reachable from the
  // network, and it is closed before this returns either way.
  dsn_socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!SocketValid(listener)) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // let the OS choose

  auto give_up = [&](dsn_socket_t a, dsn_socket_t b) {
    if (SocketValid(a)) CloseSocket(a);
    if (SocketValid(b)) CloseSocket(b);
    CloseSocket(listener);
    return false;
  };

  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    return give_up(kInvalidSocket, kInvalidSocket);
  }
  dsn_socklen_t addr_len = sizeof(addr);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
    return give_up(kInvalidSocket, kInvalidSocket);
  }
  if (::listen(listener, 1) != 0) return give_up(kInvalidSocket, kInvalidSocket);

  dsn_socket_t client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!SocketValid(client)) return give_up(kInvalidSocket, kInvalidSocket);
  if (::connect(client, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
    return give_up(client, kInvalidSocket);
  }

  dsn_socket_t server = ::accept(listener, nullptr, nullptr);
  if (!SocketValid(server)) return give_up(client, kInvalidSocket);

  CloseSocket(listener);
  out[0] = client;
  out[1] = server;
  return true;
#endif
}

// -- filesystem -----------------------------------------------------------

bool MakeDir(const std::string& path) {
  if (path.empty()) return false;
  errno = 0;
#if defined(_WIN32)
  if (::_mkdir(path.c_str()) == 0) return true;
#else
  if (::mkdir(path.c_str(), 0755) == 0) return true;
#endif
  return errno == EEXIST;
}

bool MakeDirs(const std::string& path) {
  if (path.empty()) return false;
  std::string acc;
  for (size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    acc.push_back(c);
    bool sep = (c == '/' || c == '\\');
    if (!sep && i + 1 != path.size()) continue;
    std::string component = acc;
    if (sep) component.pop_back();
    if (component.empty()) continue;
    // A bare drive designator ("C:") is not a directory to create.
    if (component.size() == 2 && component[1] == ':') continue;
    if (!MakeDir(component) && !IsDirectory(component)) return false;
  }
  return IsDirectory(path);
}

bool PathExists(const std::string& path) {
#if defined(_WIN32)
  return ::GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
#endif
}

bool IsDirectory(const std::string& path) {
#if defined(_WIN32)
  DWORD attr = ::GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
#endif
}

bool RestrictToOwner(const std::string& path) {
#if defined(_WIN32)
  // Windows has no chmod(0600). The equivalent is an explicit DACL with a
  // single ACE granting the current user full control and no inheritance
  // from the parent directory -- otherwise "Users" typically still has read
  // access to a file under %LOCALAPPDATA%. This is the real protection for
  // identity.key on Windows; silently no-op'ing chmod here would leave the
  // private key readable by every local account on the platform we ship
  // most of.
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  bool ok = false;
  if (size > 0) {
    std::vector<char> buf(size);
    if (::GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
      auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
      EXPLICIT_ACCESSA ea;
      std::memset(&ea, 0, sizeof(ea));
      ea.grfAccessPermissions = GENERIC_ALL;
      ea.grfAccessMode = SET_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
      ea.Trustee.ptstrName = reinterpret_cast<LPSTR>(user->User.Sid);
      PACL acl = nullptr;
      if (::SetEntriesInAclA(1, &ea, nullptr, &acl) == ERROR_SUCCESS) {
        ok = ::SetNamedSecurityInfoA(
                 const_cast<LPSTR>(path.c_str()), SE_FILE_OBJECT,
                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                 nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
        ::LocalFree(acl);
      }
    }
  }
  ::CloseHandle(token);
  return ok;
#else
  return ::chmod(path.c_str(), 0600) == 0;
#endif
}

bool SyncFileByPath(const std::string& path, std::string* err) {
#if defined(_WIN32)
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = "cannot open for sync: " + path;
    return false;
  }
  bool ok = ::FlushFileBuffers(h) != 0;
  if (!ok && err) *err = "FlushFileBuffers failed: " + path;
  ::CloseHandle(h);
  return ok;
#else
#if defined(__APPLE__)
  int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    if (err) *err = "cannot open for sync: " + path + ": " + std::strerror(errno);
    return false;
  }
  bool ok = (::fcntl(fd, F_FULLFSYNC) == 0);
  if (!ok) ok = (::fsync(fd) == 0);  // fallback if F_FULLFSYNC unavailable
  if (!ok && err) *err = "F_FULLFSYNC/fsync failed: " + path + ": " + std::strerror(errno);
  ::close(fd);
  return ok;
#else
  int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    if (err) *err = "cannot open for sync: " + path + ": " + std::strerror(errno);
    return false;
  }
#if defined(__linux__)
  bool ok = (::fdatasync(fd) == 0);
#else
  bool ok = (::fsync(fd) == 0);
#endif
  if (!ok && err) *err = "fdatasync/fsync failed: " + path + ": " + std::strerror(errno);
  ::close(fd);
  return ok;
#endif
#endif
}

bool SyncDirForFile(const std::string& file_path, std::string* err) {
#if defined(_WIN32)
  (void)file_path;
  (void)err;
  return true;  // Windows directory entries are durable via file flush; no-op.
#else
  std::string dir = file_path;
  size_t sep = dir.find_last_of('/');
  if (sep == std::string::npos) dir = ".";
  else if (sep == 0) dir = "/";
  else dir = dir.substr(0, sep);
  int fd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                    | O_DIRECTORY
#endif
  );
  if (fd < 0) {
    if (err) *err = "cannot open dir for sync: " + dir;
    return false;
  }
  bool ok = (::fsync(fd) == 0);
  if (!ok && err) *err = "dir fsync failed: " + dir + ": " + std::strerror(errno);
  ::close(fd);
  return ok;
#endif
}

bool ParseBindAddr(const std::string& bind_addr, uint16_t port, sockaddr_storage* out,
                   dsn_socklen_t* out_len) {
  std::memset(out, 0, sizeof(*out));
  if (bind_addr.empty() || bind_addr == "0.0.0.0") {
    auto* a = reinterpret_cast<sockaddr_in*>(out);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = INADDR_ANY;
    *out_len = sizeof(sockaddr_in);
    return true;
  }
  if (bind_addr == "::") {
    auto* a = reinterpret_cast<sockaddr_in6*>(out);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    a->sin6_addr = in6addr_any;
    *out_len = sizeof(sockaddr_in6);
    return true;
  }
  {
    auto* a = reinterpret_cast<sockaddr_in*>(out);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &a->sin_addr) == 1) {
      a->sin_family = AF_INET;
      a->sin_port = htons(port);
      *out_len = sizeof(sockaddr_in);
      return true;
    }
  }
  {
    auto* a = reinterpret_cast<sockaddr_in6*>(out);
    if (::inet_pton(AF_INET6, bind_addr.c_str(), &a->sin6_addr) == 1) {
      a->sin6_family = AF_INET6;
      a->sin6_port = htons(port);
      *out_len = sizeof(sockaddr_in6);
      return true;
    }
  }
  return false;
}

void TryDualStack(dsn_socket_t s) {
  int off = 0;
#if defined(_WIN32)
  ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
#else
  ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
#endif
}

bool ResolvePeerAddr(const std::string& host, uint16_t port, sockaddr_storage* out,
                     dsn_socklen_t* out_len) {
  std::memset(out, 0, sizeof(*out));
  {
    auto* a = reinterpret_cast<sockaddr_in*>(out);
    if (::inet_pton(AF_INET, host.c_str(), &a->sin_addr) == 1) {
      a->sin_family = AF_INET;
      a->sin_port = htons(port);
      *out_len = sizeof(sockaddr_in);
      return true;
    }
  }
  {
    auto* a = reinterpret_cast<sockaddr_in6*>(out);
    std::string h = host;
    // Strip brackets: [::1]:port style and bare [::1].
    if (h.size() > 2 && h.front() == '[') {
      auto end = h.find(']');
      if (end == std::string::npos) return false;
      h = h.substr(1, end - 1);
    }
    if (::inet_pton(AF_INET6, h.c_str(), &a->sin6_addr) == 1) {
      a->sin6_family = AF_INET6;
      a->sin6_port = htons(port);
      *out_len = sizeof(sockaddr_in6);
      return true;
    }
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
  bool ok = false;
  if (res->ai_family == AF_INET && res->ai_addrlen >= sizeof(sockaddr_in)) {
    std::memcpy(out, res->ai_addr, sizeof(sockaddr_in));
    reinterpret_cast<sockaddr_in*>(out)->sin_port = htons(port);
    *out_len = sizeof(sockaddr_in);
    ok = true;
  } else if (res->ai_family == AF_INET6 && res->ai_addrlen >= sizeof(sockaddr_in6)) {
    std::memcpy(out, res->ai_addr, sizeof(sockaddr_in6));
    reinterpret_cast<sockaddr_in6*>(out)->sin6_port = htons(port);
    *out_len = sizeof(sockaddr_in6);
    ok = true;
  }
  ::freeaddrinfo(res);
  return ok;
}

uint64_t FileSize(const std::string& path) {
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_size);
#endif
}

std::vector<std::string> ListDir(const std::string& path) {
  std::vector<std::string> out;
#if defined(_WIN32)
  WIN32_FIND_DATAA find;
  HANDLE h = ::FindFirstFileA((path + "\\*").c_str(), &find);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    std::string name = find.cFileName;
    if (name != "." && name != "..") out.push_back(name);
  } while (::FindNextFileA(h, &find));
  ::FindClose(h);
#else
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) return out;
  while (struct dirent* e = ::readdir(dir)) {
    std::string name = e->d_name;
    if (name != "." && name != "..") out.push_back(name);
  }
  ::closedir(dir);
#endif
  return out;
}

bool RemoveTree(const std::string& path) {
  if (!PathExists(path)) return true;
  if (IsDirectory(path)) {
    for (const std::string& name : ListDir(path)) {
      if (!RemoveTree(path + "/" + name)) return false;
    }
#if defined(_WIN32)
    return ::_rmdir(path.c_str()) == 0;
#else
    return ::rmdir(path.c_str()) == 0;
#endif
  }
  return std::remove(path.c_str()) == 0;
}

std::string AppDataDir() {
#if defined(_WIN32)
  const char* base = std::getenv("LOCALAPPDATA");
  std::string root = base ? base : ".";
  return root + "\\DeSentry";
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  std::string root = home ? home : ".";
  return root + "/Library/Application Support/DeSentry";
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/desentry";
  }
  const char* home = std::getenv("HOME");
  std::string root = home ? home : ".";
  return root + "/.local/share/desentry";
#endif
}

// -- time / process --------------------------------------------------------

void SleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t MonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::tm LocalTime(std::time_t t) {
  std::tm out{};
#if defined(_WIN32)
  ::localtime_s(&out, &t);
#else
  ::localtime_r(&t, &out);
#endif
  return out;
}

}  // namespace desentry
