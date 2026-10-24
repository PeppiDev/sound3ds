#include "favorites.h"
#include "local.h"
#include "debuglog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FAV_MAX LOCAL_MAX_TRACKS

static char* s_paths[FAV_MAX];
static int s_count = 0;
static uint32_t s_gen = 1;

static int find(const char* path) {
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_paths[i], path) == 0) return i;
    return -1;
}

static void save(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    FILE* f = fopen(FAVORITES_PATH ".tmp", "w");
    if (!f) return;
    for (int i = 0; i < s_count; i++) fprintf(f, "%s\n", s_paths[i]);
    fclose(f);
    remove(FAVORITES_PATH);
    rename(FAVORITES_PATH ".tmp", FAVORITES_PATH);
}

void favorites_init(void) {
    FILE* f = fopen(FAVORITES_PATH, "r");
    if (!f) return;
    char line[512];
    while (s_count < FAV_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && find(line) < 0) s_paths[s_count++] = strdup(line);
    }
    fclose(f);
    dlog("favoritos: %d", s_count);
}

bool favorites_has(const char* path) {
    return path && find(path) >= 0;
}

bool favorites_toggle(const char* path) {
    int i = find(path);
    if (i >= 0) {
        free(s_paths[i]);
        memmove(&s_paths[i], &s_paths[i + 1], (size_t)(s_count - i - 1) * sizeof(char*));
        s_count--;
    } else if (s_count < FAV_MAX) {
        s_paths[s_count++] = strdup(path);
    }
    s_gen++;
    save();
    return i < 0;
}

int favorites_count(void) {
    return s_count;
}

uint32_t favorites_generation(void) {
    return s_gen;
}
