#include "qrscan.h"
#include "camera.h"
#include "debuglog.h"
#include "quirc/quirc.h"

#include <stdio.h>
#include <string.h>

#define CAM_STALL_MS 2500   // sin cuadros por este tiempo: relanzar la cámara

static struct {
    bool open, suspended, failed, has_frame;
    struct quirc* q;
    Thread thread;
    volatile bool quit, enabled, found;
    LightLock lock;
    bool lock_ready;
    char text[1024];
    u32 base_seq, shown_seq;
    int restarts;
} s_qr;

static void decode_thread(void* arg) {
    (void)arg;
    while (!s_qr.quit) {
        LightEvent_Wait(camera_frame_event());
        if (s_qr.quit) break;
        if (!s_qr.enabled || s_qr.found) continue;

        int w, h;
        uint8_t* img = quirc_begin(s_qr.q, &w, &h);
        const u16* f = camera_lock_frame();
        for (int i = 0; i < CAM_W * CAM_H; i++) {
            u16 p = f[i];
            int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
            img[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        }
        camera_unlock_frame();
        quirc_end(s_qr.q);

        int n = quirc_count(s_qr.q);
        for (int i = 0; i < n; i++) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(s_qr.q, i, &code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
                quirc_flip(&code);   // por si la cámara entrega la imagen en espejo
                if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
            }
            LightLock_Lock(&s_qr.lock);
            snprintf(s_qr.text, sizeof(s_qr.text), "%.*s", data.payload_len, (const char*)data.payload);
            s_qr.found = true;
            LightLock_Unlock(&s_qr.lock);
            break;
        }
    }
}

static void camera_on(void) {
    if (camera_running()) return;
    s_qr.base_seq = camera_frame_seq();
    if (!camera_start()) s_qr.failed = true;
}

bool qrscan_open(void) {
    if (s_qr.open) return true;
    if (!s_qr.lock_ready) {
        LightLock_Init(&s_qr.lock);
        s_qr.lock_ready = true;
    }
    s_qr.failed = s_qr.has_frame = s_qr.found = s_qr.suspended = false;
    s_qr.restarts = 0;
    s_qr.q = quirc_new();
    if (!s_qr.q || quirc_resize(s_qr.q, CAM_W, CAM_H) < 0) {
        if (s_qr.q) quirc_destroy(s_qr.q);
        s_qr.q = NULL;
        s_qr.failed = true;
        return false;
    }
    camera_on();   // también crea el evento de cuadros que espera el decodificador
    s_qr.quit = false;
    s_qr.enabled = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Menos prioridad que la interfaz: usa el tiempo que le sobra a cada cuadro
    s_qr.thread = threadCreate(decode_thread, NULL, 0x8000, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, false);
    s_qr.open = true;
    dlog("qr: abierto (camara %s)", s_qr.failed ? "falla" : "ok");
    return !s_qr.failed;
}

void qrscan_close(void) {
    if (!s_qr.open) return;
    camera_stop();
    if (s_qr.thread) {
        s_qr.quit = true;
        LightEvent_Signal(camera_frame_event());
        threadJoin(s_qr.thread, U64_MAX);
        threadFree(s_qr.thread);
        s_qr.thread = NULL;
    }
    if (s_qr.q) quirc_destroy(s_qr.q);
    s_qr.q = NULL;
    camera_free();
    s_qr.open = false;
    dlog("qr: cerrado");
}

bool qrscan_is_open(void) { return s_qr.open; }

void qrscan_suspend(void) {
    if (!s_qr.open) return;
    s_qr.suspended = true;
    camera_stop();
}

void qrscan_resume(void) {
    if (!s_qr.open || !s_qr.suspended) return;
    s_qr.suspended = false;
    s_qr.has_frame = false;
    camera_on();
}

void qrscan_rearm(void) {
    LightLock_Lock(&s_qr.lock);
    s_qr.found = false;
    s_qr.text[0] = '\0';
    LightLock_Unlock(&s_qr.lock);
    s_qr.enabled = true;
}

bool qrscan_has_frame(void) { return s_qr.has_frame; }
bool qrscan_failed(void) { return s_qr.failed; }

void qrscan_tick(void) {
    if (!s_qr.open || s_qr.suspended) return;
    if (camera_frame_seq() != s_qr.base_seq) {
        s_qr.has_frame = true;
        s_qr.restarts = 0;
    }
    // Si la captura se cuelga, relanzar la cámara completa
    if (camera_running() && camera_ms_since_frame() > CAM_STALL_MS) {
        camera_stop();
        camera_on();
        if (++s_qr.restarts >= 3 && !s_qr.has_frame) s_qr.failed = true;
    }
}

bool qrscan_lock_new_frame(const u16** frame) {
    if (!s_qr.open || !s_qr.has_frame) return false;
    u32 seq = camera_frame_seq();
    if (seq == s_qr.shown_seq) return false;
    s_qr.shown_seq = seq;
    *frame = camera_lock_frame();
    return true;
}

void qrscan_unlock_frame(void) { camera_unlock_frame(); }

bool qrscan_take(char* out, size_t out_sz) {
    if (!s_qr.lock_ready) return false;
    LightLock_Lock(&s_qr.lock);
    bool got = s_qr.found && s_qr.enabled;
    if (got) {
        snprintf(out, out_sz, "%s", s_qr.text);
        s_qr.enabled = false;   // no leer otro hasta qrscan_rearm
    }
    LightLock_Unlock(&s_qr.lock);
    return got;
}

#ifdef SOUND3DS_AUTOTEST
void qrscan_inject(const char* text) {
    LightLock_Lock(&s_qr.lock);
    snprintf(s_qr.text, sizeof(s_qr.text), "%s", text);
    s_qr.found = true;
    LightLock_Unlock(&s_qr.lock);
}
#endif
