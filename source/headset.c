#include "headset.h"
#include "debuglog.h"

#include <stdio.h>

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
    if (s_ok) cdcChkExit();
    s_ok = false;
}

HeadsetAction headset_poll(u64 now_ms) {
    if (!s_ok) return HEADSET_NONE;

    u8 regs[4];   // 0x2C..0x2F de una vez
    if (R_FAILED(CDCCHK_ReadRegisters2(CDC_PAGE_0, CDC_REG_FLAGS_DAC, regs, sizeof(regs)))) return HEADSET_NONE;
    s_r2c = regs[0];
    s_r2e = regs[2];
    if (now_ms - s_last_hs_check >= HEADSET_CHECK_MS) {
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
    if ((down && !s_prev_down) || (sticky && !down && !s_prev_down)) {
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
    if (!s_ok) {
        snprintf(out, out_sz, "cdc:CHK no disponible");
        return;
    }
    snprintf(out, out_sz, "2C=%02X 2E=%02X 43=%02X %s  toques:%u ult:%s", s_r2c, s_r2e, s_r43,
             types[CDC_HS_TYPE(s_r43)], s_presses, acts[s_last_action]);
}
