/*
 * lui_image_enc.c — Image encoding: QOI (built-in), PNG/JPG/BMP (stb)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lui_image_enc.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- stb_image_write implementation ------------------------------------- */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

/* ---- QOI encoder (built-in) --------------------------------------------- */

/*
 * QOI — "Quite OK Image" format, see https://qoiformat.org/
 * Header: 14 bytes ("qoif" + w + h + channels + colorspace)
 * Ops: QOI_OP_RGB, QOI_OP_RGBA, QOI_OP_INDEX, QOI_OP_DIFF, QOI_OP_LUMA, QOI_OP_RUN
 * Footer: 7x 0x00 + 0x01
 */

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_DIFF   0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xC0 /* 11xxxxxx */
#define QOI_OP_RGB    0xFE
#define QOI_OP_RGBA   0xFF

#define QOI_HASH(r, g, b, a) (((r)*3 + (g)*5 + (b)*7 + (a)*11) & 63)

static void qoi_write32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >>  8) & 0xFF);
    p[3] = (unsigned char)( v        & 0xFF);
}

static unsigned char *qoi_encode(const unsigned char *rgba, int w, int h,
                                  int channels, int *out_len)
{
    int px_count = w * h;
    /* Worst case: header(14) + all QOI_OP_RGBA(5 per pixel) + footer(8) */
    int max_size = 14 + px_count * 5 + 8;
    unsigned char *out = (unsigned char *)malloc(max_size);
    if (!out) return NULL;

    int p = 0;

    /* Header */
    out[p++] = 'q'; out[p++] = 'o'; out[p++] = 'i'; out[p++] = 'f';
    qoi_write32(out + p, (uint32_t)w); p += 4;
    qoi_write32(out + p, (uint32_t)h); p += 4;
    out[p++] = (unsigned char)channels;
    out[p++] = 0; /* sRGB colorspace */

    unsigned char index[64][4];
    memset(index, 0, sizeof(index));

    unsigned char prev_r = 0, prev_g = 0, prev_b = 0, prev_a = 255;
    int run = 0;

    for (int i = 0; i < px_count; i++) {
        unsigned char r = rgba[i * channels + 0];
        unsigned char g = rgba[i * channels + 1];
        unsigned char b = rgba[i * channels + 2];
        unsigned char a = (channels >= 4) ? rgba[i * channels + 3] : 255;

        if (r == prev_r && g == prev_g && b == prev_b && a == prev_a) {
            run++;
            if (run == 62 || i == px_count - 1) {
                out[p++] = (unsigned char)(QOI_OP_RUN | (run - 1));
                run = 0;
            }
        } else {
            if (run > 0) {
                out[p++] = (unsigned char)(QOI_OP_RUN | (run - 1));
                run = 0;
            }

            int hash = QOI_HASH(r, g, b, a);

            if (index[hash][0] == r && index[hash][1] == g &&
                index[hash][2] == b && index[hash][3] == a) {
                out[p++] = (unsigned char)(QOI_OP_INDEX | hash);
            } else {
                index[hash][0] = r;
                index[hash][1] = g;
                index[hash][2] = b;
                index[hash][3] = a;

                if (a == prev_a) {
                    int dr = (int)r - (int)prev_r;
                    int dg = (int)g - (int)prev_g;
                    int db = (int)b - (int)prev_b;

                    int dr_dg = dr - dg;
                    int db_dg = db - dg;

                    if (dr >= -2 && dr <= 1 &&
                        dg >= -2 && dg <= 1 &&
                        db >= -2 && db <= 1) {
                        out[p++] = (unsigned char)(QOI_OP_DIFF |
                            ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
                    } else if (dg >= -32 && dg <= 31 &&
                               dr_dg >= -8 && dr_dg <= 7 &&
                               db_dg >= -8 && db_dg <= 7) {
                        out[p++] = (unsigned char)(QOI_OP_LUMA | (dg + 32));
                        out[p++] = (unsigned char)(((dr_dg + 8) << 4) | (db_dg + 8));
                    } else {
                        out[p++] = QOI_OP_RGB;
                        out[p++] = r;
                        out[p++] = g;
                        out[p++] = b;
                    }
                } else {
                    out[p++] = QOI_OP_RGBA;
                    out[p++] = r;
                    out[p++] = g;
                    out[p++] = b;
                    out[p++] = a;
                }
            }
        }

        prev_r = r; prev_g = g; prev_b = b; prev_a = a;
    }

    /* Footer: 7x 0x00, 1x 0x01 */
    for (int i = 0; i < 7; i++) out[p++] = 0x00;
    out[p++] = 0x01;

    *out_len = p;
    return out;
}

/* ---- stb callback for writing to memory buffer -------------------------- */

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} mem_buf_t;

static void stb_write_cb(void *context, void *data, int size)
{
    mem_buf_t *buf = (mem_buf_t *)context;
    if (size <= 0) return;
    size_t need = buf->len + (size_t)size;
    if (need < buf->len) return;  /* size_t overflow guard */
    if (need > buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap < need) new_cap = need;
        unsigned char *nb = (unsigned char *)realloc(buf->data, new_cap);
        if (!nb) return;
        buf->data = nb;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, data, (size_t)size);
    buf->len += (size_t)size;
}

/* ---- Convert ARGB to RGBA for encoders ---------------------------------- */

static unsigned char *argb_to_rgba(const uint32_t *pixels,
                                     int w, int h, int stride)
{
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!rgba) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = pixels[(size_t)y * (size_t)stride + (size_t)x];
            size_t off = ((size_t)y * (size_t)w + (size_t)x) * 4;
            rgba[off + 0] = (unsigned char)((px >> 16) & 0xFF); /* R */
            rgba[off + 1] = (unsigned char)((px >>  8) & 0xFF); /* G */
            rgba[off + 2] = (unsigned char)((px      ) & 0xFF); /* B */
            rgba[off + 3] = (unsigned char)((px >> 24) & 0xFF); /* A */
        }
    }
    return rgba;
}

static unsigned char *argb_to_rgb(const uint32_t *pixels,
                                    int w, int h, int stride)
{
    unsigned char *rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = pixels[(size_t)y * (size_t)stride + (size_t)x];
            size_t off = ((size_t)y * (size_t)w + (size_t)x) * 3;
            rgb[off + 0] = (unsigned char)((px >> 16) & 0xFF);
            rgb[off + 1] = (unsigned char)((px >>  8) & 0xFF);
            rgb[off + 2] = (unsigned char)((px      ) & 0xFF);
        }
    }
    return rgb;
}

/* ---- PPM encoder -------------------------------------------------------- */

static unsigned char *encode_ppm(const uint32_t *pixels,
                                   int w, int h, int stride, int *out_len)
{
    const size_t hdr_max = 64;
    size_t size = hdr_max + (size_t)w * (size_t)h * 3;
    unsigned char *ppm = (unsigned char *)malloc(size);
    if (!ppm) return NULL;

    int hdr_len = snprintf((char *)ppm, hdr_max, "P6\n%d %d\n255\n", w, h);
    size_t p = (size_t)hdr_len;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = pixels[(size_t)y * (size_t)stride + (size_t)x];
            ppm[p++] = (unsigned char)((px >> 16) & 0xFF);
            ppm[p++] = (unsigned char)((px >>  8) & 0xFF);
            ppm[p++] = (unsigned char)((px      ) & 0xFF);
        }
    }

    *out_len = (int)p;
    return ppm;
}

/* ---- Public API --------------------------------------------------------- */

unsigned char *lui_image_encode(const uint32_t *pixels,
                                int width, int height, int stride,
                                lui_img_enc_fmt_t fmt, int jpg_quality,
                                int *out_len)
{
    if (!pixels || width <= 0 || height <= 0 || !out_len) return NULL;
    /* Reject dimensions whose pixel count * 4 bytes would overflow size_t.
     * INT_MAX also caps *out_len below; mirror the surface.c:53 guard. */
    if ((uint64_t)width * (uint64_t)height > (uint64_t)(SIZE_MAX / 4))
        return NULL;

    *out_len = 0;

    switch (fmt) {
    case LUI_IMG_PPM:
        return encode_ppm(pixels, width, height, stride, out_len);

    case LUI_IMG_QOI: {
        unsigned char *rgba = argb_to_rgba(pixels, width, height, stride);
        if (!rgba) return NULL;
        unsigned char *enc = qoi_encode(rgba, width, height, 4, out_len);
        free(rgba);
        return enc;
    }

    case LUI_IMG_PNG: {
        unsigned char *rgba = argb_to_rgba(pixels, width, height, stride);
        if (!rgba) return NULL;
        mem_buf_t buf = {NULL, 0, 0};
        buf.cap = (size_t)width * (size_t)height * 4;
        buf.data = (unsigned char *)malloc(buf.cap);
        if (!buf.data) { free(rgba); return NULL; }
        int ok = stbi_write_png_to_func(stb_write_cb, &buf,
                                         width, height, 4, rgba,
                                         width * 4);
        free(rgba);
        if (!ok || buf.len > (size_t)INT_MAX) { free(buf.data); return NULL; }
        *out_len = (int)buf.len;
        return buf.data;
    }

    case LUI_IMG_JPG: {
        if (jpg_quality <= 0) jpg_quality = 90;
        unsigned char *rgb = argb_to_rgb(pixels, width, height, stride);
        if (!rgb) return NULL;
        mem_buf_t buf = {NULL, 0, 0};
        buf.cap = (size_t)width * (size_t)height * 3;
        buf.data = (unsigned char *)malloc(buf.cap);
        if (!buf.data) { free(rgb); return NULL; }
        int ok = stbi_write_jpg_to_func(stb_write_cb, &buf,
                                         width, height, 3, rgb,
                                         jpg_quality);
        free(rgb);
        if (!ok || buf.len > (size_t)INT_MAX) { free(buf.data); return NULL; }
        *out_len = (int)buf.len;
        return buf.data;
    }

    case LUI_IMG_BMP: {
        unsigned char *rgba = argb_to_rgba(pixels, width, height, stride);
        if (!rgba) return NULL;
        mem_buf_t buf = {NULL, 0, 0};
        buf.cap = 54 + (size_t)width * (size_t)height * 4;
        buf.data = (unsigned char *)malloc(buf.cap);
        if (!buf.data) { free(rgba); return NULL; }
        int ok = stbi_write_bmp_to_func(stb_write_cb, &buf,
                                         width, height, 4, rgba);
        free(rgba);
        if (!ok || buf.len > (size_t)INT_MAX) { free(buf.data); return NULL; }
        *out_len = (int)buf.len;
        return buf.data;
    }
    }

    return NULL;
}

const char *lui_image_mime_type(lui_img_enc_fmt_t fmt)
{
    switch (fmt) {
    case LUI_IMG_PPM: return "image/x-portable-pixmap";
    case LUI_IMG_QOI: return "image/qoi";
    case LUI_IMG_PNG: return "image/png";
    case LUI_IMG_JPG: return "image/jpeg";
    case LUI_IMG_BMP: return "image/bmp";
    }
    return "application/octet-stream";
}

int lui_image_format_from_name(const char *name)
{
    if (!name) return LUI_IMG_PNG; /* default */
    if (strcmp(name, "ppm") == 0)  return LUI_IMG_PPM;
    if (strcmp(name, "qoi") == 0)  return LUI_IMG_QOI;
    if (strcmp(name, "png") == 0)  return LUI_IMG_PNG;
    if (strcmp(name, "jpg") == 0 || strcmp(name, "jpeg") == 0) return LUI_IMG_JPG;
    if (strcmp(name, "bmp") == 0)  return LUI_IMG_BMP;
    return -1;
}

/* ---- Base64 encoder ----------------------------------------------------- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *lui_base64_encode(const unsigned char *data, int len, int *out_len)
{
    int out_cap = ((len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(out_cap);
    if (!out) return NULL;

    int j = 0;
    for (int i = 0; i < len; ) {
        unsigned int a = (i < len) ? data[i++] : 0;
        unsigned int b = (i < len) ? data[i++] : 0;
        unsigned int c = (i < len) ? data[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;

        int remaining = len - (i - 3);
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (remaining > 1) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (remaining > 2) ? b64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}
