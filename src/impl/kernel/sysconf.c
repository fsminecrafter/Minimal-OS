#include "x86_64/sysconf.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "string.h"
#include "serial.h"

#define SYSCONF_PATH "0:/etc/System.conf"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// Whether the line [s, s+n) is "key=...", and where its value starts.
static bool line_is_key(const char* s, uint32_t n, const char* key, uint32_t* value_at) {
    uint32_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    uint32_t k = 0;
    while (key[k] && i < n && lower((unsigned char)s[i]) == lower((unsigned char)key[k])) { i++; k++; }
    if (key[k]) return false;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || s[i] != '=') return false;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    *value_at = i;
    return true;
}

bool sysconf_get(const char* key, char* out, size_t cap) {
    if (!key || !out || !cap) return false;
    minimafs_file_handle_t* f = minimafs_open(SYSCONF_PATH, true);
    if (!f) return false;
    const char* d = (const char*)f->data;
    uint32_t n = f->data_size;
    bool found = false;
    for (uint32_t ls = 0; d && ls < n && !found;) {
        uint32_t le = ls;
        while (le < n && d[le] != '\n') le++;
        uint32_t v;
        if (line_is_key(d + ls, le - ls, key, &v)) {
            uint32_t ve = le - ls;
            while (ve > v && (d[ls + ve - 1] == '\r' || d[ls + ve - 1] == ' ' || d[ls + ve - 1] == '\t')) ve--;
            size_t len = ve - v;
            if (len >= cap) len = cap - 1;
            memcpy(out, d + ls + v, len);
            out[len] = 0;
            found = true;
        }
        ls = le + 1;
    }
    minimafs_close(f);
    return found;
}

bool sysconf_set(const char* key, const char* value) {
    if (!key || !value) return false;
    if (!minimafs_exists(SYSCONF_PATH)) {
        if (!minimafs_exists("0:/etc")) minimafs_mkdir("0:/etc");
        if (!minimafs_create_file(SYSCONF_PATH, "text", "conf")) return false;
    }
    minimafs_file_handle_t* f = minimafs_open(SYSCONF_PATH, false);
    if (!f) return false;
    const char* d = (const char*)f->data;
    uint32_t n = f->data_size;
    size_t kl = strlen(key), vl = strlen(value);
    uint32_t cap = n + (uint32_t)(kl + vl) + 4;
    char* out = (char*)alloc_unzeroed(cap);
    if (!out) { minimafs_close(f); return false; }
    uint32_t o = 0;
    bool wrote = false;
    for (uint32_t ls = 0; d && ls < n;) {
        uint32_t le = ls;
        while (le < n && d[le] != '\n') le++;
        uint32_t v;
        bool nl = le < n;
        if (line_is_key(d + ls, le - ls, key, &v)) {
            if (!wrote) {
                memcpy(out + o, key, kl); o += (uint32_t)kl;
                out[o++] = '=';
                memcpy(out + o, value, vl); o += (uint32_t)vl;
                out[o++] = '\n';
                wrote = true;
            }
        } else {
            memcpy(out + o, d + ls, le - ls);
            o += le - ls;
            if (nl) out[o++] = '\n';
        }
        ls = le + 1;
    }
    if (!wrote) {
        if (o && out[o - 1] != '\n') out[o++] = '\n';
        memcpy(out + o, key, kl); o += (uint32_t)kl;
        out[o++] = '=';
        memcpy(out + o, value, vl); o += (uint32_t)vl;
        out[o++] = '\n';
    }
    if (f->data) free_mem(f->data);
    f->data = (uint8_t*)out;
    f->data_size = o;
    f->position = o;
    f->modified = true;
    minimafs_close(f);
    return true;
}
