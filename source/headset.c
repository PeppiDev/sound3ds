#include "headset.h"
#include "debuglog.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// El códec del 3DS informa el botón de los audífonos en la página 100 (propia del
// 3DS, no está en el TSC del DSi), registro 0x2C: el bit 1 queda en 1 mientras el
// botón está apretado (encontrado leyendo el códec en una consola: 0x01 suelto,
// 0x23/0x03 apretado). La detección de audífonos de la página 0 (0x43 bit 7) tiene
// que estar activa.
#define CDC_PAGE_0          0
#define CDC_REG_HEADSET     0x43
#define CDC_HS_ENABLE       0x80
#define CDC_BTN_PAGE        100
#define CDC_BTN_REG         0x2C
#define CDC_BTN_HELD        0x02

// Toques seguidos dentro de esta ventana forman un gesto (1, 2 o 3 toques)
#define CLICK_WINDOW_MS     450

static bool s_ok = false;
static bool s_prev_down = false;
static int s_clicks = 0;
static u64 s_last_click = 0;

// ---------------------------------------------------------------------------
// Micrófono: con audífonos conectados se mantiene encendido para que el códec
// alimente la línea del micrófono (MICBIAS); sin ese voltaje el botón no se nota.
// La señal no se usa.
// ---------------------------------------------------------------------------
#define MIC_BUF_SIZE    0x1000
static u8* s_mic_buf = NULL;
static bool s_mic_on = false;
static u64 s_mic_retry_at = 0;

static void mic_start(void) {
    if (!s_mic_buf) s_mic_buf = memalign(0x1000, MIC_BUF_SIZE);
    if (!s_mic_buf) return;
    Result r = micInit(s_mic_buf, MIC_BUF_SIZE);
    if (R_FAILED(r)) {
        dlog("audifonos: mic:u 0x%08lX", (unsigned long)r);
        return;
    }
    MICU_SetAllowShellClosed(true);
    MICU_SetPower(true);
    Result rs = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_8180, 0, micGetSampleDataSize(), true);
    s_mic_on = true;
    dlog("audifonos: microfono encendido (0x%08lX)", (unsigned long)rs);
}

static void mic_stop(void) {
    if (!s_mic_on) return;
    MICU_StopSampling();
    MICU_SetPower(false);
    micExit();
    s_mic_on = false;
    dlog("audifonos: microfono apagado");
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
    if (R_SUCCEEDED(CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_HEADSET, &hs, 1)) && !(hs & CDC_HS_ENABLE)) {
        // El sistema no deja activada la detección: activarla (rebote del botón 16 ms)
        u8 v = (u8)((hs & 0x1C) | CDC_HS_ENABLE | 0x02);
        Result w = CDCCHK_WriteRegisters2(CDC_PAGE_0, CDC_REG_HEADSET, &v, 1);
        dlog("audifonos: deteccion activada 0x43 %02X -> %02X (0x%08lX)", hs, v, (unsigned long)w);
    }
}

void headset_exit(void) {
    mic_stop();
    free(s_mic_buf);
    s_mic_buf = NULL;
    if (s_ok) cdcChkExit();
    s_ok = false;
}

HeadsetAction headset_poll(u64 now_ms) {
    if (!s_ok) return HEADSET_NONE;

    // Micrófono encendido solo mientras hay audífonos conectados
    bool plugged = osIsHeadsetConnected();
    if (plugged && !s_mic_on && now_ms >= s_mic_retry_at) {
        mic_start();
        if (!s_mic_on) s_mic_retry_at = now_ms + 5000;
    } else if (!plugged && s_mic_on) {
        mic_stop();
    }
    if (!plugged) {
        s_prev_down = false;
        s_clicks = 0;
        return HEADSET_NONE;
    }

    u8 reg = 0;
    if (R_FAILED(CDCCHK_ReadRegisters2(CDC_BTN_PAGE, CDC_BTN_REG, &reg, 1))) return HEADSET_NONE;
    bool down = (reg & CDC_BTN_HELD) != 0;
    if (down && !s_prev_down) {
        s_clicks++;
        s_last_click = now_ms;
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
