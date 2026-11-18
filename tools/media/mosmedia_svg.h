/*
 * SVG -> vector .img (version 2), for tools/media/mosmedia.c (`mosmedia vimg`).
 *
 * Reads the part of SVG that icon sets and logos use:
 *   <svg viewBox width height>, <g>, <a>, <switch>, <use href> (with x/y),
 *   <path d> (all commands, relative ones, arcs as cubics), <rect rx ry>,
 *   <circle>, <ellipse>, <line>, <polyline>, <polygon>;
 *   transform (matrix translate scale rotate skewX skewY), fill, stroke,
 *   stroke-width/-linecap/-linejoin/-miterlimit, opacity, fill-opacity,
 *   stroke-opacity, fill-rule, color, display, visibility as attributes
 *   and in style="";
 *   colors (#rgb #rgba #rrggbb #rrggbbaa rgb() rgba() hsl() named none
 *   transparent currentColor), url(#id) with <linearGradient> and
 *   <radialGradient> (stops, href inheritance, gradientUnits,
 *   gradientTransform, spreadMethod).
 * Everything else (text, images, filters, masks, clip paths, patterns, style
 * sheets, dashes, markers) is skipped with a warning.
 *
 * `currentColor` becomes the "current color" paint unless a color property
 * was set, so the icon takes the caller's tint. Transforms are baked into the
 * coordinates; a shape with a stroke under a transform that is not a
 * similarity (a non-uniform scale or a skew) keeps the transform so its
 * stroke is bent like the SVG's. `opacity` multiplies into the fill and
 * stroke opacity of each shape below it.
 */
#ifndef MOSMEDIA_SVG_H
#define MOSMEDIA_SVG_H

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mosmedia.h"

#define SVG_PI 3.14159265358979323846

static bool svg_istarts(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != *prefix) return false;
    return true;
}

/* ---------------------------------------------------------------- utils */

static void svg_warn(const char* fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    static char seen[64][256];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], msg)) return;
    if (nseen < 64) strcpy(seen[nseen++], msg);
    fprintf(stderr, "mosmedia: warning: %s\n", msg);
}

static void* svg_xrealloc(void* p, size_t n) {
    void* r = realloc(p, n ? n : 1);
    if (!r) { fprintf(stderr, "mosmedia: out of memory\n"); exit(1); }
    return r;
}
static char* svg_strndup(const char* s, size_t n) {
    char* r = svg_xrealloc(NULL, n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* ------------------------------------------------------------------ XML */

typedef struct { char* name; char* value; } svg_attr_t;
typedef struct svg_node {
    char* name;
    svg_attr_t* attrs;
    int nattrs;
    struct svg_node** kids;
    int nkids;
    struct svg_node* parent;
} svg_node_t;

static const char* svg_attr(const svg_node_t* n, const char* name) {
    for (int i = 0; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

static void svg_decode_entities(char* s) {
    char* w = s;
    for (char* r = s; *r;) {
        if (*r != '&') { *w++ = *r++; continue; }
        char* semi = strchr(r, ';');
        if (!semi || semi - r > 10) { *w++ = *r++; continue; }
        if (!strncmp(r, "&amp;", 5)) *w++ = '&';
        else if (!strncmp(r, "&lt;", 4)) *w++ = '<';
        else if (!strncmp(r, "&gt;", 4)) *w++ = '>';
        else if (!strncmp(r, "&quot;", 6)) *w++ = '"';
        else if (!strncmp(r, "&apos;", 6)) *w++ = '\'';
        else if (r[1] == '#') {
            long v = r[2] == 'x' ? strtol(r + 3, NULL, 16) : strtol(r + 2, NULL, 10);
            *w++ = v > 0 && v < 128 ? (char)v : '?';
        } else { *w++ = *r++; continue; }
        r = semi + 1;
    }
    *w = 0;
}

static bool svg_name_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.'; }

/* Parses the document; returns the root element or NULL. */
static svg_node_t* svg_parse_xml(const char* t, size_t len) {
    const char* end = t + len;
    svg_node_t* root = NULL;
    svg_node_t* cur = NULL;
    while (t < end) {
        if (*t != '<') { t++; continue; }
        if (end - t >= 4 && !strncmp(t, "<!--", 4)) {
            const char* e = strstr(t + 4, "-->");
            if (!e) return root;
            t = e + 3;
        } else if (end - t >= 9 && !strncmp(t, "<![CDATA[", 9)) {
            const char* e = strstr(t + 9, "]]>");
            if (!e) return root;
            t = e + 3;
        } else if (t[1] == '?') {
            const char* e = strstr(t + 2, "?>");
            if (!e) return root;
            t = e + 2;
        } else if (t[1] == '!') {
            int depth = 0;
            t++;
            while (t < end && !(depth == 0 && *t == '>')) { if (*t == '[') depth++; if (*t == ']') depth--; t++; }
            t++;
        } else if (t[1] == '/') {
            const char* e = memchr(t, '>', (size_t)(end - t));
            if (!e) return root;
            if (cur) cur = cur->parent;
            t = e + 1;
        } else {
            t++;
            const char* ns = t;
            while (t < end && svg_name_char(*t)) t++;
            svg_node_t* n = svg_xrealloc(NULL, sizeof *n);
            memset(n, 0, sizeof *n);
            const char* colon = memchr(ns, ':', (size_t)(t - ns));
            if (colon) ns = colon + 1;
            n->name = svg_strndup(ns, (size_t)(t - ns));
            n->parent = cur;
            bool selfclose = false;
            for (;;) {
                while (t < end && isspace((unsigned char)*t)) t++;
                if (t >= end) return root;
                if (*t == '>') { t++; break; }
                if (*t == '/') { selfclose = true; t++; continue; }
                const char* an = t;
                while (t < end && svg_name_char(*t)) t++;
                if (t == an) { t++; continue; }
                size_t alen = (size_t)(t - an);
                while (t < end && isspace((unsigned char)*t)) t++;
                char* val = NULL;
                if (t < end && *t == '=') {
                    t++;
                    while (t < end && isspace((unsigned char)*t)) t++;
                    if (t < end && (*t == '"' || *t == '\'')) {
                        char q = *t++;
                        const char* ve = memchr(t, q, (size_t)(end - t));
                        if (!ve) return root;
                        val = svg_strndup(t, (size_t)(ve - t));
                        t = ve + 1;
                    }
                }
                if (!val) val = svg_strndup("", 0);
                svg_decode_entities(val);
                /* keep plain names and xlink:href; drop every other prefixed attribute */
                const char* c2 = memchr(an, ':', alen);
                char* nm = NULL;
                if (!c2) nm = svg_strndup(an, alen);
                else if (c2 - an == 5 && !strncmp(an, "xlink", 5)) nm = svg_strndup(c2 + 1, alen - (size_t)(c2 + 1 - an));
                if (!nm || !strncmp(nm, "xmlns", 5)) { free(nm); free(val); continue; }
                n->attrs = svg_xrealloc(n->attrs, (size_t)(n->nattrs + 1) * sizeof *n->attrs);
                n->attrs[n->nattrs].name = nm;
                n->attrs[n->nattrs].value = val;
                n->nattrs++;
            }
            if (cur) {
                cur->kids = svg_xrealloc(cur->kids, (size_t)(cur->nkids + 1) * sizeof *cur->kids);
                cur->kids[cur->nkids++] = n;
            } else if (!root) {
                root = n;
            }
            if (!selfclose) cur = n;
        }
    }
    return root;
}

/* -------------------------------------------------------------- numbers */

static const char* svg_skip(const char* p) {
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
    return p;
}

/* A number at *pp; false if there is none. */
static bool svg_num(const char** pp, double* v) {
    const char* p = svg_skip(*pp);
    char* e;
    if (!(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) return false;
    double d = strtod(p, &e);
    if (e == p) return false;
    *pp = e;
    *v = d;
    return true;
}

/* A length: a number and an optional unit (% is a fraction of `ref`). */
static double svg_length(const char* s, double ref, double dflt) {
    if (!s) return dflt;
    const char* p = s;
    double v;
    if (!svg_num(&p, &v)) return dflt;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '%') return v * ref / 100.0;
    if (!strncmp(p, "pt", 2)) return v * 4.0 / 3.0;
    if (!strncmp(p, "pc", 2)) return v * 16.0;
    if (!strncmp(p, "mm", 2)) return v * 96.0 / 25.4;
    if (!strncmp(p, "cm", 2)) return v * 96.0 / 2.54;
    if (!strncmp(p, "in", 2)) return v * 96.0;
    if (!strncmp(p, "em", 2)) return v * 16.0;
    if (!strncmp(p, "ex", 2)) return v * 8.0;
    return v;
}

/* ----------------------------------------------------------------- math */

typedef struct { double a, b, c, d, e, f; } svg_mat_t;   /* x' = a x + c y + e; y' = b x + d y + f */
static const svg_mat_t SVG_I = { 1, 0, 0, 1, 0, 0 };

static svg_mat_t svg_mul(svg_mat_t A, svg_mat_t B) {    /* A after B */
    svg_mat_t r;
    r.a = A.a * B.a + A.c * B.b; r.b = A.b * B.a + A.d * B.b;
    r.c = A.a * B.c + A.c * B.d; r.d = A.b * B.c + A.d * B.d;
    r.e = A.a * B.e + A.c * B.f + A.e; r.f = A.b * B.e + A.d * B.f + A.f;
    return r;
}
static void svg_apply(svg_mat_t m, double x, double y, double* ox, double* oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

static svg_mat_t svg_parse_transform(const char* s) {
    svg_mat_t m = SVG_I;
    if (!s) return m;
    const char* p = s;
    for (;;) {
        p = svg_skip(p);
        if (!*p) break;
        const char* ns = p;
        while (isalpha((unsigned char)*p)) p++;
        size_t nl = (size_t)(p - ns);
        while (isspace((unsigned char)*p)) p++;
        if (!nl || *p != '(') { svg_warn("bad transform \"%s\"", s); break; }
        p++;
        double v[6] = { 0, 0, 0, 0, 0, 0 };
        int n = 0;
        while (n < 6 && svg_num(&p, &v[n])) n++;
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
        svg_mat_t t = SVG_I;
        if (nl == 6 && !strncmp(ns, "matrix", 6) && n == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if (nl == 9 && !strncmp(ns, "translate", 9) && n >= 1) { t.e = v[0]; t.f = n > 1 ? v[1] : 0; }
        else if (nl == 5 && !strncmp(ns, "scale", 5) && n >= 1) { t.a = v[0]; t.d = n > 1 ? v[1] : v[0]; }
        else if (nl == 6 && !strncmp(ns, "rotate", 6) && n >= 1) {
            double r = v[0] * SVG_PI / 180.0, cs = cos(r), sn = sin(r);
            t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
            if (n >= 3) { t.e = v[1] - cs * v[1] + sn * v[2]; t.f = v[2] - sn * v[1] - cs * v[2]; }
        }
        else if (nl == 5 && !strncmp(ns, "skewX", 5) && n >= 1) t.c = tan(v[0] * SVG_PI / 180.0);
        else if (nl == 5 && !strncmp(ns, "skewY", 5) && n >= 1) t.b = tan(v[0] * SVG_PI / 180.0);
        else svg_warn("bad transform \"%s\"", s);
        m = svg_mul(m, t);
    }
    return m;
}

/* --------------------------------------------------------------- colors */

static const struct { const char* name; uint32_t rgb; } svg_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
    { "yellow", 0xFFFF00 }, { "cyan", 0x00FFFF }, { "aqua", 0x00FFFF }, { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "olive", 0x808000 },
    { "lime", 0x00FF00 }, { "teal", 0x008080 }, { "navy", 0x000080 }, { "purple", 0x800080 }, { "orange", 0xFFA500 },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE },
    { "coral", 0xFF7F50 }, { "crimson", 0xDC143C }, { "salmon", 0xFA8072 }, { "tomato", 0xFF6347 }, { "khaki", 0xF0E68C },
    { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "tan", 0xD2B48C }, { "turquoise", 0x40E0D0 }, { "skyblue", 0x87CEEB },
    { "lightblue", 0xADD8E6 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "lightgreen", 0x90EE90 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "darkblue", 0x00008B }, { "darkgreen", 0x006400 },
    { "darkred", 0x8B0000 }, { "darkorange", 0xFF8C00 }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "gainsboro", 0xDCDCDC }, { "whitesmoke", 0xF5F5F5 }, { "royalblue", 0x4169E1 }, { "steelblue", 0x4682B4 },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "orangered", 0xFF4500 }, { "limegreen", 0x32CD32 },
    { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 }, { "dodgerblue", 0x1E90FF }, { "deepskyblue", 0x00BFFF },
    { "hotpink", 0xFF69B4 }, { "deeppink", 0xFF1493 }, { "chocolate", 0xD2691E }, { "firebrick", 0xB22222 },
};

static double svg_hue(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

/* 0xAARRGGBB; false if it is not a color. */
static bool svg_color(const char* s, uint32_t* out) {
    while (isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) len--;
    char buf[64];
    if (!len || len >= sizeof buf) return false;
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[len] = 0;
    if (buf[0] == '#') {
        uint32_t v = 0;
        for (size_t i = 1; i < len; i++) {
            if (!isxdigit((unsigned char)buf[i])) return false;
            v = v << 4 | (uint32_t)(buf[i] <= '9' ? buf[i] - '0' : buf[i] - 'a' + 10);
        }
        if (len == 4) { *out = 0xFF000000u | ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17; return true; }
        if (len == 5) { *out = ((v & 15) * 17) << 24 | ((v >> 12 & 15) * 17) << 16 | ((v >> 8 & 15) * 17) << 8 | (v >> 4 & 15) * 17; return true; }
        if (len == 7) { *out = 0xFF000000u | v; return true; }
        if (len == 9) { *out = (v & 255) << 24 | v >> 8; return true; }
        return false;
    }
    if (!strncmp(buf, "rgb", 3) || !strncmp(buf, "hsl", 3)) {
        bool hsl = buf[0] == 'h';
        const char* p = strchr(buf, '(');
        if (!p) return false;
        p++;
        double v[4] = { 0, 0, 0, 1 };
        int n = 0;
        bool pct[4] = { false, false, false, false };
        while (n < 4) {
            p = svg_skip(p);
            if (*p == '/') p = svg_skip(p + 1);
            if (!svg_num(&p, &v[n])) break;
            if (*p == '%') { pct[n] = true; p++; }
            n++;
        }
        if (n < 3) return false;
        double r, g, b;
        if (hsl) {
            double h = fmod(fmod(v[0], 360.0) + 360.0, 360.0) / 360.0, sat = v[1] / 100.0, l = v[2] / 100.0;
            double q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, pp = 2 * l - q;
            r = svg_hue(pp, q, h + 1.0 / 3) * 255; g = svg_hue(pp, q, h) * 255; b = svg_hue(pp, q, h - 1.0 / 3) * 255;
        } else {
            r = pct[0] ? v[0] * 2.55 : v[0]; g = pct[1] ? v[1] * 2.55 : v[1]; b = pct[2] ? v[2] * 2.55 : v[2];
        }
        double a = n > 3 ? (pct[3] ? v[3] / 100.0 : v[3]) : 1.0;
#define SVG_CL(x) ((uint32_t)((x) < 0 ? 0 : (x) > 255 ? 255 : (x) + 0.5))
        *out = SVG_CL(a * 255.0) << 24 | SVG_CL(r) << 16 | SVG_CL(g) << 8 | SVG_CL(b);
#undef SVG_CL
        return true;
    }
    if (!strcmp(buf, "transparent")) { *out = 0; return true; }
    for (size_t i = 0; i < sizeof svg_names / sizeof *svg_names; i++)
        if (!strcmp(buf, svg_names[i].name)) { *out = 0xFF000000u | svg_names[i].rgb; return true; }
    return false;
}

/* ---------------------------------------------------------------- paths */

typedef struct { char op; double p[6]; } svg_seg_t;     /* M L Q C Z, absolute */
typedef struct { svg_seg_t* s; int n, cap; } svg_path_t;

static void svg_seg(svg_path_t* P, char op, const double* p, int np) {
    if (P->n == P->cap) { P->cap = P->cap ? P->cap * 2 : 16; P->s = svg_xrealloc(P->s, (size_t)P->cap * sizeof *P->s); }
    svg_seg_t* s = &P->s[P->n++];
    memset(s, 0, sizeof *s);
    s->op = op;
    for (int i = 0; i < np; i++) s->p[i] = p[i];
}
static void svg_M(svg_path_t* P, double x, double y) { double p[2] = { x, y }; svg_seg(P, 'M', p, 2); }
static void svg_L(svg_path_t* P, double x, double y) { double p[2] = { x, y }; svg_seg(P, 'L', p, 2); }
static void svg_Q(svg_path_t* P, double a, double b, double x, double y) { double p[4] = { a, b, x, y }; svg_seg(P, 'Q', p, 4); }
static void svg_C(svg_path_t* P, double a, double b, double c, double d, double x, double y) {
    double p[6] = { a, b, c, d, x, y };
    svg_seg(P, 'C', p, 6);
}
static void svg_Z(svg_path_t* P) { svg_seg(P, 'Z', NULL, 0); }

static double svg_angle(double ux, double uy, double vx, double vy) {
    double dot = ux * vx + uy * vy, len = sqrt(ux * ux + uy * uy) * sqrt(vx * vx + vy * vy);
    double a = acos(fmax(-1.0, fmin(1.0, len > 0 ? dot / len : 1.0)));
    return ux * vy - uy * vx < 0 ? -a : a;
}

/* SVG elliptical arc (implementation notes F.6) as up to four cubics per quarter turn. */
static void svg_arc(svg_path_t* P, double x1, double y1, double rx, double ry, double deg, int fa, int fs,
                    double x2, double y2) {
    if (x1 == x2 && y1 == y2) return;
    rx = fabs(rx); ry = fabs(ry);
    if (rx == 0 || ry == 0) { svg_L(P, x2, y2); return; }
    double phi = deg * SVG_PI / 180.0, cp = cos(phi), sp = sin(phi);
    double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { double s = sqrt(lam); rx *= s; ry *= s; }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = (fa == fs ? -1 : 1) * sqrt(fmax(0, den > 0 ? num / den : 0));
    double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    double th1 = svg_angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    double dth = svg_angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!fs && dth > 0) dth -= 2 * SVG_PI;
    if (fs && dth < 0) dth += 2 * SVG_PI;
    int n = (int)ceil(fabs(dth) / (SVG_PI / 2) - 1e-9);
    if (n < 1) n = 1;
    double delta = dth / n, t = 4.0 / 3.0 * tan(delta / 4);
    for (int i = 0; i < n; i++) {
        double a1 = th1 + i * delta, a2 = a1 + delta;
        double c1 = cos(a1), s1 = sin(a1), c2 = cos(a2), s2 = sin(a2);
        double p1x = cx + rx * c1 * cp - ry * s1 * sp, p1y = cy + rx * c1 * sp + ry * s1 * cp;
        double p2x = cx + rx * c2 * cp - ry * s2 * sp, p2y = cy + rx * c2 * sp + ry * s2 * cp;
        double d1x = -rx * s1 * cp - ry * c1 * sp, d1y = -rx * s1 * sp + ry * c1 * cp;
        double d2x = -rx * s2 * cp - ry * c2 * sp, d2y = -rx * s2 * sp + ry * c2 * cp;
        svg_C(P, p1x + t * d1x, p1y + t * d1y, p2x - t * d2x, p2y - t * d2y, i == n - 1 ? x2 : p2x, i == n - 1 ? y2 : p2y);
    }
}

static bool svg_flag(const char** pp, int* f) {
    const char* p = svg_skip(*pp);
    if (*p != '0' && *p != '1') return false;
    *f = *p - '0';
    *pp = p + 1;
    return true;
}

/* The d attribute. Returns false (keeping what was read) on a syntax error. */
static bool svg_parse_d(const char* d, svg_path_t* P) {
    const char* p = d;
    double cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0;   /* current, subpath start, last control point */
    char cmd = 0, prev = 0;
    for (;;) {
        p = svg_skip(p);
        if (!*p) return true;
        if (isalpha((unsigned char)*p)) cmd = *p++;
        else if (!cmd || cmd == 'z' || cmd == 'Z') return false;
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        if (!prev && cmd != 'M' && cmd != 'm') return false;
        bool rel = islower((unsigned char)cmd) != 0;
        char up = (char)toupper((unsigned char)cmd);
        double v[7];
        int need = up == 'M' || up == 'L' || up == 'T' ? 2 : up == 'H' || up == 'V' ? 1 : up == 'C' ? 6 : up == 'S' || up == 'Q' ? 4 : up == 'A' ? 7 : 0;
        if (up != 'Z' && up != 'M' && up != 'L' && up != 'H' && up != 'V' && up != 'C' && up != 'S' && up != 'Q' && up != 'T' && up != 'A') return false;
        for (int i = 0; i < need; i++) {
            if (up == 'A' && (i == 3 || i == 4)) { int f; if (!svg_flag(&p, &f)) return false; v[i] = f; }
            else if (!svg_num(&p, &v[i])) return false;
        }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (up) {
        case 'M': cx = v[0] + ox; cy = v[1] + oy; sx = cx; sy = cy; svg_M(P, cx, cy); break;
        case 'L': cx = v[0] + ox; cy = v[1] + oy; svg_L(P, cx, cy); break;
        case 'H': cx = v[0] + ox; svg_L(P, cx, cy); break;
        case 'V': cy = v[0] + oy; svg_L(P, cx, cy); break;
        case 'C': svg_C(P, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy);
                  lx = v[2] + ox; ly = v[3] + oy; cx = v[4] + ox; cy = v[5] + oy; break;
        case 'S': {
            double x1 = cx, y1 = cy;
            if (prev == 'C' || prev == 'S') { x1 = 2 * cx - lx; y1 = 2 * cy - ly; }
            svg_C(P, x1, y1, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
            lx = v[0] + ox; ly = v[1] + oy; cx = v[2] + ox; cy = v[3] + oy; break;
        }
        case 'Q': svg_Q(P, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
                  lx = v[0] + ox; ly = v[1] + oy; cx = v[2] + ox; cy = v[3] + oy; break;
        case 'T': {
            double x1 = cx, y1 = cy;
            if (prev == 'Q' || prev == 'T') { x1 = 2 * cx - lx; y1 = 2 * cy - ly; }
            svg_Q(P, x1, y1, v[0] + ox, v[1] + oy);
            lx = x1; ly = y1; cx = v[0] + ox; cy = v[1] + oy; break;
        }
        case 'A': svg_arc(P, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], v[5] + ox, v[6] + oy);
                  cx = v[5] + ox; cy = v[6] + oy; break;
        default: svg_Z(P); cx = sx; cy = sy; break;
        }
        prev = up;
    }
}

/* Bounding box of the path (curves sampled). */
static bool svg_bbox(const svg_path_t* P, double* x0, double* y0, double* x1, double* y1) {
    bool any = false;
    double cx = 0, cy = 0, sx = 0, sy = 0;
#define ADD(px, py) do { \
        double X = (px), Y = (py); \
        if (!any || X < *x0) *x0 = X; \
        if (!any || X > *x1) *x1 = X; \
        if (!any || Y < *y0) *y0 = Y; \
        if (!any || Y > *y1) *y1 = Y; \
        any = true; \
    } while (0)
    for (int i = 0; i < P->n; i++) {
        const double* p = P->s[i].p;
        switch (P->s[i].op) {
        case 'M': cx = sx = p[0]; cy = sy = p[1]; ADD(cx, cy); break;
        case 'L': cx = p[0]; cy = p[1]; ADD(cx, cy); break;
        case 'Q':
            for (int k = 1; k <= 32; k++) {
                double t = k / 32.0, u = 1 - t;
                ADD(u * u * cx + 2 * u * t * p[0] + t * t * p[2], u * u * cy + 2 * u * t * p[1] + t * t * p[3]);
            }
            cx = p[2]; cy = p[3]; break;
        case 'C':
            for (int k = 1; k <= 32; k++) {
                double t = k / 32.0, u = 1 - t;
                ADD(u * u * u * cx + 3 * u * u * t * p[0] + 3 * u * t * t * p[2] + t * t * t * p[4],
                    u * u * u * cy + 3 * u * u * t * p[1] + 3 * u * t * t * p[3] + t * t * t * p[5]);
            }
            cx = p[4]; cy = p[5]; break;
        default: cx = sx; cy = sy; break;
        }
    }
#undef ADD
    return any;
}

static svg_path_t svg_path_transform(const svg_path_t* P, svg_mat_t m) {
    svg_path_t R = { NULL, 0, 0 };
    for (int i = 0; i < P->n; i++) {
        svg_seg_t s = P->s[i];
        int np = s.op == 'Q' ? 2 : s.op == 'C' ? 3 : s.op == 'Z' ? 0 : 1;
        for (int k = 0; k < np; k++) svg_apply(m, P->s[i].p[2 * k], P->s[i].p[2 * k + 1], &s.p[2 * k], &s.p[2 * k + 1]);
        svg_seg(&R, s.op, s.p, 6);
    }
    return R;
}

/* ---------------------------------------------------------------- style */

enum { SP_NONE, SP_COLOR, SP_CURRENT, SP_URL };
typedef struct { int kind; uint32_t argb; char url[96]; uint32_t fallback; int fallback_kind; } svg_spec_t;

typedef struct {
    svg_spec_t fill, stroke;
    double fill_op, stroke_op, stroke_w, miter;
    int cap, join;
    bool evenodd, color_set, hidden;
    uint32_t color;
    double opacity_local;        /* this element's own, not inherited */
} svg_style_t;

static svg_style_t svg_style_default(void) {
    svg_style_t s;
    memset(&s, 0, sizeof s);
    s.fill.kind = SP_COLOR; s.fill.argb = 0xFF000000u;
    s.fill_op = s.stroke_op = 1.0; s.stroke_w = 1.0; s.miter = 4.0;
    s.cap = MIMG_CAP_BUTT; s.join = MIMG_JOIN_MITER;
    s.color = 0xFF000000u;
    s.opacity_local = 1.0;
    return s;
}

static void svg_parse_spec(const char* v, svg_spec_t* sp) {
    while (isspace((unsigned char)*v)) v++;
    uint32_t c;
    if (!strncmp(v, "none", 4)) { sp->kind = SP_NONE; return; }
    if (svg_istarts(v, "currentcolor")) { sp->kind = SP_CURRENT; return; }
    if (!strncmp(v, "url(", 4)) {
        const char* p = v + 4;
        while (*p == ' ' || *p == '\'' || *p == '"') p++;
        if (*p == '#') p++;
        size_t n = 0;
        while (p[n] && p[n] != ')' && p[n] != '\'' && p[n] != '"' && n < sizeof sp->url - 1) n++;
        memcpy(sp->url, p, n);
        sp->url[n] = 0;
        sp->kind = SP_URL;
        const char* q = strchr(p, ')');
        sp->fallback_kind = SP_NONE;
        if (q) {
            q++;
            while (isspace((unsigned char)*q)) q++;
            if (!strncmp(q, "none", 4)) sp->fallback_kind = SP_NONE;
            else if (svg_istarts(q, "currentcolor")) sp->fallback_kind = SP_CURRENT;
            else if (*q && svg_color(q, &c)) { sp->fallback_kind = SP_COLOR; sp->fallback = c; }
        }
        return;
    }
    if (svg_color(v, &c)) { sp->kind = SP_COLOR; sp->argb = c; return; }
    svg_warn("unknown paint \"%s\"", v);
}

static double svg_opacity(const char* v) {
    const char* p = v;
    double d;
    if (!svg_num(&p, &d)) return 1.0;
    if (*p == '%') d /= 100.0;
    return d < 0 ? 0 : d > 1 ? 1 : d;
}

static void svg_apply_prop(svg_style_t* s, const char* name, const char* v, double vbsize) {
    uint32_t c;
    if (!strcmp(name, "fill")) svg_parse_spec(v, &s->fill);
    else if (!strcmp(name, "stroke")) svg_parse_spec(v, &s->stroke);
    else if (!strcmp(name, "fill-opacity")) s->fill_op = svg_opacity(v);
    else if (!strcmp(name, "stroke-opacity")) s->stroke_op = svg_opacity(v);
    else if (!strcmp(name, "opacity")) s->opacity_local = svg_opacity(v);
    else if (!strcmp(name, "stroke-width")) { s->stroke_w = svg_length(v, vbsize, 1.0); if (s->stroke_w < 0) s->stroke_w = 0; }
    else if (!strcmp(name, "stroke-miterlimit")) { s->miter = svg_length(v, 1, 4.0); if (s->miter < 1) s->miter = 1; }
    else if (!strcmp(name, "stroke-linecap")) s->cap = !strncmp(v, "round", 5) ? MIMG_CAP_ROUND : !strncmp(v, "square", 6) ? MIMG_CAP_SQUARE : MIMG_CAP_BUTT;
    else if (!strcmp(name, "stroke-linejoin")) s->join = !strncmp(v, "round", 5) ? MIMG_JOIN_ROUND : !strncmp(v, "bevel", 5) ? MIMG_JOIN_BEVEL : MIMG_JOIN_MITER;
    else if (!strcmp(name, "fill-rule")) s->evenodd = !strncmp(v, "evenodd", 7);
    else if (!strcmp(name, "color")) { if (svg_color(v, &c)) { s->color = c; s->color_set = true; } }
    else if (!strcmp(name, "display")) s->hidden = !strncmp(v, "none", 4);
    else if (!strcmp(name, "visibility")) s->hidden = !strncmp(v, "hidden", 6) || !strncmp(v, "collapse", 8);
    else if (!strcmp(name, "stroke-dasharray")) { if (strncmp(v, "none", 4)) svg_warn("stroke-dasharray is not supported (stroke drawn solid)"); }
    else if (!strcmp(name, "clip-path") || !strcmp(name, "mask") || !strcmp(name, "filter")) {
        if (strncmp(v, "none", 4)) svg_warn("%s is not supported (ignored)", name);
    }
}

static const char* svg_style_props[] = { "fill", "stroke", "fill-opacity", "stroke-opacity", "opacity", "stroke-width",
    "stroke-miterlimit", "stroke-linecap", "stroke-linejoin", "fill-rule", "color", "display", "visibility",
    "stroke-dasharray", "clip-path", "mask", "filter" };

/* Presentation attributes, then style="" on top. */
static void svg_apply_element(svg_style_t* s, const svg_node_t* n, double vbsize) {
    s->opacity_local = 1.0;
    for (size_t i = 0; i < sizeof svg_style_props / sizeof *svg_style_props; i++) {
        const char* v = svg_attr(n, svg_style_props[i]);
        if (v) svg_apply_prop(s, svg_style_props[i], v, vbsize);
    }
    const char* st = svg_attr(n, "style");
    if (!st) return;
    char* copy = svg_strndup(st, strlen(st));
    for (char* item = strtok(copy, ";"); item; item = strtok(NULL, ";")) {
        char* colon = strchr(item, ':');
        if (!colon) continue;
        *colon = 0;
        char* name = item;
        char* val = colon + 1;
        while (isspace((unsigned char)*name)) name++;
        for (char* e = name + strlen(name); e > name && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*val)) val++;
        char* imp = strstr(val, "!important");
        if (imp) *imp = 0;
        svg_apply_prop(s, name, val, vbsize);
    }
    free(copy);
}

/* -------------------------------------------------------------- paints */

typedef struct {
    int type;                    /* MIMG_PAINT_* */
    uint32_t argb;               /* solid color, or current: alpha << 24 */
    uint32_t spread;
    int nstops;
    uint8_t off[8];
    uint32_t col[8];
    float g[5];
    bool has_matrix;
    float m[6];
} svg_pdesc_t;

typedef struct {
    svg_path_t path;
    int fill, stroke;            /* paint index or -1 */
    bool evenodd;
    uint8_t fill_op, stroke_op, cap, join;
    double width, miter;
    bool has_xf;
    float xf[6];
} svg_shape_t;

typedef struct {
    svg_node_t* root;
    svg_node_t** ids_node;
    char** ids;
    int nids;
    svg_pdesc_t* paints;
    int npaints;
    svg_shape_t* shapes;
    int nshapes;
    double vbx, vby, vbw, vbh;
    int depth;
} svg_doc_t;

static svg_node_t* svg_by_id(const svg_doc_t* D, const char* id) {
    for (int i = 0; i < D->nids; i++) if (!strcmp(D->ids[i], id)) return D->ids_node[i];
    return NULL;
}

static void svg_index_ids(svg_doc_t* D, svg_node_t* n) {
    const char* id = svg_attr(n, "id");
    if (id) {
        D->ids = svg_xrealloc(D->ids, (size_t)(D->nids + 1) * sizeof *D->ids);
        D->ids_node = svg_xrealloc(D->ids_node, (size_t)(D->nids + 1) * sizeof *D->ids_node);
        D->ids[D->nids] = (char*)id;
        D->ids_node[D->nids++] = n;
    }
    for (int i = 0; i < n->nkids; i++) svg_index_ids(D, n->kids[i]);
}

static int svg_add_paint(svg_doc_t* D, const svg_pdesc_t* p) {
    for (int i = 0; i < D->npaints; i++) if (!memcmp(&D->paints[i], p, sizeof *p)) return i;
    if (D->npaints >= (int)MIMG_VEC_MAX_PAINTS) { svg_warn("too many paints (more than %u), using black", MIMG_VEC_MAX_PAINTS); return 0; }
    D->paints = svg_xrealloc(D->paints, (size_t)(D->npaints + 1) * sizeof *D->paints);
    D->paints[D->npaints] = *p;
    return D->npaints++;
}

/* The first attribute `name` along the href chain of a gradient. */
static const char* svg_grad_attr(const svg_doc_t* D, const svg_node_t* g, const char* name) {
    for (int depth = 0; g && depth < 8; depth++) {
        const char* v = svg_attr(g, name);
        if (v) return v;
        const char* href = svg_attr(g, "href");
        g = href && href[0] == '#' ? svg_by_id(D, href + 1) : NULL;
    }
    return NULL;
}
static const svg_node_t* svg_grad_with_stops(const svg_doc_t* D, const svg_node_t* g) {
    for (int depth = 0; g && depth < 8; depth++) {
        for (int i = 0; i < g->nkids; i++) if (!strcmp(g->kids[i]->name, "stop")) return g;
        const char* href = svg_attr(g, "href");
        g = href && href[0] == '#' ? svg_by_id(D, href + 1) : NULL;
    }
    return NULL;
}

static double svg_grad_coord(const char* s, bool obb, double ref, double dflt) {
    if (!s) return dflt;
    const char* p = s;
    double v;
    if (!svg_num(&p, &v)) return dflt;
    if (*p == '%') return v / 100.0 * (obb ? 1.0 : ref);
    return obb ? v : svg_length(s, ref, dflt);
}

static uint32_t svg_mul_alpha(uint32_t argb, double k) {
    double a = (double)(argb >> 24) * k + 0.5;
    return (a < 0 ? 0u : a > 255 ? 255u : (uint32_t)a) << 24 | (argb & 0xFFFFFFu);
}

/* A paint for a spec on a shape (-1: nothing to paint). `m` takes the shape's path coordinates to
 * user space (identity when the path is already in it), `bb` is the path's box in those coordinates. */
static int svg_make_paint(svg_doc_t* D, const svg_style_t* st, const svg_spec_t* sp, svg_mat_t ctm, bool baked,
                          const double* bb, bool has_bb) {
    svg_pdesc_t p;
    memset(&p, 0, sizeof p);
    int kind = sp->kind;
    uint32_t color = sp->argb;
    if (kind == SP_URL) {
        svg_node_t* g = svg_by_id(D, sp->url);
        if (!g || (strcmp(g->name, "linearGradient") && strcmp(g->name, "radialGradient"))) {
            svg_warn("paint server #%s not found", sp->url);
            kind = sp->fallback_kind; color = sp->fallback;
        } else {
            bool radial = !strcmp(g->name, "radialGradient");
            const char* units = svg_grad_attr(D, g, "gradientUnits");
            bool obb = !units || strcmp(units, "userSpaceOnUse") != 0;
            const svg_node_t* sn = svg_grad_with_stops(D, g);
            svg_pdesc_t gp;
            memset(&gp, 0, sizeof gp);
            int ns = 0;
            double prev = 0;
            uint32_t cols[64];
            double offs[64];
            for (int i = 0; sn && i < sn->nkids && ns < 64; i++) {
                const svg_node_t* k = sn->kids[i];
                if (strcmp(k->name, "stop")) continue;
                svg_style_t ss = svg_style_default();
                double stop_op = 1.0;
                uint32_t sc = 0xFF000000u;
                const char* av = svg_attr(k, "stop-color");
                const char* ao = svg_attr(k, "stop-opacity");
                const char* st2 = svg_attr(k, "style");
                (void)ss;
                if (av) svg_color(av, &sc);
                if (ao) stop_op = svg_opacity(ao);
                if (st2) {
                    char* copy = svg_strndup(st2, strlen(st2));
                    for (char* it = strtok(copy, ";"); it; it = strtok(NULL, ";")) {
                        char* colon = strchr(it, ':');
                        if (!colon) continue;
                        *colon = 0;
                        char* nm = it;
                        while (isspace((unsigned char)*nm)) nm++;
                        for (char* e = nm + strlen(nm); e > nm && isspace((unsigned char)e[-1]);) *--e = 0;
                        if (!strcmp(nm, "stop-color")) svg_color(colon + 1, &sc);
                        else if (!strcmp(nm, "stop-opacity")) stop_op = svg_opacity(colon + 1);
                    }
                    free(copy);
                }
                double o = svg_grad_coord(svg_attr(k, "offset"), true, 1, 0);
                o = o < 0 ? 0 : o > 1 ? 1 : o;
                if (o < prev) o = prev;
                prev = o;
                offs[ns] = o;
                cols[ns] = svg_mul_alpha(sc, stop_op);
                ns++;
            }
            if (ns == 0) return -1;
            if (ns == 1) { kind = SP_COLOR; color = cols[0]; goto solid; }
            if (ns > 8) { svg_warn("gradient with %d stops: using 8", ns); }
            int n8 = ns > 8 ? 8 : ns;
            for (int i = 0; i < n8; i++) {
                int j = ns > 8 ? (int)((double)i * (ns - 1) / (n8 - 1) + 0.5) : i;
                gp.off[i] = (uint8_t)(offs[j] * 255.0 + 0.5);
                gp.col[i] = cols[j];
            }
            gp.nstops = n8;
            const char* spread = svg_grad_attr(D, g, "spreadMethod");
            gp.spread = spread && !strcmp(spread, "reflect") ? MIMG_SPREAD_REFLECT : spread && !strcmp(spread, "repeat") ? MIMG_SPREAD_REPEAT : MIMG_SPREAD_PAD;
            gp.type = radial ? MIMG_PAINT_RADIAL : MIMG_PAINT_LINEAR;
            double rx = D->vbw, ry = D->vbh, rd = sqrt((rx * rx + ry * ry) / 2);
            if (!radial) {
                double x1 = svg_grad_coord(svg_grad_attr(D, g, "x1"), obb, rx, 0), y1 = svg_grad_coord(svg_grad_attr(D, g, "y1"), obb, ry, 0);
                double x2 = svg_grad_coord(svg_grad_attr(D, g, "x2"), obb, rx, obb ? 1 : rx), y2 = svg_grad_coord(svg_grad_attr(D, g, "y2"), obb, ry, 0);
                gp.g[0] = (float)x1; gp.g[1] = (float)y1; gp.g[2] = (float)x2; gp.g[3] = (float)y2;
            } else {
                double cx = svg_grad_coord(svg_grad_attr(D, g, "cx"), obb, rx, obb ? 0.5 : rx / 2);
                double cy = svg_grad_coord(svg_grad_attr(D, g, "cy"), obb, ry, obb ? 0.5 : ry / 2);
                double r = svg_grad_coord(svg_grad_attr(D, g, "r"), obb, rd, obb ? 0.5 : rd / 2);
                double fx = svg_grad_coord(svg_grad_attr(D, g, "fx"), obb, rx, cx), fy = svg_grad_coord(svg_grad_attr(D, g, "fy"), obb, ry, cy);
                gp.g[0] = (float)cx; gp.g[1] = (float)cy; gp.g[2] = (float)r; gp.g[3] = (float)fx; gp.g[4] = (float)fy;
            }
            svg_mat_t m = baked ? ctm : SVG_I;
            if (obb) {
                if (!has_bb || bb[2] - bb[0] <= 0 || bb[3] - bb[1] <= 0) return -1;     /* no area: nothing is painted */
                svg_mat_t bm = { bb[2] - bb[0], 0, 0, bb[3] - bb[1], bb[0], bb[1] };
                m = svg_mul(m, bm);
            }
            const char* gt = svg_grad_attr(D, g, "gradientTransform");
            if (gt) m = svg_mul(m, svg_parse_transform(gt));
            if (m.a != 1 || m.b != 0 || m.c != 0 || m.d != 1 || m.e != 0 || m.f != 0) {
                gp.has_matrix = true;
                gp.m[0] = (float)m.a; gp.m[1] = (float)m.b; gp.m[2] = (float)m.c;
                gp.m[3] = (float)m.d; gp.m[4] = (float)m.e; gp.m[5] = (float)m.f;
            }
            return svg_add_paint(D, &gp);
        }
    }
solid:
    if (kind == SP_NONE || kind == SP_URL) return -1;
    if (kind == SP_CURRENT) {
        if (st->color_set) { p.type = MIMG_PAINT_SOLID; p.argb = st->color; }
        else { p.type = MIMG_PAINT_CURRENT; p.argb = 0xFFu << 24; }
    } else {
        p.type = MIMG_PAINT_SOLID; p.argb = color;
    }
    if (p.type == MIMG_PAINT_SOLID && (p.argb >> 24) == 0) return -1;
    return svg_add_paint(D, &p);
}

/* -------------------------------------------------------------- shapes */

static uint8_t svg_u8(double v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0 + 0.5); }

static void svg_emit(svg_doc_t* D, svg_path_t path, const svg_style_t* st, double group_opacity, svg_mat_t ctm,
                     const char* what) {
    if (!path.n || st->hidden) return;
    bool has_fill = st->fill.kind != SP_NONE, has_stroke = st->stroke.kind != SP_NONE && st->stroke_w > 0;
    if (!has_fill && !has_stroke) return;
    double det = ctm.a * ctm.d - ctm.b * ctm.c;
    if (!(fabs(det) > 1e-12)) return;
    double s2x = ctm.a * ctm.a + ctm.b * ctm.b, s2y = ctm.c * ctm.c + ctm.d * ctm.d;
    bool sim = fabs(s2x - s2y) <= 1e-4 * fmax(s2x, s2y) && fabs(ctm.a * ctm.c + ctm.b * ctm.d) <= 1e-4 * fmax(s2x, s2y);
    bool baked = !has_stroke || sim;
    double bb[4];
    bool has_bb = svg_bbox(&path, &bb[0], &bb[1], &bb[2], &bb[3]);
    svg_shape_t sh;
    memset(&sh, 0, sizeof sh);
    sh.fill = has_fill ? svg_make_paint(D, st, &st->fill, ctm, baked, bb, has_bb) : -1;
    sh.stroke = has_stroke ? svg_make_paint(D, st, &st->stroke, ctm, baked, bb, has_bb) : -1;
    if (sh.fill < 0 && sh.stroke < 0) return;
    sh.evenodd = st->evenodd;
    sh.fill_op = svg_u8(st->fill_op * group_opacity);
    sh.stroke_op = svg_u8(st->stroke_op * group_opacity);
    sh.cap = (uint8_t)st->cap; sh.join = (uint8_t)st->join;
    sh.width = st->stroke_w; sh.miter = st->miter;
    if (baked) {
        sh.path = svg_path_transform(&path, ctm);
        sh.width *= sqrt(s2x);
    } else {
        sh.path = path;
        sh.has_xf = true;
        sh.xf[0] = (float)ctm.a; sh.xf[1] = (float)ctm.b; sh.xf[2] = (float)ctm.c;
        sh.xf[3] = (float)ctm.d; sh.xf[4] = (float)ctm.e; sh.xf[5] = (float)ctm.f;
    }
    if (D->nshapes >= (int)MIMG_VEC_MAX_SHAPES) { svg_warn("too many shapes, the rest is dropped"); return; }
    (void)what;
    D->shapes = svg_xrealloc(D->shapes, (size_t)(D->nshapes + 1) * sizeof *D->shapes);
    D->shapes[D->nshapes++] = sh;
}

static void svg_rect_path(svg_path_t* P, double x, double y, double w, double h, double rx, double ry) {
    if (rx <= 0 || ry <= 0) {
        svg_M(P, x, y); svg_L(P, x + w, y); svg_L(P, x + w, y + h); svg_L(P, x, y + h); svg_Z(P);
        return;
    }
    const double k = 0.5522847498307936;
    svg_M(P, x + rx, y);
    svg_L(P, x + w - rx, y);
    svg_C(P, x + w - rx + k * rx, y, x + w, y + ry - k * ry, x + w, y + ry);
    svg_L(P, x + w, y + h - ry);
    svg_C(P, x + w, y + h - ry + k * ry, x + w - rx + k * rx, y + h, x + w - rx, y + h);
    svg_L(P, x + rx, y + h);
    svg_C(P, x + rx - k * rx, y + h, x, y + h - ry + k * ry, x, y + h - ry);
    svg_L(P, x, y + ry);
    svg_C(P, x, y + ry - k * ry, x + rx - k * rx, y, x + rx, y);
    svg_Z(P);
}

static void svg_ellipse_path(svg_path_t* P, double cx, double cy, double rx, double ry) {
    const double k = 0.5522847498307936;
    svg_M(P, cx + rx, cy);
    svg_C(P, cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry);
    svg_C(P, cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy);
    svg_C(P, cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry);
    svg_C(P, cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy);
    svg_Z(P);
}

static void svg_points_path(svg_path_t* P, const char* s, bool close) {
    const char* p = s;
    double x, y;
    int n = 0;
    while (svg_num(&p, &x) && svg_num(&p, &y)) { if (n++) svg_L(P, x, y); else svg_M(P, x, y); }
    if (close && n) svg_Z(P);
}

static void svg_walk(svg_doc_t* D, const svg_node_t* n, const svg_style_t* parent, svg_mat_t ctm, double gop) {
    if (D->depth > 24) { svg_warn("elements nested too deeply"); return; }
    const char* nm = n->name;
    if (!strcmp(nm, "defs") || !strcmp(nm, "linearGradient") || !strcmp(nm, "radialGradient") || !strcmp(nm, "symbol") ||
        !strcmp(nm, "title") || !strcmp(nm, "desc") || !strcmp(nm, "metadata") || !strcmp(nm, "stop") ||
        !strcmp(nm, "namedview") || !strcmp(nm, "style") || !strcmp(nm, "clipPath") || !strcmp(nm, "mask") ||
        !strcmp(nm, "filter") || !strcmp(nm, "pattern") || !strcmp(nm, "marker")) {
        if (!strcmp(nm, "style")) svg_warn("<style> sheets are not supported (class selectors ignored)");
        if (!strcmp(nm, "clipPath") || !strcmp(nm, "mask") || !strcmp(nm, "filter") || !strcmp(nm, "pattern") || !strcmp(nm, "marker"))
            svg_warn("<%s> is not supported (ignored)", nm);
        return;
    }
    svg_style_t st = *parent;
    svg_apply_element(&st, n, D->vbw > D->vbh ? D->vbw : D->vbh);
    if (st.hidden && !strcmp(nm, "g")) return;
    double op = gop * st.opacity_local;
    svg_mat_t m = svg_mul(ctm, svg_parse_transform(svg_attr(n, "transform")));
    double vb = D->vbw > D->vbh ? D->vbw : D->vbh;
    svg_path_t P = { NULL, 0, 0 };
    if (!strcmp(nm, "g") || !strcmp(nm, "a") || !strcmp(nm, "switch") || !strcmp(nm, "svg")) {
        if (!strcmp(nm, "svg") && n != D->root) svg_warn("nested <svg> treated as a group");
        D->depth++;
        for (int i = 0; i < n->nkids; i++) svg_walk(D, n->kids[i], &st, m, op);
        D->depth--;
        return;
    }
    if (!strcmp(nm, "use")) {
        const char* href = svg_attr(n, "href");
        svg_node_t* t = href && href[0] == '#' ? svg_by_id(D, href + 1) : NULL;
        if (!t) { svg_warn("<use> target not found"); return; }
        svg_mat_t um = svg_mul(m, (svg_mat_t){ 1, 0, 0, 1, svg_length(svg_attr(n, "x"), D->vbw, 0), svg_length(svg_attr(n, "y"), D->vbh, 0) });
        D->depth++;
        if (!strcmp(t->name, "symbol")) { for (int i = 0; i < t->nkids; i++) svg_walk(D, t->kids[i], &st, um, op); }
        else svg_walk(D, t, &st, um, op);
        D->depth--;
        return;
    }
    if (!strcmp(nm, "path")) {
        const char* d = svg_attr(n, "d");
        if (d && !svg_parse_d(d, &P)) svg_warn("path data has a syntax error (drawn up to it)");
    } else if (!strcmp(nm, "rect")) {
        double x = svg_length(svg_attr(n, "x"), D->vbw, 0), y = svg_length(svg_attr(n, "y"), D->vbh, 0);
        double w = svg_length(svg_attr(n, "width"), D->vbw, 0), h = svg_length(svg_attr(n, "height"), D->vbh, 0);
        const char* arx = svg_attr(n, "rx");
        const char* ary = svg_attr(n, "ry");
        double rx = svg_length(arx, D->vbw, -1), ry = svg_length(ary, D->vbh, -1);
        if (rx < 0 && ry < 0) rx = ry = 0; else if (rx < 0) rx = ry; else if (ry < 0) ry = rx;
        rx = fmin(rx, w / 2); ry = fmin(ry, h / 2);
        if (w > 0 && h > 0) svg_rect_path(&P, x, y, w, h, rx, ry);
    } else if (!strcmp(nm, "circle")) {
        double r = svg_length(svg_attr(n, "r"), sqrt((D->vbw * D->vbw + D->vbh * D->vbh) / 2), 0);
        if (r > 0) svg_ellipse_path(&P, svg_length(svg_attr(n, "cx"), D->vbw, 0), svg_length(svg_attr(n, "cy"), D->vbh, 0), r, r);
    } else if (!strcmp(nm, "ellipse")) {
        double rx = svg_length(svg_attr(n, "rx"), D->vbw, 0), ry = svg_length(svg_attr(n, "ry"), D->vbh, 0);
        if (rx > 0 && ry > 0) svg_ellipse_path(&P, svg_length(svg_attr(n, "cx"), D->vbw, 0), svg_length(svg_attr(n, "cy"), D->vbh, 0), rx, ry);
    } else if (!strcmp(nm, "line")) {
        svg_M(&P, svg_length(svg_attr(n, "x1"), D->vbw, 0), svg_length(svg_attr(n, "y1"), D->vbh, 0));
        svg_L(&P, svg_length(svg_attr(n, "x2"), D->vbw, 0), svg_length(svg_attr(n, "y2"), D->vbh, 0));
        st.fill.kind = SP_NONE;
    } else if (!strcmp(nm, "polyline")) {
        const char* pts = svg_attr(n, "points");
        if (pts) svg_points_path(&P, pts, false);
    } else if (!strcmp(nm, "polygon")) {
        const char* pts = svg_attr(n, "points");
        if (pts) svg_points_path(&P, pts, true);
    } else {
        svg_warn("<%s> is not supported (ignored)", nm);
        return;
    }
    (void)vb;
    svg_emit(D, P, &st, op, m, nm);
}

/* ------------------------------------------------------------- the file */

/* Converts SVG text to a vector .img in a malloc'd buffer; NULL (after a message) on failure. */
static uint8_t* svg_to_img(const char* text, size_t len, int shift_override, size_t* out_size) {
    svg_node_t* root = svg_parse_xml(text, len);
    if (!root || strcmp(root->name, "svg")) { fprintf(stderr, "mosmedia: not an SVG file (no <svg> element)\n"); return NULL; }
    svg_doc_t D;
    memset(&D, 0, sizeof D);
    D.root = root;
    const char* vb = svg_attr(root, "viewBox");
    double x = 0, y = 0, w = 0, h = 0;
    bool have_vb = false;
    if (vb) {
        const char* p = vb;
        have_vb = svg_num(&p, &x) && svg_num(&p, &y) && svg_num(&p, &w) && svg_num(&p, &h) && w > 0 && h > 0;
        if (!have_vb) svg_warn("bad viewBox \"%s\"", vb);
    }
    if (!have_vb) {
        x = y = 0;
        w = svg_length(svg_attr(root, "width"), 100, 0);
        h = svg_length(svg_attr(root, "height"), 100, 0);
        if (!(w > 0) || !(h > 0)) { svg_warn("no viewBox and no usable width/height: assuming 100 x 100"); w = h = 100; }
    }
    if (w > 1e6 || h > 1e6) { fprintf(stderr, "mosmedia: view box too large\n"); return NULL; }
    D.vbx = x; D.vby = y; D.vbw = w; D.vbh = h;
    svg_index_ids(&D, root);
    svg_style_t st = svg_style_default();
    svg_walk(&D, root, &st, SVG_I, 1.0);

    size_t cap = 4096 + (size_t)D.npaints * 128;
    for (int i = 0; i < D.nshapes; i++) cap += 64 + (size_t)D.shapes[i].path.n * 40;
    uint8_t* buf = svg_xrealloc(NULL, cap);
    uint32_t shift = shift_override >= 0 ? (uint32_t)shift_override : mimg_vw_default_shift((float)w, (float)h);
    mimg_vwriter_t wr;
    if (!mimg_vw_begin(&wr, buf, cap, shift, (float)x, (float)y, (float)w, (float)h)) {
        fprintf(stderr, "mosmedia: cannot write a vector image of this size\n");
        free(buf);
        return NULL;
    }
    for (int i = 0; i < D.npaints; i++) {
        const svg_pdesc_t* p = &D.paints[i];
        if (p->type == MIMG_PAINT_SOLID) mimg_vw_paint_solid(&wr, p->argb);
        else if (p->type == MIMG_PAINT_CURRENT) mimg_vw_paint_current(&wr, (uint8_t)(p->argb >> 24));
        else mimg_vw_paint_gradient(&wr, p->type == MIMG_PAINT_RADIAL, p->spread, p->off, p->col, (uint32_t)p->nstops, p->g,
                                    p->has_matrix ? p->m : NULL);
    }
    for (int i = 0; i < D.nshapes; i++) {
        const svg_shape_t* s = &D.shapes[i];
        mimg_vstyle_t vs;
        vs.fill_paint = s->fill; vs.even_odd = s->evenodd; vs.fill_opacity = s->fill_op;
        vs.stroke_paint = s->stroke; vs.stroke_width = (float)s->width; vs.cap = s->cap; vs.join = s->join;
        vs.miter_limit = (float)s->miter; vs.stroke_opacity = s->stroke_op; vs.transform = s->has_xf ? s->xf : NULL;
        mimg_vw_shape_begin(&wr, &vs);
        bool open = false;
        for (int k = 0; k < s->path.n; k++) {
            const svg_seg_t* g = &s->path.s[k];
            if (g->op == 'M') { mimg_vw_move(&wr, (float)g->p[0], (float)g->p[1]); open = true; }
            else if (!open) continue;
            else if (g->op == 'L') mimg_vw_line(&wr, (float)g->p[0], (float)g->p[1]);
            else if (g->op == 'Q') mimg_vw_quad(&wr, (float)g->p[0], (float)g->p[1], (float)g->p[2], (float)g->p[3]);
            else if (g->op == 'C') mimg_vw_cubic(&wr, (float)g->p[0], (float)g->p[1], (float)g->p[2], (float)g->p[3], (float)g->p[4], (float)g->p[5]);
            else mimg_vw_close(&wr);
        }
        mimg_vw_shape_end(&wr);
    }
    size_t n = mimg_vw_finish(&wr);
    if (!n) { fprintf(stderr, "mosmedia: could not encode the drawing (too many shapes or paints?)\n"); free(buf); return NULL; }
    *out_size = n;
    fprintf(stderr, "mosmedia: view box %g %g %g %g, %d shapes, %d paints, shift %u\n", x, y, w, h, D.nshapes, D.npaints, shift);
    return buf;
}

#endif
