/*
 * lui_http.h — Minimal HTTP/1.1 server (single-threaded, non-blocking)
 *
 * Designed for MCP's Streamable HTTP transport: accepts POST requests
 * with JSON bodies and returns JSON responses.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUI_HTTP_H
#define LUI_HTTP_H

#include "lui_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *method;     /* "GET", "POST", etc. (points into recv buffer) */
    const char *path;       /* "/mcp" etc. (points into recv buffer)         */
    const char *body;       /* request body (points into recv buffer)         */
    int         body_len;
    const char *content_type; /* Content-Type header value (or NULL)          */
} lui_http_request_t;

/**
 * Handler callback. Receives a parsed request and must provide a response.
 *
 * Set *status to the HTTP status code (e.g. 200).
 * Set *resp_body to a heap-allocated response body (caller will free).
 * Set *resp_len to the body length.
 * Set *resp_content_type to the Content-Type (static string, not freed).
 */
typedef void (*lui_http_handler_fn)(
    const lui_http_request_t *req,
    int    *status,
    char  **resp_body,
    int    *resp_len,
    const char **resp_content_type,
    void   *user);

typedef struct lui_http_server lui_http_server_t;

/** Create an HTTP server listening on host:port. Returns NULL on failure. */
lui_http_server_t *lui_http_server_create(const char *host, int port);

/** Set the request handler. */
void lui_http_server_set_handler(lui_http_server_t *srv,
                                  lui_http_handler_fn fn, void *user);

/**
 * Poll for incoming connections and handle them (non-blocking).
 * Returns the number of requests handled, or -1 on error.
 */
int lui_http_server_poll(lui_http_server_t *srv);

/** Destroy the server and close the listening socket. */
void lui_http_server_destroy(lui_http_server_t *srv);

/** Get the port the server is listening on. */
int lui_http_server_port(const lui_http_server_t *srv);

#ifdef __cplusplus
}
#endif

#endif /* LUI_HTTP_H */
