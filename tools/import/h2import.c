/*
 * h2import.c - see h2import.h. Portable C99, no third-party code.
 *
 * XDVDFS (Xbox DVD filesystem) summary, from the public format description:
 *   - 2048-byte sectors, all sector numbers relative to the game partition.
 *   - Volume descriptor at sector 32: "MICROSOFT*XBOX*MEDIA" (20 bytes),
 *     u32 root directory sector, u32 root directory size, u64 FILETIME,
 *     unused bytes, and the same 20-byte magic again at offset 0x7EC.
 *   - A directory is a binary tree of entries:
 *       u16 left subtree offset  (in dwords from the directory start)
 *       u16 right subtree offset (in dwords; 0 or 0xFFFF = none)
 *       u32 start sector, u32 size, u8 attributes (0x10 = directory),
 *       u8 name length, name bytes; each entry padded to 4 bytes, padding
 *       bytes are 0xFF and entries never straddle a sector.
 *   - Disc images: an XISO starts with the partition at 0; a Redump full-disc
 *     ISO has a video partition first, the game partition at 0x18300000
 *     (XGD1), 0xFD90000 (XGD2) or 0x2080000 (XGD3).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* realpath, fseeko */
#endif

#include "h2import.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define H2_SECTOR 2048u
#define H2_COPY_BUF (4u * 1024u * 1024u)
#define H2_MAX_DEPTH 48
#define H2_MAX_DIR_BYTES (64u * 1024u * 1024u)

/* ------------------------------------------------------------------ SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(h2_sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void h2_sha256_init(h2_sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->fill = 0;
}

void h2_sha256_update(h2_sha256_ctx *c, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += n;
    if (c->fill) {
        size_t take = 64 - c->fill;
        if (take > n) take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take; p += take; n -= take;
        if (c->fill == 64) { sha256_block(c, c->buf); c->fill = 0; }
    }
    while (n >= 64) { sha256_block(c, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->fill = n; }
}

void h2_sha256_final(h2_sha256_ctx *c, unsigned char out[32]) {
    uint64_t bits = c->len * 8;
    unsigned char pad[72];
    size_t padlen = (c->fill < 56) ? (56 - c->fill) : (120 - c->fill);
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));
    h2_sha256_update(c, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(c->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)c->h[i];
    }
}

void h2_hex32(const unsigned char d[32], char hex[65]) {
    static const char dig[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { hex[2 * i] = dig[d[i] >> 4]; hex[2 * i + 1] = dig[d[i] & 15]; }
    hex[64] = 0;
}

/* ------------------------------------------------------- platform wrappers */

#ifdef _WIN32
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); w = NULL; }
    return w;
}
static char *narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) { free(s); s = NULL; }
    return s;
}
#endif

static FILE *x_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp); free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int x_seek(FILE *f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* 0 = missing, 1 = file, 2 = directory. Size returned for files. */
static int x_stat(const char *path, uint64_t *size) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    memset(&fa, 0, sizeof fa);
    wchar_t *wp = widen(path);
    BOOL ok = wp ? GetFileAttributesExW(wp, GetFileExInfoStandard, &fa) : FALSE;
    free(wp);
    if (!ok) return 0;
    if (size) *size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (size) *size = (uint64_t)st.st_size;
    return S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}

static int x_mkdir1(const char *path) {
    int r;
#ifdef _WIN32
    wchar_t *wp = widen(path);
    r = wp ? (CreateDirectoryW(wp, NULL) ? 0 : -1) : -1;
    free(wp);
#else
    r = mkdir(path, 0777);
#endif
    if (r != 0 && x_stat(path, NULL) == 2) r = 0;
    return r;
}

static int x_remove(const char *path) {
#ifdef _WIN32
    wchar_t *wp = widen(path);
    int r = wp ? (DeleteFileW(wp) ? 0 : -1) : -1;
    free(wp);
    return r;
#else
    return remove(path);
#endif
}

static int x_rename_replace(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *a = widen(from), *b = widen(to);
    int r = (a && b) ? (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : -1) : -1;
    free(a); free(b);
    return r;
#else
    return rename(from, to);
#endif
}

/* Absolute normalised path (malloc'd) or NULL. Used only to compare paths. */
static char *x_fullpath(const char *path) {
#ifdef _WIN32
    wchar_t *wp = widen(path), *full;
    char *r = NULL;
    if (!wp) return NULL;
    full = _wfullpath(NULL, wp, 0);
    free(wp);
    if (full) { r = narrow(full); free(full); }
    if (r) { size_t n = strlen(r); while (n > 3 && (r[n - 1] == '\\' || r[n - 1] == '/')) r[--n] = 0; }
    return r;
#else
#if defined(__wasi__)
    char *r = NULL; (void)path; /* no realpath in WASI; the comparison is skipped */
#else
    char *r = realpath(path, NULL);
#endif
    return r;
#endif
}

typedef int (*list_fn)(void *ctx, const char *name, int is_dir);

/* Enumerate a directory; stops and returns the callback's value if nonzero. */
static int x_listdir(const char *path, list_fn fn, void *ctx, int *opened) {
#ifdef _WIN32
    size_t n = strlen(path);
    char *pat = (char *)malloc(n + 3);
    wchar_t *wpat;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int r = 0;
    *opened = 0;
    if (!pat) return 0;
    memcpy(pat, path, n);
    memcpy(pat + n, "\\*", 3);
    wpat = widen(pat);
    free(pat);
    if (!wpat) return 0;
    h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) return 0;
    *opened = 1;
    do {
        char *name;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        name = narrow(fd.cFileName);
        if (!name) continue;
        r = fn(ctx, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
        free(name);
    } while (!r && FindNextFileW(h, &fd));
    FindClose(h);
    return r;
#else
    DIR *d = opendir(path);
    struct dirent *de;
    int r = 0;
    *opened = 0;
    if (!d) return 0;
    *opened = 1;
    while (!r && (de = readdir(d)) != NULL) {
        size_t n = strlen(path) + strlen(de->d_name) + 2;
        char *full;
        int kind;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        full = (char *)malloc(n);
        if (!full) break;
        snprintf(full, n, "%s/%s", path, de->d_name);
        kind = x_stat(full, NULL);
        free(full);
        if (kind == 0) continue;
        r = fn(ctx, de->d_name, kind == 2);
    }
    closedir(d);
    return r;
#endif
}

/* ------------------------------------------------------------- utilities */

static void seterr(h2_import_result *res, const char *fmt, ...) {
    va_list ap;
    if (!res) return;
    va_start(ap, fmt);
    vsnprintf(res->err, sizeof res->err, fmt, ap);
    va_end(ap);
}

static char *join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *r = (char *)malloc(na + nb + 2);
    size_t i;
    if (!r) return NULL;
    memcpy(r, a, na);
    while (na > 0 && (r[na - 1] == '/' || r[na - 1] == '\\')) na--;
    r[na] = '/';
    memcpy(r + na + 1, b, nb + 1);
#ifdef _WIN32
    for (i = 0; r[i]; i++) if (r[i] == '/') r[i] = '\\';
#else
    (void)i;
#endif
    return r;
}

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int h2_name_ok(const char *name, size_t len) {
    size_t i;
    if (!name || len == 0) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':') return 0;
        if (c == '.' && i + 1 < len && name[i + 1] == '.') return 0;
    }
    if (len == 1 && name[0] == '.') return 0;
    return 1;
}

int h2_out_path_ok(const char *p) {
    size_t n, i, comps;
    if (!p) return 0;
    n = strlen(p);
    while (n > 0 && (p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    if (n == 0) return 0;                                     /* "", "/", "\\" */
    if (n == 2 && isalpha((unsigned char)p[0]) && p[1] == ':') return 0; /* "C:", "C:\" */
    if (n == 1 && p[0] == '.') return 0;
    if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
        /* \\?\ and \\.\ device paths: refuse outright. UNC: need > \\server\share */
        if (p[2] == '?' || p[2] == '.') return 0;
        comps = 0;
        for (i = 2; i < n; i++) {
            if (p[i] != '\\' && p[i] != '/' && (i == 2 || p[i - 1] == '\\' || p[i - 1] == '/')) comps++;
        }
        if (comps <= 2) return 0;
    }
    /* "/." style roots after trimming, e.g. "/./" */
    if (n == 2 && (p[0] == '/' || p[0] == '\\') && p[1] == '.') return 0;
    return 1;
}

/* --------------------------------------------------------- the item list */

typedef struct {
    char    *rel;    /* output-relative path, '/' separated */
    char    *src;    /* folder mode: source path            */
    uint64_t off;    /* image mode: byte offset in image    */
    uint64_t size;
    int      is_dir;
} item;

typedef struct {
    item    *v;
    size_t   n, cap;
    /* source */
    FILE    *iso;
    uint64_t iso_size;
    uint64_t part;
    const char *src_root;
    h2_import_result *res;
    int      err;
} ctx_t;

static int add_item(ctx_t *c, const char *prefix, const char *name, size_t nlen,
                    const char *src, uint64_t off, uint64_t size, int is_dir) {
    item *it;
    size_t np = prefix ? strlen(prefix) : 0;
    char *rel;
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        item *nv = (item *)realloc(c->v, nc * sizeof(item));
        if (!nv) return H2_ERR_NOMEM;
        c->v = nv; c->cap = nc;
    }
    rel = (char *)malloc(np + nlen + 2);
    if (!rel) return H2_ERR_NOMEM;
    if (np) { memcpy(rel, prefix, np); rel[np] = '/'; memcpy(rel + np + 1, name, nlen); rel[np + 1 + nlen] = 0; }
    else { memcpy(rel, name, nlen); rel[nlen] = 0; }
    it = &c->v[c->n++];
    it->rel = rel;
    it->src = NULL;
    if (src) {
        it->src = (char *)malloc(strlen(src) + 1);
        if (!it->src) return H2_ERR_NOMEM;
        strcpy(it->src, src);
    }
    it->off = off;
    it->size = size;
    it->is_dir = is_dir;
    return H2_OK;
}

static void free_items(ctx_t *c) {
    size_t i;
    for (i = 0; i < c->n; i++) { free(c->v[i].rel); free(c->v[i].src); }
    free(c->v);
    c->v = NULL; c->n = c->cap = 0;
}

/* Which top-level entries are kept. */
static int keep_top(const char *name, int is_dir) {
    static const char *dirs[] = {"maps", "media", "bink", "TDATA", "UDATA"};
    size_t i;
    if (!is_dir) return ieq(name, "default.xbe");
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
        if (ieq(name, dirs[i])) return 1;
    return 0;
}

/* ------------------------------------------------------- XDVDFS reader */

static const char XMAGIC[20] = {'M','I','C','R','O','S','O','F','T','*','X','B','O','X','*','M','E','D','I','A'};

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at(ctx_t *c, uint64_t off, void *buf, size_t n) {
    if (off > c->iso_size || n > c->iso_size - off) return -1;
    if (x_seek(c->iso, off) != 0) return -1;
    return fread(buf, 1, n, c->iso) == n ? 0 : -1;
}

static int iso_walk(ctx_t *c, uint32_t sector, uint32_t size, const char *prefix, int depth);

static int iso_entry(ctx_t *c, const unsigned char *e, const char *prefix, int depth) {
    uint32_t sector = rd32(e + 4), size = rd32(e + 8);
    unsigned attr = e[12], nlen = e[13];
    const char *name = (const char *)(e + 14);
    int is_dir = (attr & 0x10) != 0;
    char nm[256];
    uint64_t off;
    int r;

    if (!h2_name_ok(name, nlen)) {
        char shown[256];
        unsigned i;
        for (i = 0; i < nlen; i++) shown[i] = (name[i] >= 0x20 && name[i] < 0x7f) ? name[i] : '?';
        shown[nlen] = 0;
        seterr(c->res, "unsafe name in image: \"%s%s%s\" (contains \"..\", \"/\", \"\\\", \":\" or control "
               "characters) - refusing the whole image", prefix ? prefix : "", prefix ? "/" : "", shown);
        return H2_ERR_BADNAME;
    }
    memcpy(nm, name, nlen);
    nm[nlen] = 0;
    if (!prefix && !keep_top(nm, is_dir)) { if (c->res) c->res->skipped++; return H2_OK; }

    off = c->part + (uint64_t)sector * H2_SECTOR;
    if (!is_dir) {
        if (size && (off > c->iso_size || size > c->iso_size - off)) {
            seterr(c->res, "image is truncated: %s%s%s lies past the end of the file",
                   prefix ? prefix : "", prefix ? "/" : "", nm);
            return H2_ERR_FORMAT;
        }
        return add_item(c, prefix, nm, nlen, NULL, off, size, 0);
    }
    if ((r = add_item(c, prefix, nm, nlen, NULL, 0, 0, 1)) != H2_OK) return r;
    {
        char *sub = (char *)malloc((prefix ? strlen(prefix) + 1 : 0) + nlen + 1);
        if (!sub) return H2_ERR_NOMEM;
        if (prefix) sprintf(sub, "%s/%s", prefix, nm); else strcpy(sub, nm);
        r = iso_walk(c, sector, size, sub, depth + 1);
        free(sub);
    }
    return r;
}

static int iso_walk(ctx_t *c, uint32_t sector, uint32_t size, const char *prefix, int depth) {
    unsigned char *d;
    unsigned char *seen;
    uint32_t *stack;
    size_t sp = 0, ndw;
    uint32_t cur;
    int r = H2_OK;

    if (depth > H2_MAX_DEPTH) { seterr(c->res, "directory tree too deep (damaged image?)"); return H2_ERR_FORMAT; }
    if (size == 0) return H2_OK;
    if (size > H2_MAX_DIR_BYTES) { seterr(c->res, "directory table too large (damaged image?)"); return H2_ERR_FORMAT; }
    d = (unsigned char *)malloc(size);
    ndw = size / 4 + 1;
    seen = (unsigned char *)calloc(ndw, 1);
    stack = (uint32_t *)malloc(ndw * sizeof(uint32_t));
    if (!d || !seen || !stack) { free(d); free(seen); free(stack); return H2_ERR_NOMEM; }
    if (read_at(c, c->part + (uint64_t)sector * H2_SECTOR, d, size) != 0) {
        seterr(c->res, "cannot read directory table at sector %u (truncated image?)", (unsigned)sector);
        r = H2_ERR_FORMAT;
        goto done;
    }
    /* empty directory written as a padding entry */
    if (size >= 4 && rd32(d) == 0xFFFFFFFFu) goto done;

    /* iterative in-order traversal; `seen` guards against cycles */
    cur = 0;
    for (;;) {
        while (cur != 0xFFFFFFFFu) {
            uint32_t off = cur * 4, left;
            if ((size_t)off + 14 > size || seen[cur]) {
                seterr(c->res, "damaged directory tree in %s", prefix ? prefix : "(root)");
                r = H2_ERR_FORMAT;
                goto done;
            }
            seen[cur] = 1;
            if ((size_t)off + 14 + d[off + 13] > size) {
                seterr(c->res, "damaged directory entry in %s", prefix ? prefix : "(root)");
                r = H2_ERR_FORMAT;
                goto done;
            }
            stack[sp++] = cur;
            left = rd16(d + off);
            cur = (left == 0 || left == 0xFFFF) ? 0xFFFFFFFFu : left;
        }
        if (sp == 0) break;
        cur = stack[--sp];
        {
            uint32_t off = cur * 4, right = rd16(d + off + 2);
            if ((r = iso_entry(c, d + off, prefix, depth)) != H2_OK) goto done;
            cur = (right == 0 || right == 0xFFFF) ? 0xFFFFFFFFu : right;
        }
    }
done:
    free(d); free(seen); free(stack);
    return r;
}

static int iso_list(ctx_t *c, const char *path) {
    static const uint64_t probes[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
    unsigned char vd[H2_SECTOR];
    size_t i;
    uint64_t sz = 0;
    if (x_stat(path, &sz) != 1) { seterr(c->res, "cannot open %s", path); return H2_ERR_OPEN; }
    c->iso = x_fopen(path, "rb");
    if (!c->iso) { seterr(c->res, "cannot open %s", path); return H2_ERR_OPEN; }
    c->iso_size = sz;
    for (i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        uint64_t off = probes[i] + 32ull * H2_SECTOR;
        if (read_at(c, off, vd, sizeof vd) != 0) continue;
        if (memcmp(vd, XMAGIC, 20) != 0 || memcmp(vd + 0x7EC, XMAGIC, 20) != 0) continue;
        c->part = probes[i];
        if (c->res) { c->res->source_kind = 1; c->res->partition_offset = c->part; }
        return iso_walk(c, rd32(vd + 20), rd32(vd + 24), NULL, 0);
    }
    seterr(c->res, "%s is not an Xbox disc image (no XDVDFS volume at offsets 0, 0x18300000, "
           "0xFD90000 or 0x2080000)", path);
    return H2_ERR_FORMAT;
}

/* ------------------------------------------------------- folder reader */

typedef struct {
    ctx_t *c;
    const char *dir;     /* source directory being listed */
    const char *prefix;  /* output-relative prefix or NULL at top */
    int depth;
} folder_ctx;

static int folder_walk(ctx_t *c, const char *dir, const char *prefix, int depth);

static int folder_cb(void *vp, const char *name, int is_dir) {
    folder_ctx *f = (folder_ctx *)vp;
    ctx_t *c = f->c;
    size_t nlen = strlen(name);
    char *src;
    uint64_t size = 0;
    int r;
    if (!h2_name_ok(name, nlen)) {
        seterr(c->res, "unsafe name in source folder: \"%s\"", name);
        return H2_ERR_BADNAME;
    }
    if (!f->prefix && !keep_top(name, is_dir)) { if (c->res) c->res->skipped++; return H2_OK; }
    src = join(f->dir, name);
    if (!src) return H2_ERR_NOMEM;
    if (!is_dir) {
        if (x_stat(src, &size) != 1) { free(src); return H2_OK; }
        r = add_item(c, f->prefix, name, nlen, src, 0, size, 0);
    } else {
        r = add_item(c, f->prefix, name, nlen, NULL, 0, 0, 1);
        if (r == H2_OK) {
            char *sub = (char *)malloc((f->prefix ? strlen(f->prefix) + 1 : 0) + nlen + 1);
            if (!sub) { free(src); return H2_ERR_NOMEM; }
            if (f->prefix) sprintf(sub, "%s/%s", f->prefix, name); else strcpy(sub, name);
            r = folder_walk(c, src, sub, f->depth + 1);
            free(sub);
        }
    }
    free(src);
    return r;
}

static int folder_walk(ctx_t *c, const char *dir, const char *prefix, int depth) {
    folder_ctx f;
    int opened, r;
    if (depth > H2_MAX_DEPTH) { seterr(c->res, "folder tree too deep"); return H2_ERR_FORMAT; }
    f.c = c; f.dir = dir; f.prefix = prefix; f.depth = depth;
    r = x_listdir(dir, folder_cb, &f, &opened);
    if (!opened) { seterr(c->res, "cannot list folder %s", dir); return H2_ERR_OPEN; }
    return r;
}

/* ------------------------------------------------------------- copying */

typedef struct {
    h2_progress_fn cb;
    void *user;
    uint64_t done, total;
    unsigned char *buf;
} prog_t;

/* Copy one item to dst (via dst.part). If `sha` is set, the stream is hashed
 * too. Returns H2_OK or an error; the .part file is removed on failure. */
static int copy_item(ctx_t *c, const item *it, const char *dst, prog_t *p, h2_sha256_ctx *sha,
                     int keep_part, char **part_out) {
    FILE *in = NULL, *out = NULL;
    uint64_t left = it->size;
    size_t n = strlen(dst);
    char *part = (char *)malloc(n + 6);
    int r = H2_OK;
    if (!part) return H2_ERR_NOMEM;
    memcpy(part, dst, n);
    memcpy(part + n, ".part", 6);

    if (it->src) {
        in = x_fopen(it->src, "rb");
        if (!in) { seterr(c->res, "cannot open %s", it->src); free(part); return H2_ERR_OPEN; }
    } else {
        in = c->iso;
        if (x_seek(in, it->off) != 0) { seterr(c->res, "seek failed in image"); free(part); return H2_ERR_OPEN; }
    }
    out = x_fopen(part, "wb");
    if (!out) {
        seterr(c->res, "cannot create %s", part);
        if (it->src) fclose(in);
        free(part);
        return H2_ERR_WRITE;
    }
    if (p->cb) p->cb(p->done, p->total, it->rel, p->user);
    while (left) {
        size_t chunk = left > H2_COPY_BUF ? H2_COPY_BUF : (size_t)left;
        if (fread(p->buf, 1, chunk, in) != chunk) {
            seterr(c->res, "read error in %s (truncated source?)", it->rel);
            r = H2_ERR_OPEN;
            break;
        }
        if (sha) h2_sha256_update(sha, p->buf, chunk);
        if (fwrite(p->buf, 1, chunk, out) != chunk) {
            seterr(c->res, "write error on %s (disk full?)", part);
            r = H2_ERR_WRITE;
            break;
        }
        left -= chunk;
        p->done += chunk;
        if (p->cb) p->cb(p->done, p->total, it->rel, p->user);
    }
    if (it->src) fclose(in);
    if (fclose(out) != 0 && r == H2_OK) { seterr(c->res, "write error on %s", part); r = H2_ERR_WRITE; }
    if (r != H2_OK) { x_remove(part); free(part); return r; }
    if (keep_part) { *part_out = part; return H2_OK; }
    if (x_rename_replace(part, dst) != 0) {
        seterr(c->res, "cannot rename %s to %s", part, dst);
        x_remove(part);
        free(part);
        return H2_ERR_WRITE;
    }
    free(part);
    return H2_OK;
}

static int mkdirs(const char *path) {
    /* create every missing component of `path` */
    size_t n = strlen(path), i, start = 0;
    char *tmp = (char *)malloc(n + 1);
    int r = 0;
    if (!tmp) return -1;
    memcpy(tmp, path, n + 1);
    if (n >= 2 && isalpha((unsigned char)tmp[0]) && tmp[1] == ':') start = 2;
    if (n >= 2 && (tmp[0] == '/' || tmp[0] == '\\') && (tmp[1] == '/' || tmp[1] == '\\')) {
        /* UNC \\server\share\...: start creating after the share component */
        int seps = 0;
        for (start = 2; start < n; start++)
            if ((tmp[start] == '/' || tmp[start] == '\\') && ++seps == 2) break;
    }
    while (start < n && (tmp[start] == '/' || tmp[start] == '\\')) start++;
    for (i = start; i <= n && r == 0; i++) {
        if (i == n || tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            if (i > start) {
                tmp[i] = 0;
                if (x_stat(tmp, NULL) != 2) r = x_mkdir1(tmp);
                tmp[i] = save;
            }
        }
    }
    free(tmp);
    return r;
}

int h2_sha256_file(const char *path, unsigned char out[32]) {
    FILE *f = x_fopen(path, "rb");
    h2_sha256_ctx c;
    unsigned char *buf;
    size_t n;
    int err;
    if (!f) return -1;
    buf = (unsigned char *)malloc(1u << 20);
    if (!buf) { fclose(f); return -1; }
    h2_sha256_init(&c);
    while ((n = fread(buf, 1, 1u << 20, f)) > 0) h2_sha256_update(&c, buf, n);
    err = ferror(f);
    fclose(f);
    free(buf);
    if (err) return -1;
    h2_sha256_final(&c, out);
    return 0;
}

int h2_xbe_sha_ok(const char *path) {
    unsigned char d[32];
    char hex[65];
    if (!path || h2_sha256_file(path, d) != 0) return 0;
    h2_hex32(d, hex);
    return strcmp(hex, H2_XBE_SHA256_HEX) == 0;
}

/* ------------------------------------------------------------- import */

int h2_import(const char *src, const char *out_dir, h2_progress_fn progress, void *user,
              h2_import_result *res) {
    ctx_t c;
    prog_t p;
    h2_import_result local;
    size_t i, xbe = (size_t)-1;
    int kind, r = H2_OK;
    char *fa, *fb;

    if (!res) res = &local;
    memset(res, 0, sizeof *res);
    memset(&c, 0, sizeof c);
    memset(&p, 0, sizeof p);
    c.res = res;

    if (!src || !*src) { seterr(res, "no source given"); return H2_ERR_ARGS; }
    if (!h2_out_path_ok(out_dir)) {
        seterr(res, "refusing output path \"%s\": it is empty, a drive/filesystem root or \".\"/\"..\"",
               out_dir ? out_dir : "");
        return H2_ERR_OUTPATH;
    }
    kind = x_stat(src, NULL);
    if (kind == 0) { seterr(res, "source not found: %s", src); return H2_ERR_OPEN; }

    fa = x_fullpath(src);
    fb = x_fullpath(out_dir);
    if (fa && fb) {
        int same =
#ifdef _WIN32
            ieq(fa, fb);
#else
            strcmp(fa, fb) == 0;
#endif
        if (same) { free(fa); free(fb); seterr(res, "output folder is the source folder"); return H2_ERR_OUTPATH; }
    }
    free(fa); free(fb);

    /* 1. list what will be copied (nothing is written yet) */
    if (kind == 2) {
        res->source_kind = 0;
        c.src_root = src;
        r = folder_walk(&c, src, NULL, 0);
    } else {
        r = iso_list(&c, src);
    }
    if (r != H2_OK) goto out;

    for (i = 0; i < c.n; i++) {
        if (!c.v[i].is_dir && !strchr(c.v[i].rel, '/') && ieq(c.v[i].rel, "default.xbe")) xbe = i;
        if (!c.v[i].is_dir) p.total += c.v[i].size;
    }
    if (xbe == (size_t)-1) {
        seterr(res, "no default.xbe found in %s - is this a Halo 2 disc or game folder?", src);
        r = H2_ERR_NOXBE;
        goto out;
    }

    /* 2. output folder */
    if (mkdirs(out_dir) != 0) { seterr(res, "cannot create output folder %s", out_dir); r = H2_ERR_WRITE; goto out; }
    p.cb = progress;
    p.user = user;
    p.buf = (unsigned char *)malloc(H2_COPY_BUF);
    if (!p.buf) { r = H2_ERR_NOMEM; goto out; }

    /* 3. default.xbe first, hashed in flight; refuse the rest on mismatch */
    {
        h2_sha256_ctx sc;
        unsigned char d[32];
        char *dst = join(out_dir, "default.xbe"), *part = NULL;
        if (!dst) { r = H2_ERR_NOMEM; goto out; }
        h2_sha256_init(&sc);
        r = copy_item(&c, &c.v[xbe], dst, &p, &sc, 1, &part);
        if (r != H2_OK) { free(dst); goto out; }
        h2_sha256_final(&sc, d);
        h2_hex32(d, res->sha_hex);
        if (strcmp(res->sha_hex, H2_XBE_SHA256_HEX) != 0) {
            x_remove(part); /* our own temporary file only */
            free(part);
            free(dst);
            seterr(res,
                   "default.xbe is not the supported Halo 2 build.\n"
                   "  got      %s\n  expected %s\n"
                   "No game files were written and nothing in the output folder was removed.\n"
                   "Use an unmodified dump of the retail Halo 2 disc.",
                   res->sha_hex, H2_XBE_SHA256_HEX);
            r = H2_ERR_SHA;
            goto out;
        }
        if (x_rename_replace(part, dst) != 0) {
            seterr(res, "cannot rename %s to %s", part, dst);
            x_remove(part);
            free(part); free(dst);
            r = H2_ERR_WRITE;
            goto out;
        }
        free(part); free(dst);
        res->files++;
        res->bytes += c.v[xbe].size;
    }

    /* 4. everything else, in listing order (directories precede contents) */
    for (i = 0; i < c.n; i++) {
        char *dst;
        if (i == xbe) continue;
        dst = join(out_dir, c.v[i].rel);
        if (!dst) { r = H2_ERR_NOMEM; goto out; }
        if (c.v[i].is_dir) {
            if (x_mkdir1(dst) != 0) { seterr(res, "cannot create folder %s", dst); free(dst); r = H2_ERR_WRITE; goto out; }
            res->dirs++;
        } else {
            r = copy_item(&c, &c.v[i], dst, &p, NULL, 0, NULL);
            if (r != H2_OK) { free(dst); goto out; }
            res->files++;
            res->bytes += c.v[i].size;
        }
        free(dst);
    }

    /* 5. re-verify what landed on disk with the same check the game uses */
    {
        char *dst = join(out_dir, "default.xbe");
        res->sha_ok = dst ? h2_xbe_sha_ok(dst) : 0;
        free(dst);
        if (!res->sha_ok) { seterr(res, "default.xbe on disk does not verify after copying (disk error?)"); r = H2_ERR_SHA; }
    }

out:
    free(p.buf);
    if (c.iso) fclose(c.iso);
    free_items(&c);
    return r;
}
