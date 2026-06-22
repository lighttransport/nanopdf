/*
 * mcp_server.c — MCP JSON-RPC dispatch and transport handling
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/mcp.h>
#include <lightvg/surface.h>
#include <lightui/export.h>
#include "lui_json.h"
#include "lui_http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <io.h>
  #define STDIN_FD  0
  #define STDOUT_FD 1
#else
  #include <unistd.h>
  #include <sys/select.h>
  #include <fcntl.h>
  #define STDIN_FD  STDIN_FILENO
  #define STDOUT_FD STDOUT_FILENO
#endif

/* ---- Forward declarations for tools (mcp_tools.c) ----------------------- */

extern void lui_mcp_tools_list(lui_json_buf_t *out, lui_mcp_t *mcp);
extern void lui_mcp_tools_call(const char *name, const lui_json_t *args,
                                 lui_json_buf_t *out, lui_mcp_t *mcp);

/* ---- MCP server struct -------------------------------------------------- */

#define LUI_MCP_MAX_CUSTOM_TOOLS 32

typedef struct {
    char *name;
    char *description;
    char *input_schema;
    lui_mcp_tool_handler_fn handler;
    void *user_data;
} lui_mcp_custom_tool_t;

struct lui_mcp {
    lui_mcp_transport_t transport;

    /* Context (not owned) */
    void               *ui_ctx;       /* lui_ui_ctx_t* */
    void               *surface;      /* lvg_surface_t* */
    void               *theme;        /* lui_theme_t* */

    /* Stdio transport */
    char               *stdin_buf;
    int                 stdin_len;
    int                 stdin_cap;

    /* HTTP transport */
    lui_http_server_t  *http;
    int                 port;

    bool                initialized;

    /* Custom tool registry */
    lui_mcp_custom_tool_t custom_tools[LUI_MCP_MAX_CUSTOM_TOOLS];
    int                   num_custom_tools;

    /* Canvas debugging */
    void              *canvas;       /* lvg_canvas_t* (not owned)     */
    void              *font;         /* lui_font_t*   (not owned)     */
    void              *recorder;     /* lui_recorder_t* (owned)       */

    /* Named snapshots for diff_images */
    #define LUI_MCP_MAX_SNAPSHOTS 8
    struct {
        char              name[64];
        void             *surface;   /* lvg_surface_t* (owned)        */
    } snapshots[LUI_MCP_MAX_SNAPSHOTS];
    int num_snapshots;
};

/* ---- JSON-RPC helpers --------------------------------------------------- */

static void write_jsonrpc_header(lui_json_buf_t *b)
{
    lui_json_buf_key(b, "jsonrpc");
    lui_json_buf_string(b, "2.0");
}

static void write_result_with_id(lui_json_buf_t *b, const lui_json_t *id)
{
    lui_json_buf_object_begin(b);
    write_jsonrpc_header(b);
    lui_json_buf_key(b, "id");
    if (id) {
        if (lui_json_type(id) == LUI_JSON_STRING)
            lui_json_buf_string(b, lui_json_string(id));
        else
            lui_json_buf_int(b, lui_json_int(id));
    } else {
        lui_json_buf_null(b);
    }
}

static void write_error(lui_json_buf_t *b, const lui_json_t *id,
                          int code, const char *message)
{
    write_result_with_id(b, id);
    lui_json_buf_key(b, "error");
    lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "code");
        lui_json_buf_int(b, code);
        lui_json_buf_key(b, "message");
        lui_json_buf_string(b, message);
    lui_json_buf_object_end(b);
    lui_json_buf_object_end(b);
}

/* ---- Dispatch ----------------------------------------------------------- */

static char *mcp_dispatch(lui_mcp_t *mcp, const char *msg, int msg_len,
                            int *out_len)
{
    lui_json_t *root = lui_json_parse(msg, msg_len);
    if (!root) {
        lui_json_buf_t *b = lui_json_buf_new();
        write_error(b, NULL, -32700, "Parse error");
        return lui_json_buf_finish(b, out_len);
    }

    const lui_json_t *id = lui_json_get(root, "id");
    const char *method = lui_json_string(lui_json_get(root, "method"));

    if (!method) {
        lui_json_buf_t *b = lui_json_buf_new();
        write_error(b, id, -32600, "Invalid Request: missing method");
        lui_json_free(root);
        return lui_json_buf_finish(b, out_len);
    }

    lui_json_buf_t *b = lui_json_buf_new();

    /* ---- initialize ----------------------------------------------------- */
    if (strcmp(method, "initialize") == 0) {
        write_result_with_id(b, id);
        lui_json_buf_key(b, "result");
        lui_json_buf_object_begin(b);
            lui_json_buf_key(b, "protocolVersion");
            lui_json_buf_string(b, "2024-11-05");
            lui_json_buf_key(b, "capabilities");
            lui_json_buf_object_begin(b);
                lui_json_buf_key(b, "tools");
                lui_json_buf_object_begin(b);
                lui_json_buf_object_end(b);
            lui_json_buf_object_end(b);
            lui_json_buf_key(b, "serverInfo");
            lui_json_buf_object_begin(b);
                lui_json_buf_key(b, "name");
                lui_json_buf_string(b, "lightui-mcp");
                lui_json_buf_key(b, "version");
                lui_json_buf_string(b, "0.1.0");
            lui_json_buf_object_end(b);
        lui_json_buf_object_end(b);
        lui_json_buf_object_end(b);
        mcp->initialized = true;
    }
    /* ---- notifications/initialized -------------------------------------- */
    else if (strcmp(method, "notifications/initialized") == 0) {
        /* No response needed for notifications */
        lui_json_buf_free(b);
        lui_json_free(root);
        *out_len = 0;
        return NULL;
    }
    /* ---- tools/list ----------------------------------------------------- */
    else if (strcmp(method, "tools/list") == 0) {
        write_result_with_id(b, id);
        lui_json_buf_key(b, "result");
        lui_json_buf_object_begin(b);
            lui_json_buf_key(b, "tools");
            lui_json_buf_array_begin(b);
                lui_mcp_tools_list(b, mcp);
            lui_json_buf_array_end(b);
        lui_json_buf_object_end(b);
        lui_json_buf_object_end(b);
    }
    /* ---- tools/call ----------------------------------------------------- */
    else if (strcmp(method, "tools/call") == 0) {
        const lui_json_t *params = lui_json_get(root, "params");
        const char *tool_name = lui_json_string(lui_json_get(params, "name"));
        const lui_json_t *args = lui_json_get(params, "arguments");

        if (!tool_name) {
            write_error(b, id, -32602, "Missing tool name");
        } else {
            write_result_with_id(b, id);
            lui_json_buf_key(b, "result");
            lui_json_buf_object_begin(b);
                lui_json_buf_key(b, "content");
                lui_json_buf_array_begin(b);
                    lui_mcp_tools_call(tool_name, args, b, mcp);
                lui_json_buf_array_end(b);
            lui_json_buf_object_end(b);
            lui_json_buf_object_end(b);
        }
    }
    /* ---- ping ----------------------------------------------------------- */
    else if (strcmp(method, "ping") == 0) {
        write_result_with_id(b, id);
        lui_json_buf_key(b, "result");
        lui_json_buf_object_begin(b);
        lui_json_buf_object_end(b);
        lui_json_buf_object_end(b);
    }
    /* ---- unknown -------------------------------------------------------- */
    else {
        write_error(b, id, -32601, "Method not found");
    }

    lui_json_free(root);
    return lui_json_buf_finish(b, out_len);
}

/* ---- Stdio transport ---------------------------------------------------- */

static int stdin_has_data(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail > 0)
        return 1;
    return 0;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FD, &fds);
    struct timeval tv = {0, 0};
    return select(STDIN_FD + 1, &fds, NULL, NULL, &tv) > 0 ? 1 : 0;
#endif
}

static void stdio_send(const char *data, int len)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteFile(h, data, len, &written, NULL);
    WriteFile(h, "\n", 1, &written, NULL);
    FlushFileBuffers(h);
#else
    (void)!write(STDOUT_FD, data, len);
    (void)!write(STDOUT_FD, "\n", 1);
#endif
}

static int mcp_poll_stdio(lui_mcp_t *mcp)
{
    if (!stdin_has_data()) return 0;

    /* Read available data into buffer */
    char chunk[4096];
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD nr = 0;
    ReadFile(h, chunk, sizeof(chunk), &nr, NULL);
    int n = (int)nr;
#else
    int n = (int)read(STDIN_FD, chunk, sizeof(chunk));
#endif
    if (n <= 0) return 0;

    /* Append to line buffer */
    if (mcp->stdin_len + n + 1 > mcp->stdin_cap) {
        int new_cap = mcp->stdin_cap * 2;
        if (new_cap < mcp->stdin_len + n + 1) new_cap = mcp->stdin_len + n + 1;
        char *nb = (char *)realloc(mcp->stdin_buf, new_cap);
        if (!nb) return 0;
        mcp->stdin_buf = nb;
        mcp->stdin_cap = new_cap;
    }
    memcpy(mcp->stdin_buf + mcp->stdin_len, chunk, n);
    mcp->stdin_len += n;
    mcp->stdin_buf[mcp->stdin_len] = '\0';

    /* Process complete lines */
    int handled = 0;
    char *line_start = mcp->stdin_buf;
    char *newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        int line_len = (int)(newline - line_start);
        if (line_len > 0 && line_start[line_len - 1] == '\r')
            line_len--;

        if (line_len > 0) {
            int resp_len = 0;
            char *resp = mcp_dispatch(mcp, line_start, line_len, &resp_len);
            if (resp && resp_len > 0) {
                stdio_send(resp, resp_len);
                handled++;
            }
            free(resp);
        }

        line_start = newline + 1;
    }

    /* Compact buffer */
    int remaining = (int)(mcp->stdin_buf + mcp->stdin_len - line_start);
    if (remaining > 0)
        memmove(mcp->stdin_buf, line_start, remaining);
    mcp->stdin_len = remaining;

    return handled;
}

/* ---- HTTP transport ----------------------------------------------------- */

static void http_handler(const lui_http_request_t *req,
                           int *status, char **resp_body, int *resp_len,
                           const char **resp_ct, void *user)
{
    lui_mcp_t *mcp = (lui_mcp_t *)user;
    *resp_ct = "application/json";

    if (strcmp(req->method, "POST") != 0) {
        *status = 405;
        *resp_body = NULL;
        *resp_len = 0;
        return;
    }

    if (req->body_len <= 0) {
        *status = 400;
        *resp_body = NULL;
        *resp_len = 0;
        return;
    }

    *resp_body = mcp_dispatch(mcp, req->body, req->body_len, resp_len);
    *status = *resp_body ? 200 : 204;
}

static int mcp_poll_http(lui_mcp_t *mcp)
{
    if (!mcp->http) return 0;
    return lui_http_server_poll(mcp->http);
}

/* ---- Public API --------------------------------------------------------- */

lui_mcp_t *lui_mcp_create(const lui_mcp_config_t *config)
{
    if (!config) return NULL;

    lui_mcp_t *mcp = (lui_mcp_t *)calloc(1, sizeof(*mcp));
    if (!mcp) return NULL;

    mcp->transport = config->transport;

    if (config->transport == LUI_MCP_STDIO) {
        mcp->stdin_cap = 8192;
        mcp->stdin_buf = (char *)malloc(mcp->stdin_cap);
        if (!mcp->stdin_buf) { free(mcp); return NULL; }
        mcp->stdin_len = 0;

        /* Set stdin non-blocking on POSIX */
#ifndef _WIN32
        int flags = fcntl(STDIN_FD, F_GETFL, 0);
        if (flags >= 0) fcntl(STDIN_FD, F_SETFL, flags | O_NONBLOCK);
#endif
    } else if (config->transport == LUI_MCP_HTTP) {
        const char *host = config->host ? config->host : "127.0.0.1";
        int port = config->port > 0 ? config->port : 3001;

        mcp->http = lui_http_server_create(host, port);
        if (!mcp->http) { free(mcp); return NULL; }
        lui_http_server_set_handler(mcp->http, http_handler, mcp);
        mcp->port = port;

        fprintf(stderr, "lightui MCP server listening on http://%s:%d\n",
                host, port);
    }

    return mcp;
}

void lui_mcp_set_context(lui_mcp_t *mcp, void *ui_ctx,
                          struct lui_surface *surface)
{
    if (!mcp) return;
    mcp->ui_ctx = ui_ctx;
    mcp->surface = surface;
}

void lui_mcp_set_theme(lui_mcp_t *mcp, struct lui_theme *theme)
{
    if (!mcp) return;
    mcp->theme = theme;
}

void lui_mcp_set_canvas(lui_mcp_t *mcp, void *canvas)
{
    if (!mcp) return;
    mcp->canvas = canvas;
}

void lui_mcp_set_font(lui_mcp_t *mcp, void *font)
{
    if (!mcp) return;
    mcp->font = font;
}

void *lui_mcp_get_recorder(lui_mcp_t *mcp)
{
    if (!mcp) return NULL;
    return mcp->recorder;
}

int lui_mcp_poll(lui_mcp_t *mcp)
{
    if (!mcp) return 0;

    if (mcp->transport == LUI_MCP_STDIO)
        return mcp_poll_stdio(mcp);
    else if (mcp->transport == LUI_MCP_HTTP)
        return mcp_poll_http(mcp);

    return 0;
}

void lui_mcp_destroy(lui_mcp_t *mcp)
{
    if (!mcp) return;
    free(mcp->stdin_buf);
    if (mcp->http)
        lui_http_server_destroy(mcp->http);
    for (int i = 0; i < mcp->num_custom_tools; i++) {
        free(mcp->custom_tools[i].name);
        free(mcp->custom_tools[i].description);
        free(mcp->custom_tools[i].input_schema);
    }
    /* Clean up canvas debugging state */
    for (int i = 0; i < mcp->num_snapshots; i++) {
        lvg_surface_t *snap = (lvg_surface_t *)mcp->snapshots[i].surface;
        if (snap) lvg_surface_destroy(snap);
    }
    if (mcp->recorder) {
        lui_recorder_t *rec = (lui_recorder_t *)mcp->recorder;
        lui_recorder_destroy(rec);
    }
    free(mcp);
}

/* ---- Custom tool registration ------------------------------------------- */

static char *mcp_strdup(const char *s)
{
    if (!s) return NULL;
    int len = (int)strlen(s);
    char *d = (char *)malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

int lui_mcp_register_tool(lui_mcp_t *mcp,
    const char *name, const char *description,
    const char *input_schema,
    lui_mcp_tool_handler_fn handler, void *user_data)
{
    if (!mcp || !name || !handler) return -1;
    if (mcp->num_custom_tools >= LUI_MCP_MAX_CUSTOM_TOOLS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < mcp->num_custom_tools; i++) {
        if (strcmp(mcp->custom_tools[i].name, name) == 0)
            return -1;
    }

    lui_mcp_custom_tool_t *ct = &mcp->custom_tools[mcp->num_custom_tools];
    ct->name = mcp_strdup(name);
    ct->description = mcp_strdup(description ? description : "");
    ct->input_schema = mcp_strdup(input_schema ?
        input_schema : "{\"type\":\"object\",\"properties\":{}}");
    ct->handler = handler;
    ct->user_data = user_data;

    if (!ct->name || !ct->description || !ct->input_schema) {
        free(ct->name);
        free(ct->description);
        free(ct->input_schema);
        return -1;
    }

    mcp->num_custom_tools++;
    return 0;
}
