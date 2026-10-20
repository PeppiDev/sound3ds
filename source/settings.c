#include "settings.h"
#include "debuglog.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_PATH "sdmc:/3ds/sound3ds/settings.ini"

Settings g_settings = { LANG_EN, false, REPEAT_OFF, false, LID_KEYS_TAP };   // inglés por defecto (sin settings.ini)

void settings_load(void) {
    FILE* f = fopen(SETTINGS_PATH, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            int v;
            if (sscanf(line, "lang=%d", &v) == 1) g_settings.lang = v == 1 ? LANG_EN : LANG_ES;
            if (sscanf(line, "led_rgb=%d", &v) == 1) g_settings.led_rgb = v != 0;
            if (sscanf(line, "repeat=%d", &v) == 1 && v >= REPEAT_OFF && v <= REPEAT_ONE) g_settings.repeat = v;
            if (sscanf(line, "shuffle=%d", &v) == 1) g_settings.shuffle = v != 0;
            if (sscanf(line, "lid_keys=%d", &v) == 1 && v >= LID_KEYS_TAP && v <= LID_KEYS_OFF) g_settings.lid_keys = v;
        }
        fclose(f);
    }
    i18n_set(g_settings.lang);
}

void settings_save(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "lang=%d\nled_rgb=%d\nrepeat=%d\nshuffle=%d\nlid_keys=%d\n", (int)g_settings.lang,
            g_settings.led_rgb ? 1 : 0, (int)g_settings.repeat, g_settings.shuffle ? 1 : 0, (int)g_settings.lid_keys);
    fclose(f);
}

// ---------------------------------------------------------------------------
// LED de notificación
// ---------------------------------------------------------------------------

static bool s_led_init = false, s_led_ok = false, s_led_on = false;

static void led_apply(bool on) {
    if (!s_led_init) {
        s_led_ok = R_SUCCEEDED(mcuHwcInit());
        s_led_init = true;
        dlog("led: mcu::HWC %s", s_led_ok ? "ok" : "no disponible");
    }
    if (!s_led_ok) return;
    InfoLedPattern p;
    memset(&p, 0, sizeof(p));
    if (on) {
        // 32 pasos de un arcoíris (tono 0..360) con transición suave y bucle continuo
        p.delay = 0x04;        // 1/4 s por paso: una vuelta cada ~8 s
        p.smoothing = 0x40;
        p.loopDelay = 0x00;
        for (int i = 0; i < 32; i++) {
            float h = i / 32.0f * 6.0f;
            float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
            float r = 0, g = 0, b = 0;
            switch ((int)h) {
                case 0: r = 1; g = x; break;
                case 1: r = x; g = 1; break;
                case 2: g = 1; b = x; break;
                case 3: g = x; b = 1; break;
                case 4: r = x; b = 1; break;
                default: r = 1; b = x; break;
            }
            p.redPattern[i] = (u8)(r * 255);
            p.greenPattern[i] = (u8)(g * 255);
            p.bluePattern[i] = (u8)(b * 255);
        }
    } else {
        p.loopDelay = 0xFF;    // patrón vacío: LED apagado
    }
    Result res = MCUHWC_SetInfoLedPattern(&p);
    dlog("led: arcoiris %s (0x%08lX)", on ? "on" : "off", (unsigned long)res);
}

void led_set_rainbow(bool on) {
    if (on == s_led_on) return;
    s_led_on = on;
    led_apply(on);
}

void led_exit(void) {
    if (s_led_on) led_apply(false);
    s_led_on = false;
    if (s_led_ok) mcuHwcExit();
    s_led_init = s_led_ok = false;
}
