#include "headset.h"
#include "debuglog.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Registros del códec (página 0), según GBATEK "DSi Touchscreen/Sound Controller"
#define CDC_PAGE_0          0
#define CDC_REG_FLAGS_DAC   0x2C   // banderas que se quedan marcadas hasta leerlas
#define CDC_REG_STATE_DAC   0x2E   // estado actual
#define CDC_REG_HEADSET     0x43   // detección de audífonos
#define CDC_BTN_BIT         0x20   // bit 5: botón de los audífonos apretado
#define CDC_HS_ENABLE       0x80   // 0x43 bit 7: detección activada
#define CDC_HS_TYPE(v)      (((v) >> 5) & 3)   // 0 nada, 1 audífonos, 3 audífonos + micrófono

// Toques seguidos dentro de esta ventana forman un gesto (1, 2 o 3 toques)
#define CLICK_WINDOW_MS     450
#define HEADSET_CHECK_MS    500

static bool s_ok = false;
static bool s_prev_down = false;
static int s_clicks = 0;
static u64 s_last_click = 0;
static u64 s_last_hs_check = 0;
static u8 s_r2c = 0, s_r2e = 0, s_r43 = 0;
static u8 s_logged_2c = 0xFF, s_logged_2e = 0xFF, s_logged_43 = 0xFF;
static unsigned s_presses = 0;
static HeadsetAction s_last_action = HEADSET_NONE;

// Micrófono: con audífonos conectados se enciende (así el códec alimenta la línea del
// micrófono, MICBIAS, y el botón puede notarse) y se vigila la señal. El botón deja la
// línea en corto: un salto enorme y seco, mucho más fuerte que una voz. Se cuenta solo el
// salto hacia un lado (al apretar o al soltar, uno por toque).
#define MIC_BUF_SIZE    0x8000
#define MIC_SPIKE       20000   // |muestra| que cuenta como salto del botón
#define MIC_DEBOUNCE_MS 150
static u8* s_mic_buf = NULL;
static bool s_mic_ok = false, s_mic_on = false;
static u32 s_mic_pos = 0;
static s16 s_win_min = 0, s_win_max = 0;      // ventana de diagnóstico (~0,5 s)
static s16 s_show_min = 0, s_show_max = 0;
static u64 s_win_start = 0, s_last_spike = 0;
static unsigned s_spikes_neg = 0, s_spikes_pos = 0;
static u64 s_mic_retry_at = 0;   // si el micrófono no se pudo abrir, no reintentar en cada cuadro

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
    u8 gain = 0;
    MICU_GetGain(&gain);
    s_mic_pos = 0;
    s_mic_on = true;
    u8 bias = 0;
    if (s_ok) CDCCHK_ReadRegisters2(1, 0x2E, &bias, 1);
    dlog("audifonos: microfono encendido (muestreo 0x%08lX, ganancia %u, MICBIAS 1:2E=%02X)", (unsigned long)rs,
         gain, bias);
}

static void mic_stop(void) {
    if (!s_mic_ok) return;
    MICU_StopSampling();
    MICU_SetPower(false);
    micExit();
    s_mic_ok = s_mic_on = false;
    dlog("audifonos: microfono apagado");
}

// Revisa las muestras nuevas; true si hubo un salto del botón
static bool mic_poll(u64 now_ms) {
    if (!s_mic_on) return false;
    u32 size = micGetSampleDataSize();
    u32 last = micGetLastSampleOffset();
    if (last >= size) return false;
    s16 lo = 0, hi = 0;
    u32 n = 0;
    while (s_mic_pos != last && n < size / 2) {
        s16 v;
        memcpy(&v, s_mic_buf + s_mic_pos, 2);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        s_mic_pos = (s_mic_pos + 2) % size;
        n++;
    }
    if (lo < s_win_min) s_win_min = lo;
    if (hi > s_win_max) s_win_max = hi;
    if (now_ms - s_win_start >= 500) {
        s_show_min = s_win_min;
        s_show_max = s_win_max;
        if (s_win_min < -8000 || s_win_max > 8000) dlog("audifonos: mic min=%d max=%d", s_win_min, s_win_max);
        s_win_min = s_win_max = 0;
        s_win_start = now_ms;
    }
    bool neg = lo <= -MIC_SPIKE, pos = hi >= MIC_SPIKE;
    if (neg) s_spikes_neg++;
    if (pos) s_spikes_pos++;
    if (neg && now_ms - s_last_spike > MIC_DEBOUNCE_MS) {
        s_last_spike = now_ms;
        dlog("audifonos: salto del microfono min=%d max=%d", lo, hi);
        return true;
    }
    if (neg || pos) s_last_spike = now_ms;
    return false;
}

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
    if (s_mic_buf) free(s_mic_buf);
    s_mic_buf = NULL;
    if (s_ok) cdcChkExit();
    s_ok = false;
}

HeadsetAction headset_poll(u64 now_ms) {
    // Micrófono encendido solo mientras hay audífonos conectados
    bool plugged = osIsHeadsetConnected();
    if (plugged && !s_mic_on && !s_mic_ok && now_ms >= s_mic_retry_at) {
        mic_start();
        if (!s_mic_ok) s_mic_retry_at = now_ms + 5000;
    }
    else if (!plugged && s_mic_on) mic_stop();
    bool mic_click = mic_poll(now_ms);

    u8 regs[4] = { 0 };   // 0x2C..0x2F de una vez
    if (s_ok && R_SUCCEEDED(CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_FLAGS_DAC, regs, sizeof(regs)))) {
        s_r2c = regs[0];
        s_r2e = regs[2];
    }
    if (s_ok && now_ms - s_last_hs_check >= HEADSET_CHECK_MS) {
        s_last_hs_check = now_ms;
        CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_HEADSET, &s_r43, 1);
    }
    // Registro solo de los bits que interesan (los de DRC cambian con la música)
    if ((s_r2c & 0x30) != s_logged_2c || (s_r2e & 0x30) != s_logged_2e || s_r43 != s_logged_43) {
        s_logged_2c = s_r2c & 0x30;
        s_logged_2e = s_r2e & 0x30;
        s_logged_43 = s_r43;
        dlog("audifonos: 2C=%02X 2E=%02X 43=%02X", s_r2c, s_r2e, s_r43);
    }

    // Un toque = el bit pasa a 1 en el estado actual, o quedó marcado en 0x2C entre dos
    // lecturas (un toque más corto que un cuadro)
    bool down = (s_r2e & CDC_BTN_BIT) != 0;
    bool sticky = (s_r2c & CDC_BTN_BIT) != 0;
    if ((down && !s_prev_down) || (sticky && !down && !s_prev_down) || mic_click) {
        s_clicks++;
        s_presses++;
        s_last_click = now_ms;
    }
    s_prev_down = down;

    if (s_clicks > 0 && !down && now_ms - s_last_click > CLICK_WINDOW_MS) {
        HeadsetAction a = s_clicks == 1 ? HEADSET_TOGGLE : (s_clicks == 2 ? HEADSET_NEXT : HEADSET_PREV);
        dlog("audifonos: %d toque(s)", s_clicks);
        s_clicks = 0;
        s_last_action = a;
        return a;
    }
    return HEADSET_NONE;
}

void headset_diag(char* out, size_t out_sz) {
    static const char* const types[4] = { "-", "HP", "?", "HP+MIC" };
    static const char* const acts[4] = { "-", "1x", "2x", "3x" };
    snprintf(out, out_sz, "%s 2E=%02X 43=%02X %s mic:%s %d/%d s:%u/%u t:%u %s", s_ok ? "cdc" : "-", s_r2e, s_r43,
             types[CDC_HS_TYPE(s_r43)], s_mic_on ? "on" : (osIsHeadsetConnected() ? "err" : "off"), s_show_min,
             s_show_max, s_spikes_neg, s_spikes_pos, s_presses, acts[s_last_action]);
}
