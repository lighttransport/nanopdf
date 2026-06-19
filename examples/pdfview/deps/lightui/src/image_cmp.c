/*
 * src/image_cmp.c — Pixel-level image comparison (RMSE, PSNR, SSIM)
 *
 * Single-pass per-pixel statistics + 8x8 sliding-window SSIM on luminance.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <lightui/image_cmp.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ------------------------------------------------------------ */

static inline int iabs(int x) { return x < 0 ? -x : x; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/*
 * Extract a channel value from a pixel, according to the channel mode.
 * For LUI_CMP_RGB, returns the average of R, G, B (not used for per-pixel,
 * but for SSIM luminance fallback).
 */
static inline double pixel_channel(uint32_t px, lui_cmp_channel_t ch)
{
    uint8_t r = (uint8_t)((px >> 16) & 0xFF);
    uint8_t g = (uint8_t)((px >>  8) & 0xFF);
    uint8_t b = (uint8_t)( px        & 0xFF);
    uint8_t a = (uint8_t)((px >> 24) & 0xFF);

    switch (ch) {
    case LUI_CMP_RED:       return (double)r;
    case LUI_CMP_GREEN:     return (double)g;
    case LUI_CMP_BLUE:      return (double)b;
    case LUI_CMP_ALPHA:     return (double)a;
    case LUI_CMP_LUMINANCE: return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    case LUI_CMP_RGB:
    default:                return (r + g + b) / 3.0;
    }
}

/* ---- SSIM on luminance with 8x8 sliding window ------------------------- */

/*
 * SSIM constants (Wang et al., 2004):
 *   C1 = (K1*L)^2, C2 = (K2*L)^2
 *   where K1=0.01, K2=0.03, L=255 (8-bit dynamic range)
 */
#define SSIM_C1  (0.01 * 0.01 * 255.0 * 255.0)  /* 6.5025 */
#define SSIM_C2  (0.03 * 0.03 * 255.0 * 255.0)  /* 58.5225 */
#define SSIM_WIN 8

static double compute_ssim(const lvg_surface_t *a, const lvg_surface_t *b)
{
    int w = a->width;
    int h = a->height;

    /* If the image is smaller than the window, treat as a single window */
    int win_w = w < SSIM_WIN ? w : SSIM_WIN;
    int win_h = h < SSIM_WIN ? h : SSIM_WIN;

    int steps_x = w - win_w + 1;
    int steps_y = h - win_h + 1;

    if (steps_x <= 0) steps_x = 1;
    if (steps_y <= 0) steps_y = 1;

    double ssim_sum = 0.0;
    int    window_count = 0;

    for (int wy = 0; wy < steps_y; wy++) {
        for (int wx = 0; wx < steps_x; wx++) {
            double sum_a = 0, sum_b = 0;
            double sum_a2 = 0, sum_b2 = 0, sum_ab = 0;
            int n = 0;

            for (int dy = 0; dy < win_h; dy++) {
                int y = wy + dy;
                if (y >= h) break;
                for (int dx = 0; dx < win_w; dx++) {
                    int x = wx + dx;
                    if (x >= w) break;

                    double va = pixel_channel(
                        a->pixels[y * a->stride + x], LUI_CMP_LUMINANCE);
                    double vb = pixel_channel(
                        b->pixels[y * b->stride + x], LUI_CMP_LUMINANCE);

                    sum_a  += va;
                    sum_b  += vb;
                    sum_a2 += va * va;
                    sum_b2 += vb * vb;
                    sum_ab += va * vb;
                    n++;
                }
            }

            if (n == 0) continue;

            double mu_a = sum_a / n;
            double mu_b = sum_b / n;
            double sigma_a2 = sum_a2 / n - mu_a * mu_a;
            double sigma_b2 = sum_b2 / n - mu_b * mu_b;
            double sigma_ab = sum_ab / n - mu_a * mu_b;

            /* Clamp tiny negative variances from floating-point error */
            if (sigma_a2 < 0) sigma_a2 = 0;
            if (sigma_b2 < 0) sigma_b2 = 0;

            double num = (2.0 * mu_a * mu_b + SSIM_C1) *
                         (2.0 * sigma_ab + SSIM_C2);
            double den = (mu_a * mu_a + mu_b * mu_b + SSIM_C1) *
                         (sigma_a2 + sigma_b2 + SSIM_C2);

            ssim_sum += num / den;
            window_count++;
        }
    }

    return window_count > 0 ? ssim_sum / window_count : 1.0;
}

/* ---- Main comparison ---------------------------------------------------- */

int lui_image_cmp(const lvg_surface_t *a, const lvg_surface_t *b,
                  lui_cmp_channel_t channel, lui_cmp_result_t *result)
{
    if (!a || !b || !result) return -1;
    if (a->width != b->width || a->height != b->height) return -1;

    memset(result, 0, sizeof(*result));

    int w = a->width;
    int h = a->height;
    int total = w * h;

    result->total_pixels = total;

    if (total == 0) {
        result->ssim = 1.0;
        result->psnr_db = INFINITY;
        return 0;
    }

    int    diff_count = 0;
    int    max_diff = 0;
    double sum_abs = 0.0;
    double sum_sq  = 0.0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t pa = a->pixels[y * a->stride + x];
            uint32_t pb = b->pixels[y * b->stride + x];

            if (channel == LUI_CMP_RGB) {
                /* Per-channel R, G, B */
                int ra = (int)((pa >> 16) & 0xFF), rb = (int)((pb >> 16) & 0xFF);
                int ga = (int)((pa >>  8) & 0xFF), gb = (int)((pb >>  8) & 0xFF);
                int ba = (int)( pa        & 0xFF), bb = (int)( pb        & 0xFF);

                int dr = iabs(ra - rb);
                int dg = iabs(ga - gb);
                int db = iabs(ba - bb);

                int md = imax(dr, imax(dg, db));
                if (md > 0) diff_count++;
                if (md > max_diff) max_diff = md;

                /* Average absolute diff across 3 channels */
                double ad = (dr + dg + db) / 3.0;
                sum_abs += ad;

                /* MSE across 3 channels */
                sum_sq += ((double)dr * dr + (double)dg * dg + (double)db * db) / 3.0;
            } else {
                double va = pixel_channel(pa, channel);
                double vb = pixel_channel(pb, channel);
                double d = va - vb;
                double ad = d < 0 ? -d : d;
                int di = (int)(ad + 0.5);

                if (di > 0) diff_count++;
                if (di > max_diff) max_diff = di;

                sum_abs += ad;
                sum_sq  += d * d;
            }
        }
    }

    result->diff_pixels     = diff_count;
    result->max_channel_diff = max_diff;
    result->mean_diff       = sum_abs / total;

    double mse = sum_sq / total;
    result->rmse = sqrt(mse);

    if (mse < 1e-10) {
        result->psnr_db = INFINITY;
    } else {
        result->psnr_db = 10.0 * log10(255.0 * 255.0 / mse);
    }

    /* SSIM always computed on luminance */
    result->ssim = compute_ssim(a, b);

    return 0;
}

/* ---- Threshold check ---------------------------------------------------- */

int lui_image_cmp_passes(const lui_cmp_result_t *result,
                         double min_ssim, int max_diff_pix)
{
    if (!result) return 0;
    if (result->ssim < min_ssim) return 0;
    if (result->diff_pixels > max_diff_pix) return 0;
    return 1;
}

/* ---- Diff surface ------------------------------------------------------- */

lvg_surface_t *lui_image_cmp_diff_surface(const lvg_surface_t *a,
                                           const lvg_surface_t *b)
{
    if (!a || !b) return NULL;
    if (a->width != b->width || a->height != b->height) return NULL;

    int w = a->width;
    int h = a->height;

    lvg_surface_t *diff = lvg_surface_create(w, h);
    if (!diff) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t pa = a->pixels[y * a->stride + x];
            uint32_t pb = b->pixels[y * b->stride + x];

            int ra = (int)((pa >> 16) & 0xFF), rb = (int)((pb >> 16) & 0xFF);
            int ga = (int)((pa >>  8) & 0xFF), gb = (int)((pb >>  8) & 0xFF);
            int ba = (int)( pa        & 0xFF), bb = (int)( pb        & 0xFF);

            int dr = iabs(ra - rb);
            int dg = iabs(ga - gb);
            int db = iabs(ba - bb);
            int md = imax(dr, imax(dg, db));

            uint32_t out;
            if (md == 0) {
                /* Identical: dim to 20% */
                uint8_t r = (uint8_t)(((ra + rb) / 2) / 5);
                uint8_t g = (uint8_t)(((ga + gb) / 2) / 5);
                uint8_t b2 = (uint8_t)(((ba + bb) / 2) / 5);
                out = (0xFFu << 24) | ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) | b2;
            } else {
                /* Different: red with intensity proportional to error */
                int intensity = md * 4;
                if (intensity > 255) intensity = 255;
                out = (0xFFu << 24) | (0xFFu << 16) | (uint32_t)intensity;
            }

            diff->pixels[y * diff->stride + x] = out;
        }
    }

    return diff;
}

/* ---- Side-by-side composite --------------------------------------------- */

lvg_surface_t *lui_image_cmp_side_by_side(const lvg_surface_t *a,
                                           const lvg_surface_t *b)
{
    if (!a || !b) return NULL;
    if (a->width != b->width || a->height != b->height) return NULL;

    int w = a->width;
    int h = a->height;

    lvg_surface_t *out = lvg_surface_create(w * 3, h);
    if (!out) return NULL;

    lvg_surface_t *diff = lui_image_cmp_diff_surface(a, b);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Panel A */
            out->pixels[y * out->stride + x] =
                a->pixels[y * a->stride + x];

            /* Panel diff (center) */
            uint32_t dp;
            if (diff) {
                dp = diff->pixels[y * diff->stride + x];
            } else {
                dp = 0xFF000000u; /* black fallback */
            }
            out->pixels[y * out->stride + w + x] = dp;

            /* Panel B */
            out->pixels[y * out->stride + 2 * w + x] =
                b->pixels[y * b->stride + x];
        }
    }

    if (diff) lvg_surface_destroy(diff);
    return out;
}
