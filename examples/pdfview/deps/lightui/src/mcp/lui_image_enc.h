/*
 * lui_image_enc.h — Image encoding helpers for MCP screenshot tool
 *
 * Supports QOI, PNG, JPG, BMP output from ARGB pixel buffers.
 * QOI is implemented inline; PNG/JPG/BMP use stb_image_write.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUI_IMAGE_ENC_H
#define LUI_IMAGE_ENC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MCP-internal image format enum.  Avoid collision with the public
 * lui_img_enc_fmt_t defined in lightui/image.h (different enumerators). */
typedef enum {
    LUI_IMG_PPM,
    LUI_IMG_QOI,
    LUI_IMG_PNG,
    LUI_IMG_JPG,
    LUI_IMG_BMP,
} lui_img_enc_fmt_t;

/*
 * Encode an ARGB (0xAARRGGBB) pixel buffer to the given format.
 * Returns a malloc'd buffer with the encoded data.
 * Sets *out_len to the byte count. Returns NULL on failure.
 * The caller must free() the returned buffer.
 *
 * For JPG, quality is 1-100 (default 90 if <= 0).
 */
unsigned char *lui_image_encode(const uint32_t *pixels,
                                int width, int height, int stride,
                                lui_img_enc_fmt_t fmt, int jpg_quality,
                                int *out_len);

/*
 * Return the MIME type string for a given format.
 */
const char *lui_image_mime_type(lui_img_enc_fmt_t fmt);

/*
 * Parse a format name string. Returns -1 on unknown.
 */
int lui_image_format_from_name(const char *name);

/*
 * Base64 encode binary data.
 * Returns a malloc'd NUL-terminated string.
 * Sets *out_len to the string length if non-NULL.
 */
char *lui_base64_encode(const unsigned char *data, int len, int *out_len);

#ifdef __cplusplus
}
#endif

#endif /* LUI_IMAGE_ENC_H */
