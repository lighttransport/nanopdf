/*
 * lui_http.c — Minimal HTTP/1.1 server
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lui_http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HTTP_BUF_SIZE  (64 * 1024)  /* max request size */

struct lui_http_server {
    lui_socket_t       *listener;
    int                 port;
    lui_http_handler_fn handler;
    void               *handler_user;
};

/* ---- Helpers ------------------------------------------------------------ */

static const char *find_header_end(const char *buf, int len)
{
    /* Find \r\n\r\n */
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i;
    }
    return NULL;
}

static int parse_content_length(const char *headers, int hdr_len)
{
    const char *p = headers;
    const char *end = headers + hdr_len;
    while (p < end) {
        /* Case-insensitive search for "Content-Length:" */
        if ((end - p > 15) &&
            (p[0] == 'C' || p[0] == 'c') &&
            (p[7] == '-') &&
            (p[8] == 'L' || p[8] == 'l')) {
            /* Rough match, find the colon */
            const char *colon = memchr(p, ':', end - p);
            if (colon && (colon - p) < 20) {
                colon++;
                while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
                return atoi(colon);
            }
        }
        /* Skip to next line */
        const char *nl = memchr(p, '\n', end - p);
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static const char *find_header_value(const char *headers, int hdr_len,
                                      const char *name, int *out_len)
{
    int name_len = (int)strlen(name);
    const char *p = headers;
    const char *end = headers + hdr_len;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        int line_len = nl ? (int)(nl - p) : (int)(end - p);

        if (line_len > name_len + 1 && p[name_len] == ':') {
            /* Case-insensitive prefix compare */
            int match = 1;
            for (int i = 0; i < name_len; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = 0; break; }
            }
            if (match) {
                const char *val = p + name_len + 1;
                while (val < p + line_len && (*val == ' ' || *val == '\t')) val++;
                int vlen = (int)(p + line_len - val);
                if (vlen > 0 && val[vlen-1] == '\r') vlen--;
                if (out_len) *out_len = vlen;
                return val;
            }
        }

        if (!nl) break;
        p = nl + 1;
    }
    *out_len = 0;
    return NULL;
}

static int parse_request_line(char *buf, const char **method,
                               const char **path)
{
    /* "POST /mcp HTTP/1.1\r\n..." */
    char *sp1 = strchr(buf, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    *method = buf;

    char *path_start = sp1 + 1;
    char *sp2 = strchr(path_start, ' ');
    if (!sp2) return -1;
    *sp2 = '\0';
    *path = path_start;

    return 0;
}

static void send_response(lui_socket_t *client, int status,
                            const char *content_type,
                            const char *body, int body_len)
{
    const char *status_text = "OK";
    if (status == 400) status_text = "Bad Request";
    else if (status == 404) status_text = "Not Found";
    else if (status == 405) status_text = "Method Not Allowed";
    else if (status == 500) status_text = "Internal Server Error";

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    lui_net_send_all(client, header, hlen);
    if (body_len > 0)
        lui_net_send_all(client, body, body_len);
}

/* ---- Public API --------------------------------------------------------- */

lui_http_server_t *lui_http_server_create(const char *host, int port)
{
    if (lui_net_init() != 0) return NULL;

    lui_socket_t *listener = lui_net_listen(host, port);
    if (!listener) return NULL;

    lui_http_server_t *srv = (lui_http_server_t *)calloc(1, sizeof(*srv));
    if (!srv) { lui_net_close(listener); return NULL; }
    srv->listener = listener;
    srv->port = port;
    return srv;
}

void lui_http_server_set_handler(lui_http_server_t *srv,
                                  lui_http_handler_fn fn, void *user)
{
    if (!srv) return;
    srv->handler = fn;
    srv->handler_user = user;
}

int lui_http_server_poll(lui_http_server_t *srv)
{
    if (!srv || !srv->listener) return -1;

    /* Check for pending connections */
    if (lui_net_poll_read(srv->listener, 0) <= 0)
        return 0;

    lui_socket_t *client = lui_net_accept(srv->listener);
    if (!client) return 0;

    /* Read the full request (blocking on this connection, with timeout) */
    char *buf = (char *)malloc(HTTP_BUF_SIZE);
    if (!buf) { lui_net_close(client); return -1; }

    int total = 0;
    int body_expected = -1;
    const char *hdr_end = NULL;
    int hdr_len = 0;

    /* Read with a simple timeout loop */
    for (int attempts = 0; attempts < 100; attempts++) {
        if (lui_net_poll_read(client, 50) <= 0) {
            if (total > 0 && hdr_end) break;
            continue;
        }
        int n = lui_net_recv(client, buf + total, HTTP_BUF_SIZE - total - 1);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';

        if (!hdr_end) {
            hdr_end = find_header_end(buf, total);
            if (hdr_end) {
                hdr_len = (int)(hdr_end - buf);
                body_expected = parse_content_length(buf, hdr_len);
            }
        }

        if (hdr_end) {
            int body_start = hdr_len + 4; /* skip \r\n\r\n */
            int body_received = total - body_start;
            if (body_received >= body_expected) break;
        }
    }

    if (!hdr_end || total == 0) {
        free(buf);
        lui_net_close(client);
        return 0;
    }

    /* Parse request */
    const char *method, *path;
    if (parse_request_line(buf, &method, &path) != 0) {
        send_response(client, 400, "text/plain", "Bad Request", 11);
        free(buf);
        lui_net_close(client);
        return 0;
    }

    /* Handle CORS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client, 200, "text/plain", "", 0);
        free(buf);
        lui_net_close(client);
        return 1;
    }

    int body_start = hdr_len + 4;
    int body_len = total - body_start;
    if (body_len < 0) body_len = 0;

    /* Extract Content-Type from the headers (skip the request line).
     * parse_request_line wrote NULs over spaces in buf, but \r\n is intact.
     * Find the first \n to skip past the request line. */
    int ct_len = 0;
    const char *first_nl = (const char *)memchr(buf, '\n', hdr_len);
    const char *hdr_fields = first_nl ? first_nl + 1 : buf + hdr_len;
    int hdr_fields_len = (int)(hdr_end - hdr_fields);
    if (hdr_fields_len < 0) hdr_fields_len = 0;
    const char *ct = find_header_value(hdr_fields, hdr_fields_len,
                                        "Content-Type", &ct_len);
    /* Make a NUL-terminated copy of content-type if found */
    char ct_buf[128] = "";
    if (ct && ct_len > 0 && ct_len < (int)sizeof(ct_buf)) {
        memcpy(ct_buf, ct, ct_len);
        ct_buf[ct_len] = '\0';
    }

    lui_http_request_t req;
    req.method = method;
    req.path = path;
    req.body = buf + body_start;
    req.body_len = body_len;
    req.content_type = ct_buf[0] ? ct_buf : NULL;

    if (srv->handler) {
        int status = 200;
        char *resp_body = NULL;
        int resp_len = 0;
        const char *resp_ct = "application/json";

        srv->handler(&req, &status, &resp_body, &resp_len,
                      &resp_ct, srv->handler_user);

        send_response(client, status, resp_ct,
                       resp_body ? resp_body : "", resp_len);
        free(resp_body);
    } else {
        send_response(client, 404, "text/plain", "Not Found", 9);
    }

    free(buf);
    lui_net_close(client);
    return 1;
}

void lui_http_server_destroy(lui_http_server_t *srv)
{
    if (!srv) return;
    lui_net_close(srv->listener);
    free(srv);
}

int lui_http_server_port(const lui_http_server_t *srv)
{
    return srv ? srv->port : 0;
}
