/* evb_unpack.c: the game's files from the DLC Edition's own exe, on the first launch.
 *
 * The DLC Edition ships as one exe (the player's PlantsVsZombies.exe patched with the DLC's xdelta), packed with
 * Enigma Virtual Box: main.pak, bass.dll and properties/ sit in its .enigma1 section, stored as they are. Here they
 * are copied out next to it, and the game's exe is restored without the packer (dlc_game.exe: the file up to
 * .enigma1, its section count less the two Enigma sections and its import, relocation and TLS directories put
 * back), as evbunpack does it (github.com/mos9527/evbunpack, Apache 2.0: the virtual file table's layout and the
 * restore). Both are checked against the 2.7.2 build the port was made from (CRC-32): another version's code
 * would not match the recompiled one. Each file is written as NAME.part and renamed when whole, so a launch cut
 * short starts over. */
#include "../runtime/rt_compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "evb_unpack.h"

#define DLC_EXE_CRC 0x6b3d0401u   /* dlc_game.exe, 7,666,688 bytes */
#define DLC_EXE_SIZE 7666688u
#define MAIN_PAK_CRC 0x98e03c4bu  /* main.pak, 214,952,152 bytes */

static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

typedef struct {
    char path[256];
    uint64_t offset, size;
} EvbFile;

typedef struct {
    FILE *f;
    char *err;
    size_t errlen;
    EvbProgress progress;
    uint64_t done, total;
} Ctx;

static int fail(Ctx *c, const char *msg)
{
    snprintf(c->err, c->errlen, "%s", msg);
    return -1;
}

static int read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

/* copies [off, off + n) of the exe to path (as path.part, renamed when whole); its CRC-32 into *crc */
static int copy_out(Ctx *c, uint64_t off, uint64_t n, const char *path, uint32_t *crc)
{
    static uint8_t buf[1 << 20];
    char part[300];
    snprintf(part, sizeof part, "%s.part", path);
    FILE *o = fopen(part, "wb");
    if (!o) return fail(c, "Couldn't write to the SD card (is it full or read-only?).");
    uint32_t cr = 0;
    if (fseeko(c->f, (off_t)off, SEEK_SET)) { fclose(o); return fail(c, "The exe couldn't be read."); }
    while (n) {
        size_t k = n < sizeof buf ? (size_t)n : sizeof buf;
        if (fread(buf, 1, k, c->f) != k) { fclose(o); remove(part); return fail(c, "The exe is cut short (copied incompletely?)."); }
        if (fwrite(buf, 1, k, o) != k) { fclose(o); remove(part); return fail(c, "The SD card is full: the game's files need about 230 MB."); }
        cr = crc32_update(cr, buf, k);
        n -= k, c->done += k;
        if (c->progress) c->progress("Unpacking the game (first launch only)", (int)(c->done * 1000 / (c->total ? c->total : 1)));
    }
    if (fclose(o)) { remove(part); return fail(c, "The SD card is full: the game's files need about 230 MB."); }
    remove(path);
    if (rename(part, path)) return fail(c, "Couldn't write to the SD card.");
    if (crc) *crc = cr;
    return 0;
}

/* The virtual file table (evbunpack's pe_external_tree): a 64-byte pack header ("EVB\0"), the main node, then
 * per object a 16-byte header node (size, 8 bytes, objects count), a UTF-16 name ended by 00 00, a type byte
 * (2 file, 3 folder) and for a file 53 bytes (original size at +2, stored size at +49), for a folder 25; the
 * files' data follows the table, in order */
static int read_table(Ctx *c, uint64_t at, EvbFile *files, int max_files, int *n_files)
{
    uint8_t h[80];
    if (read_at(c->f, at, h, 80) || memcmp(h, "EVB\0", 4)) return fail(c, "This exe has no packed game files (not the DLC Edition's exe?).");
    uint32_t main_size = rd32(h + 64), main_count = rd32(h + 76);
    uint64_t pos = at + 80 - 1, data = at + 80 + main_size - 12;
    /* folders as a stack of (path, objects left) */
    struct { char path[200]; uint32_t left; } st[8];
    int sp = 0;
    st[0].path[0] = 0, st[0].left = main_count;
    *n_files = 0;
    for (int guard = 0; guard < 10000 && sp >= 0; guard++) {
        while (sp >= 0 && st[sp].left == 0) sp--;
        if (sp < 0) break;
        uint8_t node[16];
        if (read_at(c->f, pos, node, 16)) return fail(c, "The exe's file table is cut short.");
        uint32_t count = rd32(node + 12);
        pos += 16;
        char name[128];
        int nl = 0;
        for (;;) {
            uint8_t w[2];
            if (read_at(c->f, pos, w, 2)) return fail(c, "The exe's file table is cut short.");
            pos += 2;
            if (!w[0] && !w[1]) break;
            if (nl < (int)sizeof name - 1) name[nl++] = w[1] || w[0] < 0x20 || w[0] > 0x7e ? '_' : (char)w[0];
        }
        name[nl] = 0;
        uint8_t type;
        if (read_at(c->f, pos, &type, 1)) return fail(c, "The exe's file table is cut short.");
        pos += 1;
        if (!strcmp(name, "..") || !strcmp(name, ".") || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':'))
            return fail(c, "The exe's file table has a bad name.");
        st[sp].left--;
        if (type == 2) {
            uint8_t o[53];
            if (read_at(c->f, pos, o, 53)) return fail(c, "The exe's file table is cut short.");
            pos += 53;
            uint32_t orig = rd32(o + 2), stored = rd32(o + 49);
            if (orig != stored) return fail(c, "The exe's files are compressed (not the DLC Edition 2.7.2 exe?).");
            if (*n_files == max_files) return fail(c, "The exe has more packed files than expected.");
            EvbFile *e = &files[(*n_files)++];
            snprintf(e->path, sizeof e->path, "%s%s%s", st[sp].path, st[sp].path[0] ? "/" : "", name);
            e->offset = data, e->size = stored;
            data += stored;
        } else if (type == 3) {
            pos += 25;
            if (sp == 7) return fail(c, "The exe's folders are nested too deeply.");
            sp++;
            if (!strcmp(name, "%DEFAULT FOLDER%")) snprintf(st[sp].path, sizeof st[sp].path, "%s", st[sp - 1].path);
            else snprintf(st[sp].path, sizeof st[sp].path, "%s%s%s", st[sp - 1].path, st[sp - 1].path[0] ? "/" : "", name);
            st[sp].left = count;
        } else {
            break;
        }
    }
    return 0;
}

int evb_unpack(const char *exe, const char *dir, EvbProgress progress, char *err, size_t errlen)
{
    Ctx c = {0};
    c.err = err, c.errlen = errlen, c.progress = progress;
    c.f = fopen(exe, "rb");
    if (!c.f) return fail(&c, "The exe couldn't be opened.");
    int r = -1;
    uint8_t *hdr = malloc(4096);
    EvbFile files[32];
    int n = 0;
    if (!hdr || read_at(c.f, 0, hdr, 4096) || hdr[0] != 'M' || hdr[1] != 'Z') { fail(&c, "That isn't a Windows exe."); goto out; }
    uint32_t pe = rd32(hdr + 0x3c);
    if (pe > 4096 - 248 || memcmp(hdr + pe, "PE\0\0", 4) || rd32(hdr + pe + 4) >> 16 == 0) { fail(&c, "That isn't a Windows exe."); goto out; }
    uint32_t nsec = hdr[pe + 6] | hdr[pe + 7] << 8, opt = pe + 24, optsz = hdr[pe + 20] | hdr[pe + 21] << 8;
    uint32_t secs = opt + optsz;
    if (hdr[opt] != 0x0b || hdr[opt + 1] != 0x01 || secs + nsec * 40 > 4096) { fail(&c, "That isn't the DLC Edition's 32-bit exe."); goto out; }
    /* the Enigma sections, last two */
    int e1 = -1, e2 = -1;
    for (uint32_t i = 0; i < nsec; i++) {
        const char *nm = (const char *)hdr + secs + i * 40;
        if (!strncmp(nm, ".enigma1", 8)) e1 = (int)i;
        if (!strncmp(nm, ".enigma2", 8)) e2 = (int)i;
    }
    if (e1 < 0 || e2 != e1 + 1 || (uint32_t)e2 != nsec - 1) { fail(&c, "That exe isn't packed the way the DLC Edition is (not the DLC Edition 2.7.2 exe?)."); goto out; }
    uint32_t e1_raw = rd32(hdr + secs + e1 * 40 + 20);
    if (e1_raw < 4096 || e1_raw > 64u << 20) { fail(&c, "That exe isn't the DLC Edition 2.7.2 exe."); goto out; }
    /* .enigma1's header (evbunpack's x86 10_70): the original TLS directory's first 16 bytes, then the import
     * (+84), relocation (+92) and TLS (+100) directories */
    uint8_t eh[128];
    if (read_at(c.f, e1_raw, eh, sizeof eh)) { fail(&c, "The exe is cut short."); goto out; }
    /* the file table: "EVB\0" within .enigma1 */
    uint64_t table = 0;
    {
        static uint8_t buf[1 << 16];
        for (uint64_t at = e1_raw; at < (uint64_t)e1_raw + (4u << 20) && !table; at += sizeof buf - 4) {
            size_t got;
            if (fseeko(c.f, (off_t)at, SEEK_SET) || (got = fread(buf, 1, sizeof buf, c.f)) < 4) break;
            for (size_t i = 0; i + 4 <= got; i++)
                if (!memcmp(buf + i, "EVB\0", 4)) { table = at + i; break; }
        }
    }
    if (!table) { fail(&c, "This exe has no packed game files (not the DLC Edition's exe?)."); goto out; }
    if (read_table(&c, table, files, 32, &n)) goto out;
    int have_pak = 0;
    for (int i = 0; i < n; i++) {
        c.total += files[i].size;
        if (!strcmp(files[i].path, "main.pak")) have_pak = 1;
    }
    if (!have_pak) { fail(&c, "This exe has no main.pak inside (not the DLC Edition's exe?)."); goto out; }
    c.total += e1_raw;
    /* the game's exe first (the version check: a wrong exe stops here, before 200 MB are copied) */
    {
        uint8_t *img = malloc(e1_raw);
        if (!img || read_at(c.f, 0, img, e1_raw)) { free(img); fail(&c, "The exe couldn't be read."); goto out; }
        img[pe + 6] = (uint8_t)(nsec - 2), img[pe + 7] = (uint8_t)((nsec - 2) >> 8);
        uint8_t *dd = img + opt + 96; /* the data directories */
        wr32(dd + 1 * 8, rd32(eh + 84)), wr32(dd + 1 * 8 + 4, rd32(eh + 88));  /* import */
        wr32(dd + 5 * 8, rd32(eh + 92)), wr32(dd + 5 * 8 + 4, rd32(eh + 96));  /* relocations */
        /* TLS: its directory as it was (the header's copy of its first 12 bytes, found in the sections) */
        uint32_t tls = 0;
        for (uint32_t i = 0; i + 2 < nsec && !tls; i++) {
            const uint8_t *s = img + secs + i * 40;
            uint32_t va = rd32(s + 12), raw = rd32(s + 20), rsz = rd32(s + 16);
            if (!raw || raw + rsz > e1_raw) continue;
            for (uint32_t k = 0; k + 12 <= rsz; k++)
                if (!memcmp(img + raw + k, eh, 12)) { tls = va + k; break; }
        }
        if (tls) wr32(dd + 9 * 8, tls), wr32(dd + 9 * 8 + 4, 24);
        else wr32(dd + 9 * 8, 0), wr32(dd + 9 * 8 + 4, 0);
        wr32(dd + 3 * 8, 0), wr32(dd + 3 * 8 + 4, 0); /* (no exception directory in a 32-bit exe) */
        uint32_t crc = crc32_update(0, img, e1_raw);
        if (e1_raw != DLC_EXE_SIZE || crc != DLC_EXE_CRC) {
            free(img);
            fail(&c, "This is a different version of the DLC Edition. The port needs v2.7.2\n  (the exe made by the DLC's 2.7.2 patch).");
            goto out;
        }
        char path[300], part[310];
        snprintf(path, sizeof path, "%s/dlc_game.exe", dir);
        snprintf(part, sizeof part, "%s.part", path);
        FILE *o = fopen(part, "wb");
        int ok = o && fwrite(img, 1, e1_raw, o) == e1_raw;
        if (o && fclose(o)) ok = 0;
        free(img);
        if (!ok) { remove(part); fail(&c, "The SD card is full: the game's files need about 230 MB."); goto out; }
        c.done += e1_raw;
        /* (renamed last of all: dlc_game.exe and main.pak there = unpacked) */
        for (int i = 0; i < n; i++) {
            char out[300];
            snprintf(out, sizeof out, "%s/%s", dir, files[i].path);
            for (char *p = out + strlen(dir) + 1; *p; p++) /* the folders on the way */
                if (*p == '/') { *p = 0; mkdir(out, 0777); *p = '/'; }
            uint32_t fcrc;
            if (copy_out(&c, files[i].offset, files[i].size, out, &fcrc)) { remove(part); goto out; }
            if (!strcmp(files[i].path, "main.pak") && fcrc != MAIN_PAK_CRC) {
                remove(out), remove(part);
                fail(&c, "main.pak inside the exe doesn't match the DLC Edition 2.7.2\n  (the exe may be damaged: copy it again).");
                goto out;
            }
        }
        remove(path);
        if (rename(part, path)) { fail(&c, "Couldn't write to the SD card."); goto out; }
    }
    r = 0;
out:
    free(hdr);
    fclose(c.f);
    return r;
}
