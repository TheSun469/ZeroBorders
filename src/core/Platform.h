#pragma once

// 跨平台套接字与系统类型定义。
// Windows：SOCKET 是无符号指针类型，INVALID_SOCKET 为 ~0，SOCKET_ERROR 为 -1。
// Linux：  socket 是 int，无效值为 -1。
// 此头文件统一封装，消除散落在各源文件中的 #ifdef _WIN32 SOCKET 判断。

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  ifndef SOCKET_ERROR_VALUE
#    define SOCKET_ERROR_VALUE SOCKET_ERROR
#  endif
#  ifndef SOCKET_ERROR
#    define SOCKET_ERROR SOCKET_ERROR_VALUE
#  endif
   // closesocket 在 Windows 上是专用 API，Linux 上为 close()，已在各 .cpp 内定义
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <errno.h>
   using socket_t = int;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR_VALUE (-1)
#  define SOCKET_ERROR (-1)
#  define closesocket ::close
   // WinSock 错误 API 映射到 POSIX errno
#  define WSAGetLastError() (errno)
#  define WSAEWOULDBLOCK EWOULDBLOCK
#  define WSAEINPROGRESS EINPROGRESS
#  define WSAEINTR EINTR
   // Windows 类型别名（实现层使用）
   typedef int BOOL;
   typedef unsigned long DWORD;
   typedef unsigned long ULONG;
#  ifndef TRUE
#    define TRUE 1
#  endif
#  ifndef FALSE
#    define FALSE 0
#  endif
#endif

namespace zb {

// 跨平台获取上次系统错误码（Windows: WSAGetLastError, Linux: errno）
inline int getLastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

} // namespace zb
