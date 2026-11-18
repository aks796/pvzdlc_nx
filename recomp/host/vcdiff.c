/* vcdiff.c: a VCDIFF decoder (RFC 3284), for the first launch: the DLC Edition is shipped as xdelta3 patches for the
 * player's own PlantsVsZombies.exe (Steam / EA / OG Patch.xdelta), which the DLC team applies with DeltaPatcher (a
 * Windows program). Here the port applies them itself, on every platform. Those patches are plain VCDIFF: the default
 * code table, no secondary compression, xdelta3's application header (the file names, skipped) and its extension of a
 * window: an Adler-32 of the window's target after the three section lengths (Win_Indicator bit 0x04), checked. The
 * whole source is read in (PlantsVsZombies.exe is a few MB); each target window is made in memory and written out.
 * A window whose source segment comes from the target (VCD_TARGET) reads it back from the file being written. */
#include "vcdiff.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VCD_SOURCE = 1, VCD_TARGET = 2, VCD_ADLER32 = 4 };
enum { NOOP, ADD, RUN, COPY };
#define S_NEAR 4
#define S_SAME 3

typedef struct {
    uint8_t type[2], size[2], mode[2];
} Code;
static Code table[256];

/* the default code table (RFC 3284 5.6) */
static void build_table(void)
{
    if (table[1].type[0]) return;
    int i = 0;
    table[i++] = (Code){{RUN, NOOP}, {0, 0}, {0, 0}};
    for (int s = 0; s <= 17; s++) table[i++] = (Code){{ADD, NOOP}, {(uint8_t)s, 0}, {0, 0}};
    for (int m = 0; m <= 8; m++) {
        table[i++] = (Code){{COPY, NOOP}, {0, 0}, {(uint8_t)m, 0}};
        for (int s = 4; s <= 18; s++) table[i++] = (Code){{COPY, NOOP}, {(uint8_t)s, 0}, {(uint8_t)m, 0}};
    }
    for (int m = 0; m <= 5; m++)
        for (int a = 1; a <= 4; a++)
            for (int s = 4; s <= 6; s++) table[i++] = (Code){{ADD, COPY}, {(uint8_t)a, (uint8_t)s}, {0, (uint8_t)m}};
    for (int m = 6; m <= 8; m++)
        for (int a = 1; a <= 4; a++) table[i++] = (Code){{ADD, COPY}, {(uint8_t)a, 4}, {0, (uint8_t)m}};
    for (int m = 0; m <= 8; m++) table[i++] = (Code){{COPY, ADD}, {4, 1}, {(uint8_t)m, 0}};
}

typedef struct {
    const uint8_t *p, *end;
} Rd;
static int rd_byte(Rd *r, uint32_t *v)
{
    if (r->p >= r->end) return -1;
    *v = *r->p++;
    return 0;
}
/* a VCDIFF integer: base 128, most significant first, the top bit set on all but the last byte */
static int rd_int(Rd *r, uint64_t *v)
{
    uint64_t x = 0;
    for (int i = 0; i < 10 && r->p < r->end; i++) {
        uint8_t b = *r->p++;
        x = x << 7 | (b & 0x7f);
        if (!(b & 0x80)) {
            *v = x;
            return 0;
        }
    }
    return -1;
}
static int f_byte(FILE *f, uint32_t *v)
{
    int c = fgetc(f);
    if (c == EOF) return -1;
    *v = (uint32_t)c;
    return 0;
}
static int f_int(FILE *f, uint64_t *v)
{
    uint64_t x = 0;
    for (int i = 0; i < 10; i++) {
        int c = fgetc(f);
        if (c == EOF) return -1;
        x = x << 7 | ((uint32_t)c & 0x7f);
        if (!(c & 0x80)) {
            *v = x;
            return 0;
        }
    }
    return -1;
}

typedef struct {
    uint64_t near[S_NEAR], same[S_SAME * 256];
    int next;
} Cache;
static void cache_put(Cache *c, uint64_t a)
{
    c->near[c->next] = a;
    c->next = (c->next + 1) % S_NEAR;
    c->same[a % (S_SAME * 256)] = a;
}
static int cache_addr(Cache *c, Rd *ad, uint64_t here, int mode, uint64_t *out)
{
    uint64_t a;
    uint32_t b;
    if (mode == 0) { /* VCD_SELF */
        if (rd_int(ad, &a)) return -1;
    } else if (mode == 1) { /* VCD_HERE */
        if (rd_int(ad, &a) || a > here) return -1;
        a = here - a;
    } else if (mode - 2 < S_NEAR) {
        if (rd_int(ad, &a)) return -1;
        a += c->near[mode - 2];
    } else {
        int m = mode - 2 - S_NEAR;
        if (m >= S_SAME || rd_byte(ad, &b)) return -1;
        a = c->same[m * 256 + (int)b];
    }
    cache_put(c, a);
    *out = a;
    return 0;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) a += *p++, b += a;
        a %= 65521, b %= 65521;
    }
    return b << 16 | a;
}

int vcdiff_is_patch(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t m[4] = {0};
    int ok = fread(m, 1, 4, f) == 4 && m[0] == 0xd6 && m[1] == 0xc3 && m[2] == 0xc4 && m[3] == 0;
    fclose(f);
    return ok;
}

static uint8_t *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) free(p), p = NULL;
    fclose(f);
    if (p) *n = (size_t)len;
    return p;
}

int vcdiff_apply(const char *patch, const char *source, const char *target, int windows, VcdiffProgress progress,
                 char *err, size_t errlen)
{
    build_table();
    int ret = -1;
    size_t src_n = 0;
    uint8_t *src = read_all(source, &src_n), *enc = NULL, *tgt = NULL, *seg_buf = NULL;
    FILE *in = fopen(patch, "rb"), *out = NULL;
    char part[1100] = "";
    if (!src || !in) {
        snprintf(err, errlen, "%s couldn't be read.", !src ? source : patch);
        goto done;
    }
    fseek(in, 0, SEEK_END);
    long patch_n = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint32_t hdr;
    uint8_t magic[4];
    if (fread(magic, 1, 4, in) != 4 || magic[0] != 0xd6 || magic[1] != 0xc3 || magic[2] != 0xc4 || magic[3] != 0 ||
        f_byte(in, &hdr)) {
        snprintf(err, errlen, "%s isn't a VCDIFF (xdelta) patch.", patch);
        goto done;
    }
    if (hdr & 1) { /* a secondary compressor: not in the DLC's patches */
        snprintf(err, errlen, "%s uses secondary compression, which this port can't read: patch it with xdelta3.", patch);
        goto done;
    }
    if (hdr & 2) {
        snprintf(err, errlen, "%s has its own code table, which this port can't read: patch it with xdelta3.", patch);
        goto done;
    }
    if (hdr & 4) { /* xdelta3's application header */
        uint64_t n;
        if (f_int(in, &n) || fseek(in, (long)n, SEEK_CUR)) goto bad;
    }
    if (windows == 0) {
        snprintf(part, sizeof part, "%s.part", target);
        out = fopen(part, "w+b");
        if (!out) {
            snprintf(err, errlen, "%s couldn't be written.", part);
            goto done;
        }
    }
    uint64_t out_pos = 0;
    size_t enc_cap = 0, tgt_cap = 0, seg_cap = 0;
    int last_pm = -1;
    for (int win = 0; windows == 0 || win < windows; win++) {
        uint32_t ind;
        if (f_byte(in, &ind)) break; /* (the end) */
        uint64_t seg_len = 0, seg_pos = 0, enc_len;
        if (ind & (VCD_SOURCE | VCD_TARGET)) {
            if (f_int(in, &seg_len) || f_int(in, &seg_pos)) goto bad;
        }
        if (f_int(in, &enc_len) || enc_len > ((uint64_t)1 << 31)) goto bad;
        if (enc_len > enc_cap) {
            free(enc);
            enc = malloc((size_t)enc_len), enc_cap = (size_t)enc_len;
            if (!enc) goto nomem;
        }
        if (fread(enc, 1, (size_t)enc_len, in) != (size_t)enc_len) goto bad;
        Rd r = {enc, enc + enc_len};
        uint64_t tgt_len, data_len, inst_len, addr_len;
        uint32_t delta_ind;
        if (rd_int(&r, &tgt_len) || rd_byte(&r, &delta_ind) || rd_int(&r, &data_len) || rd_int(&r, &inst_len) ||
            rd_int(&r, &addr_len) || tgt_len > ((uint64_t)1 << 30))
            goto bad;
        if (delta_ind) {
            snprintf(err, errlen, "%s has compressed sections, which this port can't read: patch it with xdelta3.", patch);
            goto done;
        }
        uint32_t want = 0;
        if (ind & VCD_ADLER32) {
            if (r.end - r.p < 4) goto bad;
            want = (uint32_t)r.p[0] << 24 | (uint32_t)r.p[1] << 16 | (uint32_t)r.p[2] << 8 | r.p[3];
            r.p += 4;
        }
        if ((uint64_t)(r.end - r.p) < data_len + inst_len + addr_len) goto bad;
        Rd data = {r.p, r.p + data_len}, inst = {data.end, data.end + inst_len}, addr = {inst.end, inst.end + addr_len};
        /* the source segment: from the source, or from the target written so far */
        const uint8_t *seg = NULL;
        if (ind & VCD_SOURCE) {
            if (seg_pos > src_n || seg_len > src_n - seg_pos) {
                ret = -2;
                snprintf(err, errlen, "%s isn't the Plants vs. Zombies exe this patch is for.", source);
                goto done;
            }
            seg = src + seg_pos;
        } else if (ind & VCD_TARGET) {
            if (!out || seg_pos > out_pos || seg_len > out_pos - seg_pos) goto bad;
            if (seg_len > seg_cap) {
                free(seg_buf);
                seg_buf = malloc((size_t)seg_len), seg_cap = (size_t)seg_len;
                if (!seg_buf) goto nomem;
            }
            fflush(out);
            if (fseek(out, (long)seg_pos, SEEK_SET) || fread(seg_buf, 1, (size_t)seg_len, out) != (size_t)seg_len) goto bad;
            fseek(out, 0, SEEK_END);
            seg = seg_buf;
        }
        if (tgt_len > tgt_cap) {
            free(tgt);
            tgt = malloc((size_t)tgt_len + 1), tgt_cap = (size_t)tgt_len;
            if (!tgt) goto nomem;
        }
        Cache cache;
        memset(&cache, 0, sizeof cache);
        uint64_t pos = 0;
        while (inst.p < inst.end) {
            uint32_t ci;
            if (rd_byte(&inst, &ci)) goto bad;
            const Code *k = &table[ci];
            for (int h = 0; h < 2; h++) {
                if (k->type[h] == NOOP) continue;
                uint64_t size = k->size[h];
                if (!size && rd_int(&inst, &size)) goto bad;
                if (size > tgt_len - pos) goto bad;
                if (k->type[h] == ADD) {
                    if ((uint64_t)(data.end - data.p) < size) goto bad;
                    memcpy(tgt + pos, data.p, (size_t)size);
                    data.p += size;
                } else if (k->type[h] == RUN) {
                    uint32_t v;
                    if (rd_byte(&data, &v)) goto bad;
                    memset(tgt + pos, (int)v, (size_t)size);
                } else {
                    uint64_t a;
                    if (cache_addr(&cache, &addr, seg_len + pos, k->mode[h], &a) || a >= seg_len + pos) goto bad;
                    if (a + size <= seg_len) {
                        if (!seg) goto bad;
                        memcpy(tgt + pos, seg + a, (size_t)size);
                    } else {
                        /* (from the target window itself, perhaps overlapping what it makes: byte by byte) */
                        for (uint64_t j = 0; j < size; j++) {
                            uint64_t s = a + j;
                            if (s < seg_len) {
                                if (!seg) goto bad;
                                tgt[pos + j] = seg[s];
                            } else
                                tgt[pos + j] = tgt[s - seg_len];
                        }
                    }
                }
                pos += size;
            }
        }
        if (pos != tgt_len) goto bad;
        if ((ind & VCD_ADLER32) && adler32(tgt, (size_t)tgt_len) != want) {
            ret = -2;
            snprintf(err, errlen, "%s isn't the Plants vs. Zombies exe this patch is for.", source);
            goto done;
        }
        if (out && fwrite(tgt, 1, (size_t)tgt_len, out) != (size_t)tgt_len) {
            snprintf(err, errlen, "%s couldn't be written (is the storage full?).", part);
            goto done;
        }
        out_pos += tgt_len;
        if (progress && patch_n > 0) {
            int pm = (int)(ftell(in) * 1000 / patch_n);
            if (pm != last_pm) progress("Patching the DLC Edition exe (first launch only)", pm), last_pm = pm;
        }
    }
    if (out) {
        if (fclose(out)) {
            out = NULL;
            snprintf(err, errlen, "%s couldn't be written (is the storage full?).", part);
            goto done;
        }
        out = NULL;
        remove(target);
        if (rename(part, target)) {
            snprintf(err, errlen, "%s couldn't be renamed.", part);
            goto done;
        }
        part[0] = 0;
    }
    ret = 0;
    goto done;
nomem:
    snprintf(err, errlen, "Not enough memory to apply the patch.");
    goto done;
bad:
    snprintf(err, errlen, "%s is damaged (or isn't a VCDIFF patch).", patch);
done:
    if (out) fclose(out);
    if (part[0]) remove(part);
    if (in) fclose(in);
    free(src), free(enc), free(tgt), free(seg_buf);
    return ret;
}
