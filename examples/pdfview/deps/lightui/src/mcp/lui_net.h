/*
 * lui_net.h — Portable socket abstraction (POSIX + Win32 Winsock2)
 *
 * Provides a thin wrapper over BSD sockets / Winsock2 for TCP connections.
 * All functions are non-blocking where noted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUI_NET_H
#define LUI_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque socket handle */
typedef struct lui_socket lui_socket_t;

/** Initialise networking (WSAStartup on Windows, no-op elsewhere). Returns 0 on success. */
int lui_net_init(void);

/** Cleanup networking (WSACleanup on Windows, no-op elsewhere). */
void lui_net_cleanup(void);

/** Create a TCP listening socket bound to host:port. Returns NULL on failure. */
lui_socket_t *lui_net_listen(const char *host, int port);

/**
 * Accept a pending connection (non-blocking).
 * Returns NULL if no connection is pending or on error.
 */
lui_socket_t *lui_net_accept(lui_socket_t *listener);

/** Receive up to @len bytes. Returns bytes read, 0 on disconnect, -1 on would-block/error. */
int lui_net_recv(lui_socket_t *sock, char *buf, int len);

/** Send @len bytes. Returns bytes sent, or -1 on error. */
int lui_net_send(lui_socket_t *sock, const char *buf, int len);

/** Send all @len bytes (blocks until complete). Returns 0 on success, -1 on error. */
int lui_net_send_all(lui_socket_t *sock, const char *buf, int len);

/** Close and free a socket. */
void lui_net_close(lui_socket_t *sock);

/**
 * Check if data is available for reading (non-blocking).
 * Returns 1 if readable, 0 if not, -1 on error.
 * @timeout_ms: 0 for immediate check, >0 to wait.
 */
int lui_net_poll_read(lui_socket_t *sock, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* LUI_NET_H */
