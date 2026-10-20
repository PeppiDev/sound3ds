#include "headset.h"
#include "debuglog.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Registros del códec (página 0), según GBATEK "DSi Touchscreen/Sound Controller"
#define CDC_PAGE_0          0
#define CDC_REG_FLAGS_DAC   0x2C   // banderas que se quedan marcadas hasta leerlas
#define CDC_REG_HEADSET     0x43   // detección de audífonos
#define CDC_BTN_BIT         0x20   // bit 5: botón de los audífonos apretado
#define CDC_HS_ENABLE       0x80   // 0x43 bit 7: detección activada
#define CDC_HS_TYPE(v)      (((v) >> 5) & 3)   // 0 nada, 1 audífonos, 3 audífonos + micrófono

// Toques seguidos dentro de esta ventana forman un gesto (1, 2 o 3 toques)
#define CLICK_WINDOW_MS     450

static bool s_ok = false;
static bool s_prev_down = false;
static int s_clicks = 0;
static u64 s_last_click = 0;
static u8 s_r2c = 0, s_r2e = 0, s_r43 = 0;
static unsigned s_presses = 0;

// ---------------------------------------------------------------------------
// Micrófono: con audífonos conectados se enciende para que el códec alimente la línea
// del micrófono (MICBIAS); sin eso el botón no cambia nada que se pueda detectar. Para
// el diagnóstico se guardan los últimos segundos de señal y se pueden volcar a un WAV.
// ---------------------------------------------------------------------------
#define MIC_BUF_SIZE    0x8000
#define MIC_RATE        8182
#define REC_SAMPLES     (MIC_RATE * 8)
static u8* s_mic_buf = NULL;
static bool s_mic_ok = false, s_mic_on = false;
static u32 s_mic_pos = 0;
static u64 s_mic_retry_at = 0;
static s16* s_rec = NULL;
static u32 s_rec_pos = 0, s_rec_count = 0;
static s16 s_win_min = 0, s_win_max = 0, s_show_min = 0, s_show_max = 0;
static u64 s_win_start = 0;

static void mic_start(void) {
    if (!s_mic_buf) s_mic_buf = memalign(0x1000, MIC_BUF_SIZE);
    if (!s_mic_buf) return;
    memset(s_mic_buf, 0, MIC_BUF_SIZE);
    Result r = micInit(s_mic_buf, MIC_BUF_SIZE);
    s_mic_ok = R_SUCCEEDED(r);
    if (!s_mic_ok) {
        dlog("audifonos: mic:u 0x%08lX", (unsigned long)r);
        return;
    }
    MICU_SetAllowShellClosed(true);
    MICU_SetPower(true);
    Result rs = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_8180, 0, micGetSampleDataSize(), true);
    s_mic_pos = 0;
    s_mic_on = true;
    u8 bias = 0;
    if (s_ok) CDCCHK_ReadRegisters2(1, 0x2E, &bias, 1);
    dlog("audifonos: microfono encendido (0x%08lX, MICBIAS 1:2E=%02X)", (unsigned long)rs, bias);
}

static void mic_stop(void) {
    if (!s_mic_ok) return;
    MICU_StopSampling();
    MICU_SetPower(false);
    micExit();
    s_mic_ok = s_mic_on = false;
    dlog("audifonos: microfono apagado");
}

static void mic_poll(u64 now_ms) {
    if (!s_mic_on) return;
    u32 size = micGetSampleDataSize();
    u32 last = micGetLastSampleOffset();
    if (last >= size) return;
    if (!s_rec) s_rec = malloc(REC_SAMPLES * sizeof(s16));
    u32 n = 0;
    while (s_mic_pos != last && n < size / 2) {
        s16 v;
        memcpy(&v, s_mic_buf + s_mic_pos, 2);
        if (v < s_win_min) s_win_min = v;
        if (v > s_win_max) s_win_max = v;
        if (s_rec) {
            s_rec[s_rec_pos] = v;
            s_rec_pos = (s_rec_pos + 1) % REC_SAMPLES;
            if (s_rec_count < REC_SAMPLES) s_rec_count++;
        }
        s_mic_pos = (s_mic_pos + 2) % size;
        n++;
    }
    if (now_ms - s_win_start >= 500) {
        s_show_min = s_win_min;
        s_show_max = s_win_max;
        s_win_min = s_win_max = 0;
        s_win_start = now_ms;
    }
}

static void put_le32(FILE* f, u32 v) {
    u8 b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24 };
    fwrite(b, 1, 4, f);
}

static void put_le16(FILE* f, u16 v) {
    u8 b[2] = { v & 0xFF, v >> 8 };
    fwrite(b, 1, 2, f);
}

bool headset_dump_wav(char* path_out, size_t path_sz) {
    if (!s_rec || s_rec_count == 0) return false;
    static int n = 0;
    mkdir("sdmc:/3ds/sound3ds", 0777);
    snprintf(path_out, path_sz, "sdmc:/3ds/sound3ds/mic_%d.wav", ++n);
    FILE* f = fopen(path_out, "wb");
    if (!f) return false;
    u32 bytes = s_rec_count * 2;
    fwrite("RIFF", 1, 4, f);
    put_le32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le32(f, 16);
    put_le16(f, 1);
    put_le16(f, 1);
    put_le32(f, MIC_RATE);
    put_le32(f, MIC_RATE * 2);
    put_le16(f, 2);
    put_le16(f, 16);
    fwrite("data", 1, 4, f);
    put_le32(f, bytes);
    u32 start = (s_rec_pos + REC_SAMPLES - s_rec_count) % REC_SAMPLES;
    for (u32 i = 0; i < s_rec_count; i++) put_le16(f, (u16)s_rec[(start + i) % REC_SAMPLES]);
    fclose(f);
    dlog("audifonos: grabacion guardada en %s (%lu muestras)", path_out, (unsigned long)s_rec_count);
    return true;
}

// ---------------------------------------------------------------------------
// Escáner de registros del códec (diagnóstico): se leen varias páginas completas; los
// primeros segundos se aprende qué bits cambian solos y después se anota cualquier otro
// cambio. Si el botón está en algún registro, aparece al apretarlo.
// ---------------------------------------------------------------------------
static const u8 s_scan_pages[] = { 0, 1, 3, 8, 9, 100, 101, 102, 103 };
#define SCAN_NPAGES   (sizeof(s_scan_pages))
#define SCAN_LEARN_MS 3000
static bool s_scan_on = false;
static u64 s_scan_start = 0, s_scan_last = 0;
static u8 s_regs[SCAN_NPAGES][128];
static u8 s_noisy[SCAN_NPAGES][128];   // bits que cambiaron durante el aprendizaje
static bool s_regs_valid = false;
static char s_events[3][40];
static unsigned s_event_count = 0;

static void scan_read(u8 out[SCAN_NPAGES][128]) {
    for (unsigned p = 0; p < SCAN_NPAGES; p++) {
        if (R_FAILED(CDCCHK_ReadRegisters2(s_scan_pages[p], 0, out[p], 64))) memset(out[p], 0, 64);
        if (R_FAILED(CDCCHK_ReadRegisters2(s_scan_pages[p], 64, out[p] + 64, 64))) memset(out[p] + 64, 0, 64);
    }
}

void headset_scan_enable(bool on) {
    if (on == s_scan_on) return;
    s_scan_on = on;
    s_regs_valid = false;
    if (on) {
        s_scan_start = osGetTime();
        memset(s_noisy, 0, sizeof(s_noisy));
        s_event_count = 0;
        for (int i = 0; i < 3; i++) s_events[i][0] = '\0';
        dlog("audifonos: escaner de registros activo");
    }
}

static void scan_poll(u64 now_ms) {
    if (!s_scan_on || !s_ok || now_ms - s_scan_last < 33) return;
    s_scan_last = now_ms;
    static u8 cur[SCAN_NPAGES][128];
    scan_read(cur);
    if (!s_regs_valid) {
        memcpy(s_regs, cur, sizeof(s_regs));
        s_regs_valid = true;
        return;
    }
    bool learning = now_ms - s_scan_start < SCAN_LEARN_MS;
    for (unsigned p = 0; p < SCAN_NPAGES; p++) {
        for (int r = 0; r < 128; r++) {
            u8 diff = cur[p][r] ^ s_regs[p][r];
            if (!diff) continue;
            if (learning) {
                s_noisy[p][r] |= diff;
            } else if (diff & ~s_noisy[p][r]) {
                char ev[40];
                snprintf(ev, sizeof(ev), "%u:%02X %02X>%02X", s_scan_pages[p], r, s_regs[p][r], cur[p][r]);
                dlog("audifonos: registro %s", ev);
                memmove(s_events[1], s_events[0], sizeof(s_events[0]) * 2);
                snprintf(s_events[0], sizeof(s_events[0]), "%s", ev);
                s_event_count++;
            }
            s_regs[p][r] = cur[p][r];
        }
    }
}

// ---------------------------------------------------------------------------

void headset_init(void) {
    // cdc:CHK sin bloquear: en emuladores no existe y no hay que quedarse esperando
    srvSetBlockingPolicy(true);
    Result res = cdcChkInit();
    srvSetBlockingPolicy(false);
    s_ok = R_SUCCEEDED(res);
    dlog("audifonos: cdc:CHK %s (0x%08lX)", s_ok ? "ok" : "no disponible", (unsigned long)res);
    if (!s_ok) return;

    u8 hs = 0;
    if (R_SUCCEEDED(CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_HEADSET, &hs, 1))) {
        dlog("audifonos: reg 0x43 = 0x%02X", hs);
        if (!(hs & CDC_HS_ENABLE)) {
            // El sistema no dejó activada la detección: activarla (rebote del botón 16 ms)
            u8 v = (u8)((hs & 0x1C) | CDC_HS_ENABLE | 0x02);
            Result w = CDCCHK_WriteRegisters2(CDC_PAGE_0, CDC_REG_HEADSET, &v, 1);
            dlog("audifonos: deteccion activada 0x43 <- 0x%02X (0x%08lX)", v, (unsigned long)w);
        }
        s_r43 = hs;
    }
}

void headset_exit(void) {
    mic_stop();
    free(s_mic_buf);
    free(s_rec);
    s_mic_buf = NULL;
    s_rec = NULL;
    if (s_ok) cdcChkExit();
    s_ok = false;
}

HeadsetAction headset_poll(u64 now_ms) {
    // Micrófono encendido solo mientras hay audífonos conectados
    bool plugged = osIsHeadsetConnected();
    if (plugged && !s_mic_on && !s_mic_ok && now_ms >= s_mic_retry_at) {
        mic_start();
        if (!s_mic_ok) s_mic_retry_at = now_ms + 5000;
    } else if (!plugged && s_mic_on) {
        mic_stop();
    }
    mic_poll(now_ms);
    scan_poll(now_ms);
    if (!s_ok) return HEADSET_NONE;

    u8 regs[4];   // 0x2C..0x2F de una vez
    if (R_FAILED(CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_FLAGS_DAC, regs, sizeof(regs)))) return HEADSET_NONE;
    s_r2c = regs[0];
    s_r2e = regs[2];
    CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_HEADSET, &s_r43, 1);

    // Un toque = el bit pasa a 1 en el estado actual, o quedó marcado en 0x2C entre dos
    // lecturas (un toque más corto que un cuadro)
    bool down = (s_r2e & CDC_BTN_BIT) != 0;
    bool sticky = (s_r2c & CDC_BTN_BIT) != 0;
    if ((down && !s_prev_down) || (sticky && !down && !s_prev_down)) {
        s_clicks++;
        s_presses++;
        s_last_click = now_ms;
        dlog("audifonos: toque (2C=%02X 2E=%02X)", s_r2c, s_r2e);
    }
    s_prev_down = down;

    if (s_clicks > 0 && !down && now_ms - s_last_click > CLICK_WINDOW_MS) {
        HeadsetAction a = s_clicks == 1 ? HEADSET_TOGGLE : (s_clicks == 2 ? HEADSET_NEXT : HEADSET_PREV);
        dlog("audifonos: %d toque(s)", s_clicks);
        s_clicks = 0;
        return a;
    }
    return HEADSET_NONE;
}

void headset_diag(char* out, size_t out_sz) {
    static const char* const types[4] = { "-", "HP", "?", "HP+MIC" };
    if (s_scan_on && osGetTime() - s_scan_start < SCAN_LEARN_MS) {
        snprintf(out, out_sz, "aprendiendo registros... no toques nada");
        return;
    }
    snprintf(out, out_sz, "%s mic:%s %d/%d t:%u cambios:%u %s", types[CDC_HS_TYPE(s_r43)],
             s_mic_on ? "on" : "off", s_show_min, s_show_max, s_presses, s_event_count, s_events[0]);
}
