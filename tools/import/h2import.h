/*
 * h2import - bring-your-own-game importer for Open Halo 2.
 *
 * Reads the user's own Halo 2 disc image (Redump full-disc ISO or XISO) or an
 * already-extracted folder and copies only the files the game needs into a
 * `game` directory: default.xbe, maps/, media/, bink/, TDATA/, UDATA/.
 * Then it checks default.xbe against the supported retail SHA-256.
 *
 * Portable C99 (MSVC, clang/gcc, emscripten). No third-party code: the XDVDFS
 * reader and SHA-256 are implemented in h2import.c.
 *
 * Paths are UTF-8 on every platform (converted to UTF-16 on Windows).
 */
#ifndef H2IMPORT_H
#define H2IMPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-256 of the supported retail default.xbe (lowercase hex). */
#define H2_XBE_SHA256_HEX "03215919bb7163259257d361f4c7bf802a7ab12aa85e2689436369b5c427935d"

enum {
    H2_OK = 0,
    H2_ERR_ARGS = -1,      /* bad arguments                                 */
    H2_ERR_OUTPATH = -2,   /* output path refused (empty, root, == source)  */
    H2_ERR_OPEN = -3,      /* cannot open/read the source                   */
    H2_ERR_FORMAT = -4,    /* not an Xbox image / damaged filesystem        */
    H2_ERR_BADNAME = -5,   /* image holds a name with "..", "/", "\\", ":"  */
    H2_ERR_NOXBE = -6,     /* no default.xbe in the source                  */
    H2_ERR_SHA = -7,       /* default.xbe is not the supported retail build */
    H2_ERR_WRITE = -8,     /* cannot create/write the output                */
    H2_ERR_NOMEM = -9
};

/* Called at the start of every file and after every copied chunk.
 * `name` is the output-relative path of the file being copied. */
typedef void (*h2_progress_fn)(uint64_t done, uint64_t total, const char *name, void *user);

typedef struct {
    int      source_kind;       /* 0 = folder, 1 = disc image             */
    uint64_t partition_offset;  /* image only: game partition byte offset */
    unsigned files;             /* files copied                           */
    unsigned dirs;              /* directories created                    */
    unsigned skipped;           /* top-level entries not needed (skipped) */
    uint64_t bytes;             /* bytes copied                           */
    int      sha_ok;            /* 1 if output default.xbe matches        */
    char     sha_hex[65];       /* SHA-256 of the source default.xbe      */
    char     err[768];          /* human readable error, "" on success    */
} h2_import_result;

/* Import `src` (.iso file or extracted folder) into `out_dir` (created if
 * missing). Returns H2_OK or an H2_ERR_* code; `res` may be NULL.
 * Files are written as <name>.part and renamed when complete; on failure only
 * the importer's own .part file is removed - nothing else is ever deleted. */
int h2_import(const char *src, const char *out_dir,
              h2_progress_fn progress, void *user, h2_import_result *res);

/* 1 if the file at `path` hashes to H2_XBE_SHA256_HEX, else 0. */
int h2_xbe_sha_ok(const char *path);

/* SHA-256 of a whole file; returns 0 on success. */
int h2_sha256_file(const char *path, unsigned char out[32]);

/* Streaming SHA-256. */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t   fill;
} h2_sha256_ctx;
void h2_sha256_init(h2_sha256_ctx *c);
void h2_sha256_update(h2_sha256_ctx *c, const void *data, size_t n);
void h2_sha256_final(h2_sha256_ctx *c, unsigned char out[32]);
void h2_hex32(const unsigned char d[32], char hex[65]);

/* 1 if `out_dir` is acceptable as an output (not empty, "/", a drive root,
 * a UNC/device root, "." or ".."), else 0. */
int h2_out_path_ok(const char *out_dir);

/* 1 if `name` (a single path component taken from an image or folder) is safe:
 * non-empty, no "..", "/", "\\", ":" and no control characters. */
int h2_name_ok(const char *name, size_t len);

#ifdef __cplusplus
}
#endif
#endif
