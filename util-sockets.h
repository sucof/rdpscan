/*

    A wrapper around the Sockets API to deal with
    Windows/POSIX differences
*/
#ifndef UTIL_SOCKETS_H
#define UTIL_SOCKETS_H
#include <stdio.h>

#ifdef WIN32
#if defined(_MSC_VER)
#define WIN32_LEAN_AND_MEAN 
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "libssl.lib")
#pragma comment(lib, "libcrypto.lib")
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <intrin.h>
#include <process.h>

#define snprintf _snprintf
#define $close(fd) closesocket(fd)
typedef int ssize_t;
#define $errno WSAGetLastError()

extern const char *$strerror(int err);

#define $EWOULDBLOCK    WSAEWOULDBLOCK
#define $ECONNRESET     WSAECONNRESET
#define $EINTR          WSAEINTR
#define $EINPROGRESS    WSAEINPROGRESS
#define $ETIMEDOUT      WSAETIMEDOUT
#define $ECONNABORTED   WSAECONNABORTED
#define $ECONNREFUSED   WSAECONNREFUSED

#else
#include <unistd.h>		/* select read write close */
#include <sys/socket.h>		/* socket connect setsockopt */
#include <sys/time.h>		/* timeval */
#include <netdb.h>		/* gethostbyname */
#include <netinet/in.h>		/* sockaddr_in */
#include <netinet/tcp.h>	/* TCP_NODELAY */
#include <arpa/inet.h>		/* inet_addr */
#include <fcntl.h>
#include <errno.h>
#define $EWOULDBLOCK    EWOULDBLOCK
#define $ECONNRESET     ECONNRESET
#define $EINTR          EINTR
#define $EINPROGRESS    EINPROGRESS
#define $ETIMEDOUT      ETIMEDOUT
#define $ECONNABORTED   ECONNABORTED
#define $ECONNREFUSED   ECONNREFUSED
#define $strerror(err)  strerror(err)
#define $errno          errno
#define $close(fd)      close(fd)
#endif

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long) -1)
#endif


#endif
