#ifndef ACCOUNT_H
#define ACCOUNT_H

// Cuenta de SoundCloud conectada con Sound3DS Share + Sound3DS QR.
//
// Sound3DS QR guarda en sdmc:/3ds/sound3ds/account.json la sesión web: un token
// de acceso que dura 24 h y un token de renovación. Este módulo la carga, la
// renueva sola (al iniciar si tiene más de 20 h, cada cierto tiempo mientras la
// app sigue abierta y tras un 401) y guarda el token nuevo sin dejar el archivo
// a medias. Si SoundCloud rechaza la renovación se borra la sesión y se pide
// volver a conectar; si solo falla la red se reintenta más tarde.
//
// Los tokens nunca se escriben en el registro.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define ACCOUNT_PATH "sdmc:/3ds/sound3ds/account.json"

// Carga la cuenta guardada (llamar después de network_init)
void account_init(void);
void account_exit(void);

// Llamar en cada cuadro: lanza la renovación en segundo plano cuando toca
void account_poll(void);

bool account_connected(void);
// Nombre de usuario (vacío si no hay cuenta)
void account_username(char* out, size_t out_sz);

// Id del usuario de SoundCloud (0 si no hay cuenta)
long long account_user_id(void);

// Copia el token de acceso actual y su generación (cambia con cada renovación).
// false si no hay cuenta.
bool account_token(char* out, size_t out_sz, u32* gen);

// Tras un 401 con el token de la generación gen: renueva la sesión en este hilo
// (si otro hilo no lo hizo ya). true si hay un token nuevo para reintentar una
// vez. Llamar solo desde hilos de red.
bool account_on_unauthorized(u32 gen);

// true si SoundCloud rechazó la sesión en esta ejecución (hay que volver a conectar)
bool account_expired(void);

// Borra la sesión de la tarjeta SD y de la memoria
void account_disconnect(void);

// Aviso pendiente para el usuario (por ejemplo, la sesión venció); false si no hay
bool account_take_message(char* out, size_t out_sz);

#endif // ACCOUNT_H
