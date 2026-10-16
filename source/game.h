#ifndef GAME_H
#define GAME_H

// Minijuego "Cubo vs. Ruido" para la pantalla superior: el cubo se mueve con el
// analógico, esquiva bloques de ruido (que aparecen más con los graves de la
// música) y junta notas. (X) impulso, (Y) onda expansiva. Al perder vuelve a
// empezar solo. Solo avanza cuando se llama a game_update: si no está visible,
// queda congelado.

#include <3ds.h>
#include <stdbool.h>

// Área de juego en la pantalla superior (debajo de la barra de encabezado)
#define GAME_LEFT    0.0f
#define GAME_RIGHT   400.0f
#define GAME_TOP     24.0f
#define GAME_BOTTOM  240.0f

#define GAME_MAX_HAZARDS   40
#define GAME_MAX_NOTES     3
#define GAME_MAX_PARTICLES 48
#define GAME_LIVES         3
#define GAME_PLAYER_R      11.0f
#define GAME_NOTES_FOR_WAVE 4

typedef enum {
    GAME_READY = 0,  // cuenta antes de empezar o al volver a mostrarlo
    GAME_PLAYING,
    GAME_OVER        // muestra puntos y récord; luego vuelve a READY
} GamePhase;

typedef struct {
    float x, y, vx, vy, r, rot, spin;
    bool alive;
} GameHazard;

typedef struct {
    float x, y, age;
    bool alive;
} GameNote;

typedef struct {
    float x, y, vx, vy, life;
    bool hazard;     // color del ruido o del cubo
} GameParticle;

typedef struct {
    GamePhase phase;
    float phase_t;           // segundos en la fase actual

    float px, py;            // centro del cubo
    float dir_x, dir_y;      // última dirección de movimiento (para el impulso)
    int lives;
    float invuln;            // segundos de invulnerabilidad tras un golpe
    float dash_t;            // > 0 mientras dura el impulso
    float dash_cd;           // enfriamiento del impulso
    int charge;              // notas juntadas para la onda (0..GAME_NOTES_FOR_WAVE)
    float wave_t;            // >= 0 mientras se expande la onda
    float wave_x, wave_y;
    float shake;             // sacudida de pantalla tras un golpe

    float time;              // segundos jugados en esta partida
    float score_f;
    int score;
    int best;
    bool new_best;

    float spawn_acc;
    float note_acc;
    float beat_cd;
    float last_bass;

    GameHazard hazards[GAME_MAX_HAZARDS];
    GameNote notes[GAME_MAX_NOTES];
    GameParticle parts[GAME_MAX_PARTICLES];
} GameState;

// Prepara una partida nueva y carga el récord guardado
void game_init(GameState* g);

// Al volver a mostrar el juego: pausa breve antes de seguir
void game_resume(GameState* g);

// Avanza el juego dt segundos. stick_x/stick_y en -1..1 (y positivo hacia abajo),
// kDown para (X)/(Y) y bass 0..1 con la energía de graves de la música.
void game_update(GameState* g, float dt, float stick_x, float stick_y, u32 kDown, float bass);

#endif // GAME_H
