#pragma once
// Cross-platform primitives. The v1 engine was written against POSIX
// directly (sys/socket.h, unistd.h, sys/stat.h); v2 ships as a desktop
// application on Windows 11, macOS 14+ and Ubuntu 22.04+, so every
// platform-specific call in the engine now goes through this one header.
//
// Design rule, same spirit as security/crypto.h being the only file that
// touches OpenSSL: this is the *only* file allowed to `#ifdef _WIN32`.
// Everything above it is written once, portably. That keeps the "does this
// work on Windows?" question answerable by reading one file rather than
// auditing every socket call in the tree.

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <direct.h>
  #include <io.h>
  #include <windows.h>
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace desentry {

// -- socket handle --------------------------------------------------------
// Windows SOCKET is an unsigned pointer-width handle whose invalid value is
// ~0, not -1; the classic `int fd; if (fd < 0)` idiom is silently wrong
// there. A distinct type plus an explicit validity predicate makes the
// difference impossible to get wrong by accident.
#if defined(_WIN32)
using dsn_socket_t = UINT_PTR;
inline constexpr dsn_socket_t kInvalidSocket = static_cast<dsn_socket_t>(~0);
using dsn_socklen_t = int;
using dsn_iolen_t = int;   // send()/recv() take/return int on Windows
#else
using dsn_socket_t = int;
inline constexpr dsn_socket_t kInvalidSocket = -1;
using dsn_socklen_t = socklen_t;
using dsn_iolen_t = ssize_t;
#endif

inline bool SocketValid(dsn_socket_t s) { return s != kInvalidSocket; }

// Winsock needs an explicit process-wide startup before any socket call and
// is refcounted, so calling this from several components is safe. A no-op
// everywhere else. Idempotent; call it early in main() and in any library
// entry point that may be reached first.
void NetInit();
void NetShutdown();

void CloseSocket(dsn_socket_t s);
void ShutdownSocket(dsn_socket_t s);  // graceful RDWR shutdown, ignoring errors

// Blocking send/recv of exactly the requested length where possible;
// thin wrappers so callers don't have to spell the Windows int casts.
dsn_iolen_t SocketSend(dsn_socket_t s, const char* buf, size_t len);
dsn_iolen_t SocketRecv(dsn_socket_t s, char* buf, size_t len);

// Sets SO_RCVTIMEO/SO_SNDTIMEO portably (Windows takes a DWORD of
// milliseconds, POSIX a struct timeval).
void SetSocketTimeoutMs(dsn_socket_t s, int timeout_ms);

// setsockopt for a plain int option value. Windows types the option buffer
// as const char*, POSIX as const void*, so every call site would otherwise
// need its own cast.
void SetSockOptInt(dsn_socket_t s, int level, int optname, int value);

// Datagram send/recv wrappers (same int-vs-size_t reason as above).
dsn_iolen_t SocketSendTo(dsn_socket_t s, const char* buf, size_t len,
                          const struct sockaddr* dest, dsn_socklen_t dest_len);
dsn_iolen_t SocketRecvFrom(dsn_socket_t s, char* buf, size_t len,
                            struct sockaddr* from, dsn_socklen_t* from_len);

// True if the last socket error was a benign interruption worth retrying
// (EINTR / WSAEINTR) rather than a real failure.
bool SocketRetryable();

// Last socket error as a human-readable string (WSAGetLastError vs errno).
std::string SocketErrorString();

// A connected pair of sockets, for testing protocol code without a listener.
// POSIX has socketpair(); Windows has no AF_UNIX socketpair, so this performs
// a short loopback listen/connect/accept and hands back the two ends. Both
// are ordinary blocking stream sockets either way, so callers cannot tell
// which path produced them. Returns false and leaves `out` untouched on
// failure. Close both ends with CloseSocket().
bool SocketPair(dsn_socket_t out[2]);

// -- filesystem -----------------------------------------------------------
// Creates one directory. Returns true if it now exists (already-exists is
// success, matching every call site's intent).
bool MakeDir(const std::string& path);
// Creates every missing component of `path` (mkdir -p).
bool MakeDirs(const std::string& path);
bool PathExists(const std::string& path);
bool IsDirectory(const std::string& path);
// Restricts a file to owner-only read/write. chmod(0600) on POSIX; on
// Windows, strips inheritance and grants only the current user (see the
// implementation note -- this is a real ACL change, not a silent no-op).
bool RestrictToOwner(const std::string& path);
// Size in bytes, or 0 if unavailable.
uint64_t FileSize(const std::string& path);
// Non-recursive directory listing of entry names (no "." / "..").
std::vector<std::string> ListDir(const std::string& path);
// Recursive delete; returns true on success. Used only by tests and by the
// supervisor's reclaim path, never by the data plane.
bool RemoveTree(const std::string& path);

// Platform-appropriate per-user application data root, e.g.
//   Windows: %LOCALAPPDATA%\DeSentry
//   macOS:   ~/Library/Application Support/DeSentry
//   Linux:   $XDG_DATA_HOME/desentry or ~/.local/share/desentry
std::string AppDataDir();

// -- time / process --------------------------------------------------------
void SleepMs(uint32_t ms);
int64_t NowMs();      // wall clock, milliseconds since epoch
int64_t MonotonicMs();
std::tm LocalTime(std::time_t t);  // localtime_r / localtime_s wrapper

}  // namespace desentry
