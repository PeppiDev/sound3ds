#include "playlists.h"
#include "debuglog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct {
    char name[PLAYLIST_NAME_LEN];
    bool m3u8;               // el archivo es .m3u8 (se guarda con la misma extensión)
    int n;
    char* paths[PLAYLIST_MAX_SONGS];
} Playlist;

static Playlist s_pl[PLAYLISTS_MAX];
static int s_count = 0;
static uint32_t s_gen = 1;

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

// Nombre que sirve como archivo en FAT: sin caracteres prohibidos ni espacio o punto al final
static void clean_name(const char* raw, char* out, size_t out_sz) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)raw; *p && o + 1 < out_sz; p++) {
        if (*p < 0x20 || strchr("\\/:*?\"<>|", *p)) continue;
        out[o++] = (char)*p;
    }
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;   // no cortar un carácter UTF-8
    if (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0xC0) o--;
    out[o] = '\0';
    char* s = out;
    while (*s == ' ' || *s == '.') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
    o = strlen(out);
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = '\0';
}

static void file_path(const char* name, bool m3u8, char* out, size_t out_sz) {
    snprintf(out, out_sz, PLAYLISTS_DIR "/%s.%s", name, m3u8 ? "m3u8" : "m3u");
}

static void free_playlist(Playlist* p) {
    for (int k = 0; k < p->n; k++) free(p->paths[k]);
    p->n = 0;
}

// Línea de un .m3u -> ruta de la SD ("/music/x.mp3" o "music/x.mp3" -> "sdmc:/music/x.mp3")
static char* to_sd_path(const char* line) {
    char buf[300];
    if (strncasecmp(line, "sdmc:", 5) == 0) snprintf(buf, sizeof(buf), "%s", line);
    else if (line[0] == '/') snprintf(buf, sizeof(buf), "sdmc:%s", line);
    else snprintf(buf, sizeof(buf), "sdmc:/%s", line);
    for (char* c = buf; *c; c++)
        if (*c == '\\') *c = '/';   // listas hechas en Windows
    return strdup(buf);
}

static void load_file(const char* fname) {
    if (s_count >= PLAYLISTS_MAX) return;
    char path[320];
    snprintf(path, sizeof(path), PLAYLISTS_DIR "/%s", fname);
    FILE* f = fopen(path, "r");
    if (!f) return;
    Playlist* p = &s_pl[s_count];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%.*s", (int)(strrchr(fname, '.') - fname), fname);
    p->m3u8 = has_ext(fname, ".m3u8");
    char line[320];
    while (fgets(line, sizeof(line), f) && p->n < PLAYLIST_MAX_SONGS) {
        size_t n = strcspn(line, "\r\n");
        line[n] = '\0';
        const char* s = line;
        if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        char* sd = to_sd_path(s);
        if (sd) p->paths[p->n++] = sd;
    }
    fclose(f);
    s_count++;
}

static int cmp_playlists(const void* a, const void* b) {
    return strcasecmp(((const Playlist*)a)->name, ((const Playlist*)b)->name);
}

void playlists_init(void) {
    for (int i = 0; i < s_count; i++) free_playlist(&s_pl[i]);
    s_count = 0;
    DIR* d = opendir(PLAYLISTS_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL && s_count < PLAYLISTS_MAX)
            if (e->d_name[0] != '.' && (has_ext(e->d_name, ".m3u") || has_ext(e->d_name, ".m3u8"))) load_file(e->d_name);
        closedir(d);
    }
    qsort(s_pl, s_count, sizeof(Playlist), cmp_playlists);
    s_gen++;
    if (s_count > 0) dlog("playlists: %d", s_count);
}

static bool save(int i) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    mkdir(PLAYLISTS_DIR, 0777);
    char path[320], tmp[330];
    file_path(s_pl[i].name, s_pl[i].m3u8, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fputs("#EXTM3U\n", f) >= 0;
    for (int k = 0; k < s_pl[i].n && ok; k++) ok = fprintf(f, "%s\n", s_pl[i].paths[k]) > 0;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    s_gen++;
    return ok;
}

int playlists_count(void) {
    return s_count;
}

const char* playlists_name(int i) {
    return i >= 0 && i < s_count ? s_pl[i].name : "";
}

int playlists_size(int i) {
    return i >= 0 && i < s_count ? s_pl[i].n : 0;
}

const char* playlists_song(int i, int k) {
    return i >= 0 && i < s_count && k >= 0 && k < s_pl[i].n ? s_pl[i].paths[k] : "";
}

bool playlists_contains(int i, const char* path) {
    if (i < 0 || i >= s_count) return false;
    for (int k = 0; k < s_pl[i].n; k++)
        if (strcmp(s_pl[i].paths[k], path) == 0) return true;
    return false;
}

static int find_name(const char* name) {
    for (int i = 0; i < s_count; i++)
        if (strcasecmp(s_pl[i].name, name) == 0) return i;
    return -1;
}

int playlists_create(const char* raw) {
    char name[PLAYLIST_NAME_LEN];
    clean_name(raw, name, sizeof(name));
    if (!name[0] || s_count >= PLAYLISTS_MAX || find_name(name) >= 0) return -1;
    Playlist* p = &s_pl[s_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    if (!save(s_count - 1)) {
        s_count--;
        return -1;
    }
    // Mantener el orden alfabético y devolver la nueva posición
    qsort(s_pl, s_count, sizeof(Playlist), cmp_playlists);
    return find_name(name);
}

bool playlists_add(int i, const char* path) {
    if (i < 0 || i >= s_count || s_pl[i].n >= PLAYLIST_MAX_SONGS || playlists_contains(i, path)) return false;
    char* copy = strdup(path);
    if (!copy) return false;
    s_pl[i].paths[s_pl[i].n++] = copy;
    if (!save(i)) {
        free(s_pl[i].paths[--s_pl[i].n]);
        return false;
    }
    return true;
}

bool playlists_remove(int i, const char* path) {
    if (i < 0 || i >= s_count) return false;
    Playlist* p = &s_pl[i];
    for (int k = 0; k < p->n; k++) {
        if (strcmp(p->paths[k], path) != 0) continue;
        free(p->paths[k]);
        memmove(&p->paths[k], &p->paths[k + 1], (size_t)(p->n - k - 1) * sizeof(char*));
        p->n--;
        return save(i);
    }
    return false;
}

bool playlists_rename(int i, const char* raw) {
    if (i < 0 || i >= s_count) return false;
    char name[PLAYLIST_NAME_LEN], old_path[320], new_path[320];
    clean_name(raw, name, sizeof(name));
    if (!name[0]) return false;
    int other = find_name(name);
    if (other >= 0 && other != i) return false;
    file_path(s_pl[i].name, s_pl[i].m3u8, old_path, sizeof(old_path));
    file_path(name, s_pl[i].m3u8, new_path, sizeof(new_path));
    if (strcmp(old_path, new_path) != 0 && rename(old_path, new_path) != 0) return false;
    snprintf(s_pl[i].name, sizeof(s_pl[i].name), "%s", name);
    qsort(s_pl, s_count, sizeof(Playlist), cmp_playlists);
    s_gen++;
    return true;
}

bool playlists_delete(int i) {
    if (i < 0 || i >= s_count) return false;
    char path[320];
    file_path(s_pl[i].name, s_pl[i].m3u8, path, sizeof(path));
    remove(path);
    free_playlist(&s_pl[i]);
    memmove(&s_pl[i], &s_pl[i + 1], (size_t)(s_count - i - 1) * sizeof(Playlist));
    s_count--;
    s_gen++;
    return true;
}

uint32_t playlists_generation(void) {
    return s_gen;
}
