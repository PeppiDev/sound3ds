#include "debuglog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#define DLOG_PATH "sdmc:/3ds/sound3ds/debug.log"

static LightLock s_lock;
static u64 s_start;
static bool s_ready = false;

void dlog_init(void) {
    LightLock_Init(&s_lock);
    s_start = osGetTime();
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    FILE* f = fopen(DLOG_PATH, "w"); // se reinicia en cada arranque
    if (!f) return;
    fclose(f);
    s_ready = true;
}

void dlog(const char* fmt, ...) {
    if (!s_ready) return;
    LightLock_Lock(&s_lock);
    FILE* f = fopen(DLOG_PATH, "a");
    if (f) {
        fprintf(f, "[%7.2f] ", (double)(osGetTime() - s_start) / 1000.0);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LightLock_Unlock(&s_lock);
}
