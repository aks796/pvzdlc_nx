/* hle_advapi32.c: the registry, kept in memory and saved to
 * <game>/registry.txt so settings survive restarts.
 * Keys are full paths like "HKCU\Software\PopCap\PlantsVsZombies". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rt.h"

typedef struct RegVal {
    char key[256], name[128];
    uint32_t type, len;
    uint8_t *data;
    struct RegVal *next;
} RegVal;
typedef struct RegKey {
    char path[256];
    struct RegKey *next;
} RegKey;
typedef struct {
    RtObj obj;
    char path[256];
} KeyObj;

static RegVal *vals;
static RegKey *keys;
static int loaded;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void save(void)
{
    char p[600];
    snprintf(p, sizeof p, "%s/registry.txt", rt_game_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (RegKey *k = keys; k; k = k->next) fprintf(f, "K\t%s\n", k->path);
    for (RegVal *v = vals; v; v = v->next) {
        fprintf(f, "V\t%s\t%s\t%u\t", v->key, v->name, (unsigned)v->type);
        for (uint32_t i = 0; i < v->len; i++) fprintf(f, "%02x", v->data[i]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void add_key(const char *path)
{
    for (RegKey *k = keys; k; k = k->next)
        if (!strcasecmp(k->path, path)) return;
    RegKey *k = calloc(1, sizeof *k);
    snprintf(k->path, sizeof k->path, "%s", path);
    k->next = keys;
    keys = k;
}

static RegVal *find_val(const char *key, const char *name)
{
    for (RegVal *v = vals; v; v = v->next)
        if (!strcasecmp(v->key, key) && !strcasecmp(v->name, name)) return v;
    return NULL;
}

static void set_val(const char *key, const char *name, uint32_t type, const uint8_t *data, uint32_t len)
{
    RegVal *v = find_val(key, name);
    if (!v) {
        v = calloc(1, sizeof *v);
        snprintf(v->key, sizeof v->key, "%s", key);
        snprintf(v->name, sizeof v->name, "%s", name);
        v->next = vals;
        vals = v;
    }
    free(v->data);
    v->data = malloc(len ? len : 1);
    memcpy(v->data, data, len);
    v->len = len;
    v->type = type;
}

static void load(void)
{
    if (loaded) return;
    loaded = 1;
    char p[600];
    snprintf(p, sizeof p, "%s/registry.txt", rt_game_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'K' && line[1] == '\t') {
            add_key(line + 2);
        } else if (line[0] == 'V' && line[1] == '\t') {
            char *save_p = NULL;
            char *key = strtok_r(line + 2, "\t", &save_p), *name = strtok_r(NULL, "\t", &save_p);
            char *type = strtok_r(NULL, "\t", &save_p), *hex = strtok_r(NULL, "\t", &save_p);
            if (!key || !name || !type) continue;
            uint8_t buf[2048];
            uint32_t n = 0;
            for (const char *h = hex ? hex : ""; h[0] && h[1] && n < sizeof buf; h += 2) {
                unsigned b;
                sscanf(h, "%2x", &b);
                buf[n++] = (uint8_t)b;
            }
            set_val(key, name, (uint32_t)atoi(type), buf, n);
        }
    }
    fclose(f);
}

static int base_path(uint32_t hkey, char *out, size_t cap)
{
    switch (hkey) {
    case 0x80000000u: snprintf(out, cap, "HKCR"); return 1;
    case 0x80000001u: snprintf(out, cap, "HKCU"); return 1;
    case 0x80000002u: snprintf(out, cap, "HKLM"); return 1;
    case 0x80000003u: snprintf(out, cap, "HKU"); return 1;
    default: {
        KeyObj *k = (KeyObj *)rt_handle_get(hkey, OBJ_REGKEY);
        if (!k) return 0;
        snprintf(out, cap, "%s", k->path);
        return 1;
    }
    }
}

static int full_path(uint32_t hkey, uint32_t sub, char *out, size_t cap)
{
    char base[256], s[256] = "";
    if (!base_path(hkey, base, sizeof base)) return 0;
    if (sub) rt_a2u(sub, s, sizeof s);
    if (s[0]) snprintf(out, cap, "%s\\%s", base, s);
    else snprintf(out, cap, "%s", base);
    return 1;
}

static int key_exists(const char *path)
{
    size_t n = strlen(path);
    for (RegKey *k = keys; k; k = k->next)
        if (!strncasecmp(k->path, path, n) && (k->path[n] == 0 || k->path[n] == '\\')) return 1;
    return 0;
}

static uint32_t open_handle(const char *path)
{
    KeyObj *k = calloc(1, sizeof *k);
    k->obj.type = OBJ_REGKEY;
    snprintf(k->path, sizeof k->path, "%s", path);
    return rt_handle_new(&k->obj);
}

static uint32_t reg_open(uint32_t hkey, uint32_t sub, uint32_t out, int create)
{
    char path[512];
    pthread_mutex_lock(&lock);
    load();
    if (!full_path(hkey, sub, path, sizeof path)) {
        pthread_mutex_unlock(&lock);
        return ERROR_INVALID_HANDLE;
    }
    if (!key_exists(path)) {
        if (!create) {
            pthread_mutex_unlock(&lock);
            if (rt_verbose) rt_log("[reg] open %s: not found\n", path);
            return ERROR_FILE_NOT_FOUND;
        }
        add_key(path);
        save();
    }
    pthread_mutex_unlock(&lock);
    g_wr32(out, open_handle(path));
    return ERROR_SUCCESS;
}

HLE(RegOpenKeyExA) { RET(5, reg_open(ARG(0), ARG(1), ARG(4), 0)); }
HLE(RegOpenKeyA) { RET(3, reg_open(ARG(0), ARG(1), ARG(2), 0)); }
HLE(RegCreateKeyExA)
{
    uint32_t r = reg_open(ARG(0), ARG(1), ARG(7), 1);
    if (ARG(8)) g_wr32(ARG(8), 2); /* REG_OPENED_EXISTING_KEY */
    RET(9, r);
}
HLE(RegCloseKey)
{
    if (ARG(0) < 0x80000000u) rt_handle_close(ARG(0));
    RET(1, ERROR_SUCCESS);
}
HLE(RegQueryValueExA)
{
    uint32_t hkey = ARG(0), type_out = ARG(3), data = ARG(4), cb = ARG(5);
    char key[256], name[128] = "";
    if (!base_path(hkey, key, sizeof key)) RET(6, ERROR_INVALID_HANDLE);
    if (ARG(1)) rt_a2u(ARG(1), name, sizeof name);
    pthread_mutex_lock(&lock);
    load();
    RegVal *v = find_val(key, name);
    uint32_t r = ERROR_SUCCESS;
    if (!v) {
        r = ERROR_FILE_NOT_FOUND;
    } else {
        if (type_out) g_wr32(type_out, v->type);
        if (data) {
            if (!cb || g_rd32(cb) < v->len) r = 234; /* ERROR_MORE_DATA */
            else memcpy(G2H(data), v->data, v->len);
        }
        if (cb) g_wr32(cb, v->len);
    }
    pthread_mutex_unlock(&lock);
    if (rt_verbose) rt_log("[reg] query %s\\%s -> %u\n", key, name, (unsigned)r);
    RET(6, r);
}
HLE(RegSetValueExA)
{
    uint32_t hkey = ARG(0), type = ARG(3), data = ARG(4), len = ARG(5);
    char key[256], name[128] = "";
    if (!base_path(hkey, key, sizeof key)) RET(6, ERROR_INVALID_HANDLE);
    if (ARG(1)) rt_a2u(ARG(1), name, sizeof name);
    pthread_mutex_lock(&lock);
    load();
    set_val(key, name, type, G2H(data), len);
    save();
    pthread_mutex_unlock(&lock);
    RET(6, ERROR_SUCCESS);
}
HLE(RegDeleteValueA)
{
    char key[256], name[128] = "";
    if (!base_path(ARG(0), key, sizeof key)) RET(2, ERROR_INVALID_HANDLE);
    if (ARG(1)) rt_a2u(ARG(1), name, sizeof name);
    pthread_mutex_lock(&lock);
    load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    for (RegVal **pp = &vals; *pp; pp = &(*pp)->next) {
        if (!strcasecmp((*pp)->key, key) && !strcasecmp((*pp)->name, name)) {
            RegVal *v = *pp;
            *pp = v->next;
            free(v->data);
            free(v);
            r = ERROR_SUCCESS;
            save();
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    RET(2, r);
}
HLE(RegDeleteKeyA)
{
    char path[512];
    if (!full_path(ARG(0), ARG(1), path, sizeof path)) RET(2, ERROR_INVALID_HANDLE);
    pthread_mutex_lock(&lock);
    load();
    for (RegKey **pp = &keys; *pp;) {
        if (!strcasecmp((*pp)->path, path)) {
            RegKey *k = *pp;
            *pp = k->next;
            free(k);
        } else {
            pp = &(*pp)->next;
        }
    }
    save();
    pthread_mutex_unlock(&lock);
    RET(2, ERROR_SUCCESS);
}
HLE(RegEnumKeyA)
{
    char key[256];
    if (!base_path(ARG(0), key, sizeof key)) RET(4, ERROR_INVALID_HANDLE);
    uint32_t want = ARG(1), idx = 0;
    size_t n = strlen(key);
    pthread_mutex_lock(&lock);
    load();
    for (RegKey *k = keys; k; k = k->next) {
        if (!strncasecmp(k->path, key, n) && k->path[n] == '\\' && !strchr(k->path + n + 1, '\\')) {
            if (idx++ == want) {
                rt_u2a(k->path + n + 1, ARG(2), ARG(3));
                pthread_mutex_unlock(&lock);
                RET(4, ERROR_SUCCESS);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    RET(4, ERROR_NO_MORE_ITEMS);
}

const HleEntry hle_advapi32[] = {
#define A(n) {"advapi32.dll", #n, n}
    A(RegOpenKeyExA), A(RegOpenKeyA), A(RegCreateKeyExA), A(RegCloseKey), A(RegQueryValueExA), A(RegSetValueExA),
    A(RegDeleteValueA), A(RegDeleteKeyA), A(RegEnumKeyA),
#undef A
    {NULL, NULL, NULL}};
