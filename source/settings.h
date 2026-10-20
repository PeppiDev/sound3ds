#ifndef SETTINGS_H
#define SETTINGS_H

// Configuración guardada en sdmc:/3ds/sound3ds/settings.ini, y el LED de
// notificación (el de StreetPass) en arcoíris mientras suena música.

#include <stdbool.h>
#include "i18n.h"

// Repetir: nada (se detiene al final de la lista), la lista entera o la canción
typedef enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE } RepeatMode;

// L/R con la tapa cerrada: toque (como siempre), mantener ~1 s (a prueba de
// bolsillo) o desactivados
typedef enum { LID_KEYS_TAP = 0, LID_KEYS_HOLD, LID_KEYS_OFF } LidKeys;

typedef struct {
    Lang lang;
    bool led_rgb;
    RepeatMode repeat;
    bool shuffle;
    LidKeys lid_keys;
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);

// Enciende/apaga el arcoíris del LED. Solo habla con el MCU cuando cambia el estado.
// Funciona en toda la familia (3DS, 3DS XL, 2DS, New 3DS, New 3DS XL, New 2DS XL):
// todas tienen el LED RGB de notificación manejado por el MCU (mcu::HWC).
void led_set_rainbow(bool on);

// Apaga el LED y cierra el servicio (al salir)
void led_exit(void);

#endif // SETTINGS_H
