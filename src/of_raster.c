/* GFOpenFont's outline rasteriser: a 26.6 outline (TrueType quadratics, CFF
 * cubics) to an A8 coverage bitmap, without hinting.
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE).
 *
 * Exact area coverage, in integers only:
 *   - each curve is cut into straight lines, more of them the more it bends
 *     (its second differences), so no line strays more than about a
 *     sixteenth of a pixel from the curve;
 *   - each line adds, to every pixel it crosses, how far it climbs there
 *     (cover) and that climb times its mean place across the pixel (area);
 *   - one sweep along each row then gives a pixel's coverage: the cover of
 *     every line to its left, less the part of its own lines' climb that
 *     lies left of them. Non-zero winding: the magnitude, capped at full.
 * No state outside the call, nothing but integer arithmetic and two arrays,
 * so it can move to GFOpenGPU as it is. */
#include "gfopenfont/gfopenfont.h"
#include "of_font.h"

#include <stdlib.h>
#include <string.h>

#define SUB 256                     /* subpixels a pixel, each way */
#define SUB_SHIFT 8

typedef struct Raster {
    int32_t w, h;                   /* in pixels */
    int32_t *cover;                 /* per cell: the climb, in subpixels */
    int32_t *area;                  /* per cell: climb * 2 * x within the cell, in subpixels^2 */
} Raster;

/* adds a piece of line that stays in pixel column i of row j: it climbs dy
 * (signed subpixels) between x positions fx0 and fx1 (subpixels in the
 * column, 0..SUB) */
static void cell(Raster *r, int32_t i, int32_t j, int32_t dy, int32_t fx0, int32_t fx1)
{
    int32_t at;
    if (j < 0 || j >= r->h || !dy)
        return;
    if (i < 0) {
        i = 0;                      /* left of the bitmap: all of its climb counts in column 0 */
        fx0 = fx1 = 0;
    }
    if (i >= r->w)
        return;                     /* right of the bitmap: covers nothing */
    at = j * r->w + i;
    r->cover[at] += dy;
    r->area[at] += dy * (fx0 + fx1);
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/* x at height y along the line (x0,y0)-(x1,y1), y0 != y1, rounded down */
static int64_t x_at(int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t y)
{
    return x0 + floor_div((x1 - x0) * (y - y0), y1 - y0);
}

/* the part of a line within row j: from (xa, ya) to (xb, yb), both heights
 * within the row, climbing dy (sign: the line's direction) */
static void row_piece(Raster *r, int32_t j, int64_t xa, int64_t xb, int32_t dy)
{
    int64_t ia = floor_div(xa, SUB), ib = floor_div(xb, SUB);
    int64_t i, step, x_from, dx;
    int32_t done = 0;
    if (ia == ib) {
        cell(r, (int32_t)ia, j, dy, (int32_t)(xa - ia * SUB), (int32_t)(xb - ia * SUB));
        return;
    }
    /* across several columns: the climb shared out by how much of the line's
     * width each column holds, the remainder kept so the shares add up */
    dx = xb - xa;
    step = dx > 0 ? 1 : -1;
    x_from = xa;
    for (i = ia;; i += step) {
        int64_t x_to, share;
        int last = i == ib;
        if (last) {
            x_to = xb;
            share = dy - done;
        } else {
            x_to = step > 0 ? (i + 1) * SUB : i * SUB;
            share = (int64_t)dy * (x_to - xa) / dx - done;
        }
        cell(r, (int32_t)i, j, (int32_t)share, (int32_t)(x_from - i * SUB), (int32_t)(x_to - i * SUB));
        done += (int32_t)share;
        if (last)
            break;
        x_from = x_to;
    }
}

/* a straight line, subpixel coordinates (y down) */
static void line(Raster *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    int32_t sign = 1;
    int64_t y, ylim;
    if (y0 == y1)
        return;                     /* horizontal: covers no height */
    if (y0 > y1) {
        int64_t t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        sign = -1;
    }
    /* clip to the bitmap's rows */
    ylim = (int64_t)r->h * SUB;
    if (y1 <= 0 || y0 >= ylim)
        return;
    y = y0 < 0 ? 0 : y0;
    while (y < y1 && y < ylim) {
        int64_t j = y >> SUB_SHIFT;
        int64_t yn = (j + 1) * SUB;
        int64_t xa, xb;
        if (yn > y1)
            yn = y1;
        xa = (y == y0) ? x0 : x_at(x0, y0, x1, y1, y);
        xb = (yn == y1) ? x1 : x_at(x0, y0, x1, y1, yn);
        row_piece(r, (int32_t)j, xa, xb, sign * (int32_t)(yn - y));
        y = yn;
    }
}

/* ---- curves ---- */

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* The lines a curve is cut into: n equal steps of its parameter leave each
 * line at most m / (8 n^2) from the curve, m the largest second derivative
 * of the curve: 2 dd for a quadratic, 6 dd for a cubic, dd the largest
 * second difference of its control points. n keeps that under TOLERANCE
 * subpixels; OF_RASTER_TOLERANCE can set it for a test. */
#ifndef OF_RASTER_TOLERANCE
#define OF_RASTER_TOLERANCE (SUB / 16)
#endif
#define TOLERANCE OF_RASTER_TOLERANCE
#define MAX_PIECES 4096

static uint32_t pieces_for(int64_t dd)
{
    uint64_t want = (uint64_t)(dd / (8 * TOLERANCE)) + 1;
    uint32_t n = isqrt64(want);
    if ((uint64_t)n * n < want)
        n++;
    if (n < 1)
        n = 1;
    if (n > MAX_PIECES)
        n = MAX_PIECES;
    return n;
}

static void quad(Raster *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    int64_t ddx = x0 - 2 * x1 + x2, ddy = y0 - 2 * y1 + y2;
    int64_t dd = iabs64(ddx) > iabs64(ddy) ? iabs64(ddx) : iabs64(ddy);
    uint32_t n = pieces_for(2 * dd), k;
    int64_t n2 = (int64_t)n * n, px = x0, py = y0;
    for (k = 1; k <= n; ++k) {
        int64_t a = (int64_t)(n - k) * (n - k), b = 2 * (int64_t)k * (n - k), c = (int64_t)k * k;
        int64_t qx = (a * x0 + b * x1 + c * x2), qy = (a * y0 + b * y1 + c * y2);
        int64_t x = k == n ? x2 : floor_div(qx + n2 / 2, n2);
        int64_t y = k == n ? y2 : floor_div(qy + n2 / 2, n2);
        line(r, px, py, x, y);
        px = x;
        py = y;
    }
}

static void cubic(Raster *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                  int64_t x2, int64_t y2, int64_t x3, int64_t y3)
{
    int64_t d1x = x0 - 2 * x1 + x2, d1y = y0 - 2 * y1 + y2;
    int64_t d2x = x1 - 2 * x2 + x3, d2y = y1 - 2 * y2 + y3;
    int64_t dd = iabs64(d1x), t;
    uint32_t n, k;
    int64_t n3, px = x0, py = y0;
    if ((t = iabs64(d1y)) > dd) dd = t;
    if ((t = iabs64(d2x)) > dd) dd = t;
    if ((t = iabs64(d2y)) > dd) dd = t;
    n = pieces_for(6 * dd);
    n3 = (int64_t)n * n * n;
    for (k = 1; k <= n; ++k) {
        int64_t u = n - k;
        int64_t a = u * u * u, b = 3 * (int64_t)k * u * u, c = 3 * (int64_t)k * k * u, d = (int64_t)k * k * k;
        int64_t x = k == n ? x3 : floor_div(a * x0 + b * x1 + c * x2 + d * x3 + n3 / 2, n3);
        int64_t y = k == n ? y3 : floor_div(a * y0 + b * y1 + c * y2 + d * y3 + n3 / 2, n3);
        line(r, px, py, x, y);
        px = x;
        py = y;
    }
}

/* ---- the outline ---- */

typedef struct Pt {
    int64_t x, y;
} Pt;

/* one contour, its points already in subpixels (y down) */
static void contour(Raster *r, const OFPath *p, uint32_t first, uint32_t last,
                    int64_t ox, int64_t oy)
{
    uint32_t n = last - first + 1, k, start;
    Pt cur, beg;
#define PX(i) ((int64_t)(p->x[(i)] - ox) * (SUB / 64))
#define PY(i) ((int64_t)(oy - p->y[(i)]) * (SUB / 64))
    if (n < 2)
        return;
    /* begin at an on-curve point; a quadratic contour may have none, then
     * at the middle of its first two control points */
    for (start = 0; start < n; ++start)
        if (p->tag[first + start] == OF_PT_ON)
            break;
    if (start == n) {
        if (p->tag[first] != OF_PT_QUAD)
            return;
        beg.x = (PX(first) + PX(first + 1)) / 2;
        beg.y = (PY(first) + PY(first + 1)) / 2;
        start = 0;
        /* from the middle of points 0 and 1: each control point k + 1 takes
         * the curve to the middle of it and the next, the last back to beg */
        cur = beg;
        for (k = 0; k < n; ++k) {
            uint32_t c = first + (k + 1) % n;
            uint32_t i = first + (k + 2) % n;
            Pt ctl = { PX(c), PY(c) }, to;
            to.x = (ctl.x + PX(i)) / 2;
            to.y = (ctl.y + PY(i)) / 2;
            if (k == n - 1)
                to = beg;
            quad(r, cur.x, cur.y, ctl.x, ctl.y, to.x, to.y);
            cur = to;
        }
        return;
    }
    beg.x = PX(first + start);
    beg.y = PY(first + start);
    cur = beg;
    for (k = 1; k <= n;) {
        uint32_t i = first + (start + k) % n;
        uint8_t tag = p->tag[i];
        if (tag == OF_PT_ON) {
            Pt to = { PX(i), PY(i) };
            line(r, cur.x, cur.y, to.x, to.y);
            cur = to;
            k++;
        } else if (tag == OF_PT_QUAD) {
            Pt ctl = { PX(i), PY(i) }, to;
            uint32_t ni = first + (start + k + 1) % n;
            if (p->tag[ni] == OF_PT_ON) {
                to.x = PX(ni);
                to.y = PY(ni);
                k += 2;
            } else {
                /* two control points: the curve passes through their middle */
                to.x = (ctl.x + PX(ni)) / 2;
                to.y = (ctl.y + PY(ni)) / 2;
                k += 1;
            }
            quad(r, cur.x, cur.y, ctl.x, ctl.y, to.x, to.y);
            cur = to;
        } else {
            /* a cubic: two control points, then the end */
            uint32_t i2 = first + (start + k + 1) % n, i3 = first + (start + k + 2) % n;
            Pt c1 = { PX(i), PY(i) }, c2 = { PX(i2), PY(i2) }, to = { PX(i3), PY(i3) };
            if (k + 2 > n)
                to = beg;
            cubic(r, cur.x, cur.y, c1.x, c1.y, c2.x, c2.y, to.x, to.y);
            cur = to;
            k += 3;
        }
    }
    if (cur.x != beg.x || cur.y != beg.y)
        line(r, cur.x, cur.y, beg.x, beg.y);
#undef PX
#undef PY
}

static int32_t floor64(int32_t v) { return v & ~63; }
static int32_t ceil64(int32_t v) { return (v + 63) & ~63; }

int of_raster_a8(const OFPath *path, uint32_t *width, uint32_t *height,
                 int32_t *left, int32_t *top, uint8_t **pixels)
{
    Raster r;
    int32_t xmin, ymin, xmax, ymax, x, y;
    uint32_t i, c, first;
    size_t cells;
    uint8_t *out;

    *width = *height = 0;
    *left = *top = 0;
    *pixels = NULL;
    if (!path || !path->npoints || !path->ncontours)
        return OF_OK;
    /* the control box, rounded out to whole pixels */
    xmin = xmax = path->x[0];
    ymin = ymax = path->y[0];
    for (i = 1; i < path->npoints; ++i) {
        if (path->x[i] < xmin) xmin = path->x[i];
        if (path->x[i] > xmax) xmax = path->x[i];
        if (path->y[i] < ymin) ymin = path->y[i];
        if (path->y[i] > ymax) ymax = path->y[i];
    }
    xmin = floor64(xmin);
    ymin = floor64(ymin);
    xmax = ceil64(xmax);
    ymax = ceil64(ymax);
    r.w = (xmax - xmin) >> 6;
    r.h = (ymax - ymin) >> 6;
    *left = xmin >> 6;
    *top = ymax >> 6;
    if (r.w <= 0 || r.h <= 0)
        return OF_OK;
    if (r.w > 16384 || r.h > 16384)
        return OF_ERR_RANGE;
    *width = (uint32_t)r.w;
    *height = (uint32_t)r.h;
    cells = (size_t)r.w * r.h;
    r.cover = (int32_t *)calloc(cells, sizeof(int32_t));
    r.area = (int32_t *)calloc(cells, sizeof(int32_t));
    out = (uint8_t *)malloc(cells);
    if (!r.cover || !r.area || !out) {
        free(r.cover);
        free(r.area);
        free(out);
        *width = *height = 0;
        return OF_ERR_MEMORY;
    }
    first = 0;
    for (c = 0; c < path->ncontours; ++c) {
        uint32_t last = path->ends[c];
        if (last >= path->npoints || last < first)
            break;
        contour(&r, path, first, last, xmin, ymax);
        first = last + 1;
    }
    /* the sweep: coverage in subpixels^2 is SUB * (cover to the left and
     * here) - area / 2 */
    for (y = 0; y < r.h; ++y) {
        int64_t acc = 0;
        for (x = 0; x < r.w; ++x) {
            size_t at = (size_t)y * r.w + x;
            int64_t cov;
            acc += r.cover[at];
            cov = acc * SUB - r.area[at] / 2;
            if (cov < 0)
                cov = -cov;
            /* SUB * SUB is a full pixel: 255 */
            cov = (cov * 255 + (SUB * SUB) / 2) / (SUB * SUB);
            out[at] = (uint8_t)(cov > 255 ? 255 : cov);
        }
    }
    free(r.cover);
    free(r.area);
    *pixels = out;
    return OF_OK;
}
