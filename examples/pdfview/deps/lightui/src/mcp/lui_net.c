/*
 * lui_net.c — Portable socket implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lui_net.h"

#include <stdlib.h>
#include <string.h>

/* ---- Platform includes -------------------------------------------------- */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  typedef SOCKET sock_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define SOCK_ERR     SOCKET_ERROR
  #define CLOSE_SOCK   closesocket

  static void set_nonblocking(sock_t s)
  {
      u_long mode = 1;
      ioctlsocket(s, FIONBIO, &mode);
  }
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <sys/select.h>

  typedef int sock_t;
  #define INVALID_SOCK (-1)
  #define SOCK_ERR     (-1)
  #define CLOSE_SOCK   close

  static void set_nonblocking(sock_t s)
  {
      int flags = fcntl(s, F_GETFL, 0);
      if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
  }
#endif

/* ---- Socket struct ------------------------------------------------------ */

struct lui_socket {
    sock_t fd;
};

/* ---- Init / Cleanup ----------------------------------------------------- */

int lui_net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void lui_net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ---- Listen ------------------------------------------------------------- */

lui_socket_t *lui_net_listen(const char *host, int port)
{
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) return NULL;

    /* Allow address reuse */
    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (!host || host[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        addr.sin_addr.s_addr = inet_addr(host);
        if (addr.sin_addr.s_addr == INADDR_NONE)
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        CLOSE_SOCK(fd);
        return NULL;
    }

    if (listen(fd, 8) == SOCK_ERR) {
        CLOSE_SOCK(fd);
        return NULL;
    }

    set_nonblocking(fd);

    lui_socket_t *sock = (lui_socket_t *)malloc(sizeof(*sock));
    if (!sock) { CLOSE_SOCK(fd); return NULL; }
    sock->fd = fd;
    return sock;
}

/* ---- Accept ------------------------------------------------------------- */

lui_socket_t *lui_net_accept(lui_socket_t *listener)
{
    if (!listener) return NULL;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

#ifdef _WIN32
    sock_t client = accept(listener->fd, (struct sockaddr *)&addr, (int *)&addrlen);
#else
    sock_t client = accept(listener->fd, (struct sockaddr *)&addr, &addrlen);
#endif

    if (client == INVALID_SOCK) return NULL;

    lui_socket_t *sock = (lui_socket_t *)malloc(sizeof(*sock));
    if (!sock) { CLOSE_SOCK(client); return NULL; }
    sock->fd = client;
    return sock;
}

/* ---- Recv / Send -------------------------------------------------------- */

int lui_net_recv(lui_socket_t *sock, char *buf, int len)
{
    if (!sock) return -1;
    int n = (int)recv(sock->fd, buf, len, 0);
    if (n > 0) return n;
    if (n == 0) return 0; /* disconnect */
#ifdef _WIN32
    if (WSAGetLastError() == WSAEWOULDBLOCK) return -1;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
#endif
    return -1;
}

int lui_net_send(lui_socket_t *sock, const char *buf, int len)
{
    if (!sock) return -1;
#ifdef _WIN32
    return (int)send(sock->fd, buf, len, 0);
#else
    return (int)send(sock->fd, buf, len, MSG_NOSIGNAL);
#endif
}

int lui_net_send_all(lui_socket_t *sock, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = lui_net_send(sock, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ---- Close -------------------------------------------------------------- */

void lui_net_close(lui_socket_t *sock)
{
    if (!sock) return;
    CLOSE_SOCK(sock->fd);
    free(sock);
}

/* ---- Poll --------------------------------------------------------------- */

int lui_net_poll_read(lui_socket_t *sock, int timeout_ms)
{
    if (!sock) return -1;

    fd_set fds;
    FD_ZERO(&fds);

#ifdef _WIN32
    FD_SET(sock->fd, &fds);
#else
    /* Suppress warning for large fd values */
    if (sock->fd >= FD_SETSIZE) return -1;
    FD_SET(sock->fd, &fds);
#endif

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select((int)sock->fd + 1, &fds, NULL, NULL, &tv);
    if (ret > 0) return 1;
    if (ret == 0) return 0;
    return -1;
}
