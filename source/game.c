#include "game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BEST_PATH "sdmc:/3ds/sound3ds/game_best.txt"

#define READY_SECS     1.5f
#define OVER_SECS      3.5f   // tras perder, vuelve a empezar solo
#define PLAYER_SPEED   150.0f
#define DASH_SECS      0.22f
#define DASH_SPEED     420.0f
#define DASH_COOLDOWN  0.9f
#define HIT_INVULN     1.6f
#define WAVE_SECS      0.45f
#define WAVE_RADIUS    130.0f
#define NOTE_LIFE      7.0f

static u32 s_rng = 0x2545F491;

static float frand(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) / (float)0x1000000;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void load_best(GameState* g) {
    g->best = 0;
    FILE* f = fopen(BEST_PATH, "r");
    if (!f) return;
    if (fscanf(f, "%d", &g->best) != 1 || g->best < 0) g->best = 0;
    fclose(f);
}

static void save_best(const GameState* g) {
    FILE* f = fopen(BEST_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", g->best);
    fclose(f);
}

// Tablero limpio para una partida nueva (conserva el récord)
static void new_round(GameState* g) {
    int best = g->best;
    memset(g, 0, sizeof(*g));
    g->best = best;
    g->phase = GAME_READY;
    g->px = (GAME_LEFT + GAME_RIGHT) / 2;
    g->py = (GAME_TOP + GAME_BOTTOM) / 2 + 10;
    g->dir_x = 1.0f;
    g->lives = GAME_LIVES;
    g->wave_t = -1.0f;
    g->spawn_acc = -1.0f;   // un respiro antes del primer bloque
    g->note_acc = 1.0f;
}

void game_init(GameState* g) {
    s_rng ^= (u32)osGetTime();
    memset(g, 0, sizeof(*g));
    load_best(g);
    new_round(g);
}

void game_resume(GameState* g) {
    // Si estaba jugando, una cuenta corta para retomar sin que te peguen de golpe
    if (g->phase == GAME_PLAYING) {
        g->phase = GAME_READY;
        g->phase_t = 0.0f;
    }
}

static void burst(GameState* g, float x, float y, int n, bool hazard) {
    for (int i = 0; i < GAME_MAX_PARTICLES && n > 0; i++) {
        GameParticle* p = &g->parts[i];
        if (p->life > 0) continue;
        float a = frand() * 6.2832f, sp = 40 + frand() * 110;
        p->x = x;
        p->y = y;
        p->vx = cosf(a) * sp;
        p->vy = sinf(a) * sp;
        p->life = 0.4f + frand() * 0.4f;
        p->hazard = hazard;
        n--;
    }
}

// Un bloque de ruido entra por un borde apuntando cerca del cubo
static void spawn_hazard(GameState* g) {
    for (int i = 0; i < GAME_MAX_HAZARDS; i++) {
        GameHazard* h = &g->hazards[i];
        if (h->alive) continue;
        h->r = 6 + frand() * 6;
        float m = h->r + 4;
        switch ((int)(frand() * 4)) {
            case 0:  h->x = GAME_LEFT - m;  h->y = GAME_TOP + frand() * (GAME_BOTTOM - GAME_TOP); break;
            case 1:  h->x = GAME_RIGHT + m; h->y = GAME_TOP + frand() * (GAME_BOTTOM - GAME_TOP); break;
            case 2:  h->x = GAME_LEFT + frand() * (GAME_RIGHT - GAME_LEFT); h->y = GAME_TOP - m; break;
            default: h->x = GAME_LEFT + frand() * (GAME_RIGHT - GAME_LEFT); h->y = GAME_BOTTOM + m; break;
        }
        float tx = g->px + (frand() - 0.5f) * 140, ty = g->py + (frand() - 0.5f) * 100;
        float dx = tx - h->x, dy = ty - h->y, len = sqrtf(dx * dx + dy * dy);
        if (len < 1) len = 1;
        float speed = 55 + fminf(g->time, 120.0f) * 0.9f + frand() * 30;
        h->vx = dx / len * speed;
        h->vy = dy / len * speed;
        h->rot = frand() * 6.2832f;
        h->spin = (frand() - 0.5f) * 6;
        h->alive = true;
        return;
    }
}

// Una nota aparece lejos del cubo
static void spawn_note(GameState* g) {
    for (int i = 0; i < GAME_MAX_NOTES; i++) {
        GameNote* n = &g->notes[i];
        if (n->alive) continue;
        for (int tries = 0; tries < 8; tries++) {
            n->x = GAME_LEFT + 24 + frand() * (GAME_RIGHT - GAME_LEFT - 48);
            n->y = GAME_TOP + 30 + frand() * (GAME_BOTTOM - GAME_TOP - 50);
            float dx = n->x - g->px, dy = n->y - g->py;
            if (dx * dx + dy * dy > 90 * 90) break;
        }
        n->age = 0;
        n->alive = true;
        return;
    }
}

static void update_particles(GameState* g, float dt) {
    for (int i = 0; i < GAME_MAX_PARTICLES; i++) {
        GameParticle* p = &g->parts[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= 0.92f;
        p->vy *= 0.92f;
    }
}

static void lose_life(GameState* g) {
    g->lives--;
    g->invuln = HIT_INVULN;
    g->shake = 0.35f;
    burst(g, g->px, g->py, 14, false);
    if (g->lives <= 0) {
        g->phase = GAME_OVER;
        g->phase_t = 0.0f;
        g->new_best = g->score > g->best;
        if (g->new_best) {
            g->best = g->score;
            save_best(g);
        }
    }
}

void game_update(GameState* g, float dt, float stick_x, float stick_y, u32 kDown, float bass) {
    if (dt > 0.05f) dt = 0.05f;
    g->phase_t += dt;
    if (g->shake > 0) g->shake -= dt;
    update_particles(g, dt);

    if (g->phase == GAME_OVER) {
        if (g->phase_t > OVER_SECS || (g->phase_t > 0.8f && (kDown & KEY_X))) new_round(g);
        return;
    }
    if (g->phase == GAME_READY) {
        if (g->phase_t >= READY_SECS) {
            g->phase = GAME_PLAYING;
            g->phase_t = 0.0f;
        }
        return;
    }

    g->time += dt;
    g->score_f += dt * 10.0f;
    g->score = (int)g->score_f;
    if (g->invuln > 0) g->invuln -= dt;
    if (g->dash_cd > 0) g->dash_cd -= dt;

    // Movimiento con el analógico (zona muerta pequeña)
    float mag = sqrtf(stick_x * stick_x + stick_y * stick_y);
    if (mag < 0.15f) {
        stick_x = stick_y = 0;
    } else {
        g->dir_x = stick_x / mag;
        g->dir_y = stick_y / mag;
        if (mag > 1.0f) {
            stick_x /= mag;
            stick_y /= mag;
        }
    }

    // (X) impulso en la dirección del analógico: rápido e invulnerable
    if ((kDown & KEY_X) && g->dash_cd <= 0) {
        g->dash_t = DASH_SECS;
        g->dash_cd = DASH_COOLDOWN;
    }
    float vx, vy;
    if (g->dash_t > 0) {
        g->dash_t -= dt;
        vx = g->dir_x * DASH_SPEED;
        vy = g->dir_y * DASH_SPEED;
    } else {
        vx = stick_x * PLAYER_SPEED;
        vy = stick_y * PLAYER_SPEED;
    }
    g->px = clampf(g->px + vx * dt, GAME_LEFT + GAME_PLAYER_R, GAME_RIGHT - GAME_PLAYER_R);
    g->py = clampf(g->py + vy * dt, GAME_TOP + GAME_PLAYER_R + 4, GAME_BOTTOM - GAME_PLAYER_R);

    // (Y) onda expansiva cuando la carga está completa
    if ((kDown & KEY_Y) && g->charge >= GAME_NOTES_FOR_WAVE && g->wave_t < 0) {
        g->charge = 0;
        g->wave_t = 0.0f;
        g->wave_x = g->px;
        g->wave_y = g->py;
    }
    float wave_r = -1.0f;
    if (g->wave_t >= 0) {
        g->wave_t += dt;
        wave_r = WAVE_RADIUS * (g->wave_t / WAVE_SECS);
        if (g->wave_t > WAVE_SECS) g->wave_t = -1.0f;
    }

    // Aparición de ruido: cada vez más seguido, y con un extra en cada golpe de graves
    float interval = fmaxf(0.28f, 1.0f - g->time * 0.008f);
    g->spawn_acc += dt;
    while (g->spawn_acc >= interval) {
        g->spawn_acc -= interval;
        spawn_hazard(g);
    }
    if (g->beat_cd > 0) g->beat_cd -= dt;
    if (bass > 0.12f && bass > g->last_bass * 1.35f && g->beat_cd <= 0) {
        spawn_hazard(g);
        g->beat_cd = 0.35f;
    }
    g->last_bass = g->last_bass * 0.95f + bass * 0.05f;

    // Notas
    g->note_acc += dt;
    if (g->note_acc > 1.6f) {
        g->note_acc = 0;
        spawn_note(g);
    }
    for (int i = 0; i < GAME_MAX_NOTES; i++) {
        GameNote* n = &g->notes[i];
        if (!n->alive) continue;
        n->age += dt;
        if (n->age > NOTE_LIFE) {
            n->alive = false;
            continue;
        }
        float dx = n->x - g->px, dy = n->y - g->py;
        if (dx * dx + dy * dy < (GAME_PLAYER_R + 9) * (GAME_PLAYER_R + 9)) {
            n->alive = false;
            g->score_f += 50;
            if (g->charge < GAME_NOTES_FOR_WAVE) g->charge++;
        }
    }

    // Ruido: avanzar, chocar con la onda o con el cubo
    for (int i = 0; i < GAME_MAX_HAZARDS; i++) {
        GameHazard* h = &g->hazards[i];
        if (!h->alive) continue;
        h->x += h->vx * dt;
        h->y += h->vy * dt;
        h->rot += h->spin * dt;
        float m = h->r + 30;
        if (h->x < GAME_LEFT - m || h->x > GAME_RIGHT + m || h->y < GAME_TOP - m || h->y > GAME_BOTTOM + m) {
            h->alive = false;
            continue;
        }
        if (wave_r > 0) {
            float wx = h->x - g->wave_x, wy = h->y - g->wave_y;
            if (wx * wx + wy * wy < wave_r * wave_r) {
                h->alive = false;
                g->score_f += 15;
                burst(g, h->x, h->y, 5, true);
                continue;
            }
        }
        float dx = h->x - g->px, dy = h->y - g->py, rr = h->r * 0.85f + GAME_PLAYER_R;
        if (dx * dx + dy * dy < rr * rr && g->invuln <= 0 && g->dash_t <= 0) {
            h->alive = false;
            burst(g, h->x, h->y, 6, true);
            lose_life(g);
            if (g->phase == GAME_OVER) return;
        }
    }
}
