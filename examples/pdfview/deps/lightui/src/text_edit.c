/*
 * src/text_edit.c — Multi-line interactive text editor
 *
 * Gap buffer + text layout integration.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <lightui/text_edit.h>
#include "utf8_util.h"

#include <stdlib.h>
#include <string.h>

/* Local aliases for the shared UTF-8 helpers */
#define utf8_cp_len(buf, pos, len) lui__utf8_cp_len((buf), (pos), (len))
#define utf8_prev(buf, pos)        lui__utf8_prev((buf), (pos))

/* ---- Gap buffer internals ----------------------------------------------- */

#define GAP_INIT_CAP 256
#define GAP_MIN_SIZE 64

static int gap_text_len(const lui_text_edit_t *te)
{
    return te->buf_cap - (te->gap_end - te->gap_start);
}

static void gap_ensure(lui_text_edit_t *te, int needed)
{
    int gap_size = te->gap_end - te->gap_start;
    if (gap_size >= needed) return;

    int text_len = gap_text_len(te);
    int new_cap = te->buf_cap * 2;
    if (new_cap < text_len + needed + GAP_MIN_SIZE)
        new_cap = text_len + needed + GAP_MIN_SIZE;

    char *new_buf = (char *)malloc(new_cap);
    if (!new_buf) return;

    /* Copy pre-gap */
    if (te->gap_start > 0)
        memcpy(new_buf, te->buf, te->gap_start);

    /* Copy post-gap at the end of new buffer */
    int tail_len = te->buf_cap - te->gap_end;
    if (tail_len > 0)
        memcpy(new_buf + new_cap - tail_len, te->buf + te->gap_end, tail_len);

    free(te->buf);
    te->buf = new_buf;
    te->gap_end = new_cap - tail_len;
    te->buf_cap = new_cap;
}

/* Move gap so that gap_start == pos (byte offset in logical text) */
static void gap_move_to(lui_text_edit_t *te, int pos)
{
    int text_len = gap_text_len(te);
    if (pos < 0) pos = 0;
    if (pos > text_len) pos = text_len;

    if (pos == te->gap_start) return;

    if (pos < te->gap_start) {
        /* Move text from before gap into after gap */
        int move = te->gap_start - pos;
        memmove(te->buf + te->gap_end - move,
                te->buf + pos, move);
        te->gap_start = pos;
        te->gap_end -= move;
    } else {
        /* Move text from after gap into before gap */
        int move = pos - te->gap_start;
        memmove(te->buf + te->gap_start,
                te->buf + te->gap_end, move);
        te->gap_start += move;
        te->gap_end += move;
    }
}

/* Flatten gap buffer to contiguous string for layout engine */
static void gap_flatten(lui_text_edit_t *te)
{
    int text_len = gap_text_len(te);
    if (text_len + 1 > te->flat_cap) {
        int new_cap = text_len + 1;
        char *new_buf = (char *)realloc(te->flat_buf, new_cap);
        if (!new_buf) return;
        te->flat_buf = new_buf;
        te->flat_cap = new_cap;
    }

    /* Copy pre-gap */
    if (te->gap_start > 0)
        memcpy(te->flat_buf, te->buf, te->gap_start);

    /* Copy post-gap */
    int tail_len = te->buf_cap - te->gap_end;
    if (tail_len > 0)
        memcpy(te->flat_buf + te->gap_start,
               te->buf + te->gap_end, tail_len);

    te->flat_buf[text_len] = '\0';
    te->flat_len = text_len;
}

/* logical_to_buf and logical_byte removed — not currently needed */

/* ---- Lifecycle ---------------------------------------------------------- */

void lui_text_edit_init(lui_text_edit_t *te, lui_font_t *font, int max_width)
{
    if (!te) return;
    memset(te, 0, sizeof(*te));

    te->buf_cap = GAP_INIT_CAP;
    te->buf = (char *)calloc(te->buf_cap, 1);
    te->gap_start = 0;
    te->gap_end = te->buf_cap;

    te->cursor = 0;
    te->sel_start = -1;
    te->sel_end = -1;

    te->font = font;
    te->max_width = max_width;
    te->text_color = LVG_COLOR_WHITE;
    te->sel_color = LVG_COLOR_ARGB(0x80, 0x58, 0x9C, 0xE0);
    te->cursor_color = LVG_COLOR_WHITE;
    te->cursor_width = 2;

    lui_text_layout_init(&te->layout, font, max_width);
    te->needs_layout = true;
}

void lui_text_edit_destroy(lui_text_edit_t *te)
{
    if (!te) return;
    free(te->buf);
    free(te->flat_buf);
    lui_text_layout_destroy(&te->layout);
    memset(te, 0, sizeof(*te));
}

/* ---- Content ------------------------------------------------------------ */

void lui_text_edit_set_text(lui_text_edit_t *te, const char *utf8, int len)
{
    if (!te) return;
    if (!utf8) { len = 0; utf8 = ""; }
    if (len < 0) len = (int)strlen(utf8);

    /* Reset gap buffer with new content */
    gap_ensure(te, len);
    memcpy(te->buf, utf8, len);
    te->gap_start = len;
    te->gap_end = te->buf_cap;

    te->cursor = len;
    te->sel_start = -1;
    te->sel_end = -1;
    te->needs_layout = true;
}

int lui_text_edit_text_len(const lui_text_edit_t *te)
{
    if (!te) return 0;
    return gap_text_len(te);
}

void lui_text_edit_get_text(const lui_text_edit_t *te, char *out, int out_cap)
{
    if (!te || !out || out_cap <= 0) return;
    int text_len = gap_text_len(te);
    int copy_len = text_len < out_cap - 1 ? text_len : out_cap - 1;

    int pre = te->gap_start < copy_len ? te->gap_start : copy_len;
    if (pre > 0)
        memcpy(out, te->buf, pre);

    int remaining = copy_len - pre;
    if (remaining > 0)
        memcpy(out + pre, te->buf + te->gap_end, remaining);

    out[copy_len] = '\0';
}

/* ---- Editing ------------------------------------------------------------ */

void lui_text_edit_insert(lui_text_edit_t *te, const char *utf8, int len)
{
    if (!te || !utf8) return;
    if (len < 0) len = (int)strlen(utf8);
    if (len == 0) return;

    /* Delete selection first if present */
    if (lui_text_edit_has_selection(te))
        lui_text_edit_delete_selection(te);

    gap_move_to(te, te->cursor);
    gap_ensure(te, len);

    memcpy(te->buf + te->gap_start, utf8, len);
    te->gap_start += len;
    te->cursor += len;
    te->needs_layout = true;
}

void lui_text_edit_delete_back(lui_text_edit_t *te, int count)
{
    if (!te || count <= 0) return;
    if (te->cursor <= 0) return;

    gap_move_to(te, te->cursor);

    int del = count < te->gap_start ? count : te->gap_start;
    te->gap_start -= del;
    te->cursor -= del;
    te->needs_layout = true;
}

void lui_text_edit_delete_forward(lui_text_edit_t *te, int count)
{
    if (!te || count <= 0) return;
    int text_len = gap_text_len(te);
    if (te->cursor >= text_len) return;

    gap_move_to(te, te->cursor);

    int tail = te->buf_cap - te->gap_end;
    int del = count < tail ? count : tail;
    te->gap_end += del;
    te->needs_layout = true;
}

void lui_text_edit_delete_selection(lui_text_edit_t *te)
{
    if (!te || !lui_text_edit_has_selection(te)) return;

    int start = te->sel_start < te->sel_end ? te->sel_start : te->sel_end;
    int end   = te->sel_start > te->sel_end ? te->sel_start : te->sel_end;
    int text_len = gap_text_len(te);

    if (start < 0) start = 0;
    if (end > text_len) end = text_len;

    /* Move gap to start, then expand to end */
    gap_move_to(te, start);
    int del = end - start;
    te->gap_end += del;
    te->cursor = start;
    te->sel_start = -1;
    te->sel_end = -1;
    te->needs_layout = true;
}

/* ---- Cursor movement ---------------------------------------------------- */

void lui_text_edit_cursor_left(lui_text_edit_t *te)
{
    if (!te || te->cursor <= 0) return;

    /* Need flattened text for UTF-8 inspection */
    gap_flatten(te);
    te->cursor = utf8_prev(te->flat_buf, te->cursor);
    te->sel_start = -1;
    te->sel_end = -1;
}

void lui_text_edit_cursor_right(lui_text_edit_t *te)
{
    if (!te) return;
    int text_len = gap_text_len(te);
    if (te->cursor >= text_len) return;

    gap_flatten(te);
    int cp_len = utf8_cp_len(te->flat_buf, te->cursor, te->flat_len);
    te->cursor += cp_len;
    te->sel_start = -1;
    te->sel_end = -1;
}

void lui_text_edit_cursor_home(lui_text_edit_t *te)
{
    if (!te) return;
    gap_flatten(te);

    /* Find start of current line */
    int pos = te->cursor;
    while (pos > 0 && te->flat_buf[pos - 1] != '\n')
        pos--;
    te->cursor = pos;
    te->sel_start = -1;
    te->sel_end = -1;
}

void lui_text_edit_cursor_end(lui_text_edit_t *te)
{
    if (!te) return;
    gap_flatten(te);

    int pos = te->cursor;
    while (pos < te->flat_len && te->flat_buf[pos] != '\n')
        pos++;
    te->cursor = pos;
    te->sel_start = -1;
    te->sel_end = -1;
}

void lui_text_edit_cursor_up(lui_text_edit_t *te)
{
    if (!te || !te->font) return;

    lui_text_edit_build(te);

    /* Find cursor position via hit-test at current line - 1 line height */
    int cursor_x = 0;
    if (te->cursor > 0 && te->flat_buf)
        cursor_x = lui_font_measure_text(te->font, te->flat_buf, te->cursor);

    int lh = lui_font_line_height(te->font);

    /* Find which line the cursor is on */
    int cursor_line = -1;
    int cursor_y_approx = 0;
    for (int i = 0; i < te->layout.line_count; i++) {
        const lui_line_t *line = &te->layout.lines[i];
        if (line->y + line->height > cursor_y_approx) {
            cursor_line = i;
            break;
        }
        cursor_y_approx = line->y + line->height;
    }

    if (cursor_line <= 0) {
        /* Already on first line */
        te->cursor = 0;
        return;
    }

    /* Hit-test on the line above */
    const lui_line_t *prev_line = &te->layout.lines[cursor_line - 1];
    int hit = lui_text_edit_hit_test(te, cursor_x, prev_line->y + lh / 2);
    if (hit >= 0)
        te->cursor = hit;
    te->sel_start = -1;
    te->sel_end = -1;
}

void lui_text_edit_cursor_down(lui_text_edit_t *te)
{
    if (!te || !te->font) return;

    lui_text_edit_build(te);

    int cursor_x = 0;
    if (te->cursor > 0 && te->flat_buf)
        cursor_x = lui_font_measure_text(te->font, te->flat_buf, te->cursor);

    int lh = lui_font_line_height(te->font);

    /* Find which line the cursor is on.
     * Simple heuristic: first line that contains the cursor, or the last. */
    int cursor_line = te->layout.line_count > 0
                      ? te->layout.line_count - 1 : -1;
    /* For cursor_down we just need to be on *some* line so we can
     * advance to the next one.  Use line 0 as default. */
    if (cursor_line > 0) cursor_line = 0;

    if (cursor_line < 0 || cursor_line >= te->layout.line_count - 1) {
        /* Already on last line */
        te->cursor = gap_text_len(te);
        return;
    }

    const lui_line_t *next_line = &te->layout.lines[cursor_line + 1];
    int hit = lui_text_edit_hit_test(te, cursor_x, next_line->y + lh / 2);
    if (hit >= 0)
        te->cursor = hit;
    te->sel_start = -1;
    te->sel_end = -1;
}

/* ---- Selection ---------------------------------------------------------- */

void lui_text_edit_set_selection(lui_text_edit_t *te, int start, int end)
{
    if (!te) return;
    te->sel_start = start;
    te->sel_end = end;
}

void lui_text_edit_clear_selection(lui_text_edit_t *te)
{
    if (!te) return;
    te->sel_start = -1;
    te->sel_end = -1;
}

bool lui_text_edit_has_selection(const lui_text_edit_t *te)
{
    if (!te) return false;
    return te->sel_start >= 0 && te->sel_end >= 0 && te->sel_start != te->sel_end;
}

/* ---- Hit testing -------------------------------------------------------- */

int lui_text_edit_hit_test(lui_text_edit_t *te, int px, int py)
{
    if (!te || !te->font) return -1;

    lui_text_edit_build(te);

    if (te->layout.line_count == 0) return 0;

    /* Find the line that contains py */
    int target_line = -1;
    for (int i = 0; i < te->layout.line_count; i++) {
        const lui_line_t *line = &te->layout.lines[i];
        if (py >= line->y && py < line->y + line->height) {
            target_line = i;
            break;
        }
    }

    /* If above all lines, return start of text */
    if (target_line < 0 && py < 0) return 0;

    /* If below all lines, return end of text */
    if (target_line < 0)
        return gap_text_len(te);

    /* Walk runs on this line to find the closest character boundary */
    const lui_line_t *line = &te->layout.lines[target_line];

    /* Find the run containing px */
    for (int ri = line->run_start;
         ri < line->run_start + line->run_count; ri++) {
        const lui_run_t *run = &te->layout.runs[ri];
        if (run->is_image) continue;

        if (px >= run->x && px < run->x + run->width && run->utf8 && run->len > 0) {
            /* Walk characters in this run */
            const char *rp = run->utf8;
            int byte_off = (int)(rp - te->flat_buf);
            int local_x = run->x;

            const char *rend = rp + run->len;
            int best = byte_off;
            int best_dist = px < local_x ? local_x - px : px - local_x;

            while (rp < rend) {
                int cp_len = utf8_cp_len(rp, 0, (int)(rend - rp));
                int advance = lui_font_measure_text(te->font, rp, cp_len);
                local_x += advance;
                byte_off += cp_len;
                rp += cp_len;

                int dist = px - local_x;
                if (dist < 0) dist = -dist;
                if (dist < best_dist) {
                    best_dist = dist;
                    best = byte_off;
                }
            }
            return best;
        }
    }

    /* If we're past the last run on the line, return the end offset of that line */
    if (line->run_count > 0) {
        const lui_run_t *last = &te->layout.runs[line->run_start + line->run_count - 1];
        if (!last->is_image && last->utf8) {
            return (int)(last->utf8 - te->flat_buf) + last->len;
        }
    }

    return gap_text_len(te);
}

/* ---- Layout & draw ------------------------------------------------------ */

void lui_text_edit_build(lui_text_edit_t *te)
{
    if (!te || !te->needs_layout) return;

    gap_flatten(te);

    /* Feed the flattened text to the layout engine */
    lui_text_layout_clear(&te->layout);
    if (te->flat_len > 0) {
        lui_text_layout_add_text(&te->layout,
                                 te->flat_buf, te->flat_len,
                                 te->text_color, LVG_COLOR_TRANSPARENT,
                                 0, te->font);
    }
    lui_text_layout_build(&te->layout);
    te->needs_layout = false;
}

void lui_text_edit_draw(lui_text_edit_t *te,
                         lvg_canvas_t *canvas,
                         int x, int y,
                         double time_s)
{
    if (!te || !canvas || !te->font) return;

    lui_text_edit_build(te);

    te->anim_time = time_s;

    /* Draw selection highlight */
    if (lui_text_edit_has_selection(te)) {
        int s0 = te->sel_start < te->sel_end ? te->sel_start : te->sel_end;
        int s1 = te->sel_start > te->sel_end ? te->sel_start : te->sel_end;

        /* Walk layout runs and highlight those within selection */
        for (int ri = 0; ri < te->layout.run_count; ri++) {
            const lui_run_t *run = &te->layout.runs[ri];
            if (run->is_image || !run->utf8) continue;

            int run_start = (int)(run->utf8 - te->flat_buf);
            int run_end = run_start + run->len;

            /* Check overlap with selection */
            int overlap_start = s0 > run_start ? s0 : run_start;
            int overlap_end = s1 < run_end ? s1 : run_end;
            if (overlap_start >= overlap_end) continue;

            /* Compute pixel range for the overlap */
            int px_start = run->x;
            if (overlap_start > run_start)
                px_start += lui_font_measure_text(te->font,
                    run->utf8, overlap_start - run_start);

            int px_end = run->x;
            if (overlap_end > run_start)
                px_end += lui_font_measure_text(te->font,
                    run->utf8, overlap_end - run_start);

            /* Find the line this run is on */
            int line_h = lui_font_line_height(te->font);
            int ry = run->y - lui_font_ascent(te->font);

            lvg_canvas_fill_rect(canvas,
                                  x + px_start, y + ry,
                                  px_end - px_start, line_h,
                                  te->sel_color);
        }
    }

    /* Draw text via layout engine */
    lui_text_layout_draw(&te->layout, canvas, x, y, NULL, NULL);

    /* Draw cursor */
    if (te->flat_buf) {
        int cursor_px = 0;
        if (te->cursor > 0)
            cursor_px = lui_font_measure_text(te->font,
                te->flat_buf, te->cursor);

        /* Find cursor y by finding which line it's on */
        int cursor_y = 0;
        int lh = lui_font_line_height(te->font);

        /* Simple approach: cursor y = line_of_cursor * line_height */
        /* Count newlines before cursor to determine line */
        int line_num = 0;
        for (int i = 0; i < te->cursor && i < te->flat_len; i++) {
            if (te->flat_buf[i] == '\n') {
                line_num++;
                cursor_px = 0;
            }
        }

        /* Recompute cursor_px from start of current line */
        int line_start = te->cursor;
        while (line_start > 0 && te->flat_buf[line_start - 1] != '\n')
            line_start--;
        if (te->cursor > line_start)
            cursor_px = lui_font_measure_text(te->font,
                te->flat_buf + line_start, te->cursor - line_start);

        if (line_num < te->layout.line_count)
            cursor_y = te->layout.lines[line_num].y;
        else
            cursor_y = line_num * lh;

        lvg_canvas_fill_rect(canvas,
                              x + cursor_px, y + cursor_y,
                              te->cursor_width, lh,
                              te->cursor_color);
    }
}

/* ---- Animation ---------------------------------------------------------- */

void lui_text_edit_set_animation(lui_text_edit_t *te,
                                  lui_text_anim_fn fn, void *user)
{
    if (!te) return;
    te->anim_fn = fn;
    te->anim_user = user;
}
