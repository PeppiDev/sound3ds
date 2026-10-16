#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "audio.h"
#include <3ds/services/csnd.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>

// Anillo PCM del backend CSND: AUDIO_NUM_BUFFERS segmentos por canal.
#define CSND_RING_SAMPLES   (AUDIO_BUF_SAMPLES * AUDIO_NUM_BUFFERS)
// Margen detrás del cabezal de reproducción estimado para no pisar audio que aún suena.
#define CSND_GUARD_SAMPLES  512

#define AUDIO_THREAD_STACK_SZ (32 * 1024)

// Estructura interna del motor de audio
static struct {
    bool initialized;
    AudioBackend backend;
    volatile bool is_playing;
    volatile bool thread_exit;
    float volume;

    // Ring buffer para bytes MP3 comprimidos
    uint8_t ring_buf[AUDIO_RING_SIZE];
    size_t ring_head; // posicion de escritura
    size_t ring_tail; // posicion de lectura
    volatile size_t ring_used;
    size_t pending_skip; // bytes de etiqueta ID3 que faltan por descartar
    LightLock ring_lock;
    LightLock decode_lock;

    // Muestras restantes (remainder) para evitar descartar PCM al llenar bloques
    int16_t remainder_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    size_t remainder_count;  // Muestras estéreo disponibles
    size_t remainder_offset; // Muestras estéreo ya consumidas

    // Posicion: muestras reales decodificadas (sin relleno de silencio)
    u64 decoded_real;
    volatile bool starved; // el ultimo bloque fue solo silencio por falta de datos

    Thread worker_thread;
    mp3dec_t mp3d;

    // Backend NDSP: cola de wavebufs estéreo intercalados
    struct {
        bool initialized;
        ndspWaveBuf wbuf[AUDIO_NUM_BUFFERS];
        int16_t* pcm[AUDIO_NUM_BUFFERS];
    } ndsp;

    // Backend CSND: anillo mono por canal reproducido en bucle
    struct {
        bool initialized;
        int left_channel;
        int right_channel;
        int16_t* left;
        int16_t* right;
        int16_t* stereo_tmp;   // bloque decodificado antes de separar canales
        int16_t* rotate_tmp;   // copia temporal al pausar
        bool running;
        u64 start_tick;
        u64 written;           // muestras escritas desde el ultimo arranque
        u32 timer;
    } csnd;

    // Snapshot para el visualizador FFT
    int16_t recent_pcm[AUDIO_BUF_SAMPLES * 2];
    LightLock pcm_lock;
} s_audio;

static char s_audio_init_error[64];

// ---------------------------------------------------------------------------
// Decodificacion MP3 -> PCM estéreo intercalado
// ---------------------------------------------------------------------------

static void ring_peek(uint8_t* dst, size_t len) {
    size_t first = AUDIO_RING_SIZE - s_audio.ring_tail;
    if (first > len) first = len;
    memcpy(dst, &s_audio.ring_buf[s_audio.ring_tail], first);
    if (len > first) memcpy(dst + first, s_audio.ring_buf, len - first);
}

static void ring_consume(size_t len) {
    if (len > s_audio.ring_used) len = s_audio.ring_used;
    s_audio.ring_tail = (s_audio.ring_tail + len) % AUDIO_RING_SIZE;
    s_audio.ring_used -= len;
}

// Llena dst con n muestras estéreo. Si no hay datos MP3 suficientes completa con
// silencio para que la salida nunca se detenga. Devuelve las muestras reales.
static size_t decode_block(int16_t* dst, size_t n) {
    size_t filled = 0;
    size_t real = 0;

    if (s_audio.remainder_count > 0) {
        size_t to_take = s_audio.remainder_count - s_audio.remainder_offset;
        if (to_take > n) to_take = n;
        memcpy(dst, s_audio.remainder_pcm + s_audio.remainder_offset * 2,
               to_take * 2 * sizeof(int16_t));
        filled += to_take;
        s_audio.remainder_offset += to_take;
        if (s_audio.remainder_offset >= s_audio.remainder_count) {
            s_audio.remainder_count = 0;
            s_audio.remainder_offset = 0;
        }
    }

    int16_t temp_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint8_t decode_in[1500];

    while (filled < n) {
        LightLock_Lock(&s_audio.ring_lock);

        // Descartar etiquetas ID3 de una vez en lugar de byte a byte
        if (s_audio.pending_skip > 0) {
            size_t skip = s_audio.pending_skip < s_audio.ring_used ? s_audio.pending_skip : s_audio.ring_used;
            ring_consume(skip);
            s_audio.pending_skip -= skip;
        }

        size_t avail = s_audio.ring_used;
        if (avail < 512 || s_audio.pending_skip > 0) {
            LightLock_Unlock(&s_audio.ring_lock);
            break;
        }

        size_t to_read = avail > sizeof(decode_in) ? sizeof(decode_in) : avail;
        ring_peek(decode_in, to_read);

        if (decode_in[0] == 'I' && decode_in[1] == 'D' && decode_in[2] == '3' && to_read >= 10) {
            size_t tag = ((size_t)(decode_in[6] & 0x7F) << 21) | ((size_t)(decode_in[7] & 0x7F) << 14) |
                         ((size_t)(decode_in[8] & 0x7F) << 7) | (size_t)(decode_in[9] & 0x7F);
            tag += 10;
            if (decode_in[5] & 0x10) tag += 10; // footer
            s_audio.pending_skip = tag;
            LightLock_Unlock(&s_audio.ring_lock);
            continue;
        }
        LightLock_Unlock(&s_audio.ring_lock);

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_audio.mp3d, decode_in, (int)to_read, temp_pcm, &info);

        LightLock_Lock(&s_audio.ring_lock);
        ring_consume(info.frame_bytes > 0 ? (size_t)info.frame_bytes : 1);
        LightLock_Unlock(&s_audio.ring_lock);

        if (info.frame_bytes <= 0 || samples <= 0) continue;

        int16_t frame_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
        if (info.channels == 2) {
            memcpy(frame_stereo, temp_pcm, (size_t)samples * 2 * sizeof(int16_t));
        } else {
            for (int s = 0; s < samples; s++) {
                frame_stereo[s * 2] = temp_pcm[s];
                frame_stereo[s * 2 + 1] = temp_pcm[s];
            }
        }

        size_t needed = n - filled;
        if ((size_t)samples <= needed) {
            memcpy(dst + filled * 2, frame_stereo, (size_t)samples * 2 * sizeof(int16_t));
            filled += samples;
        } else {
            memcpy(dst + filled * 2, frame_stereo, needed * 2 * sizeof(int16_t));
            filled += needed;
            size_t excess = (size_t)samples - needed;
            memcpy(s_audio.remainder_pcm, frame_stereo + needed * 2, excess * 2 * sizeof(int16_t));
            s_audio.remainder_count = excess;
            s_audio.remainder_offset = 0;
        }
    }

    real = filled;
    if (filled < n) {
        memset(dst + filled * 2, 0, (n - filled) * 2 * sizeof(int16_t));
    }

    s_audio.decoded_real += real;
    s_audio.starved = (real == 0);

    size_t snap = n < AUDIO_BUF_SAMPLES ? n : AUDIO_BUF_SAMPLES;
    LightLock_Lock(&s_audio.pcm_lock);
    memcpy(s_audio.recent_pcm, dst, snap * 2 * sizeof(int16_t));
    LightLock_Unlock(&s_audio.pcm_lock);

    return real;
}

// ---------------------------------------------------------------------------
// Backend NDSP: cola de wavebufs, sin huecos entre bloques
// ---------------------------------------------------------------------------

static void ndsp_apply_volume(void) {
    float mix[12] = {0};
    mix[0] = s_audio.volume;
    mix[1] = s_audio.volume;
    ndspChnSetMix(0, mix);
}

static bool ndsp_backend_init(void) {
    Result res = ndspInit();
    if (R_FAILED(res)) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "ndsp 0x%08lX", (unsigned long)res);
        return false;
    }
    s_audio.ndsp.initialized = true;

    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        s_audio.ndsp.pcm[i] = (int16_t*)linearAlloc(AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));
        if (!s_audio.ndsp.pcm[i]) {
            snprintf(s_audio_init_error, sizeof(s_audio_init_error), "ndsp linearAlloc");
            return false;
        }
        memset(&s_audio.ndsp.wbuf[i], 0, sizeof(ndspWaveBuf));
        s_audio.ndsp.wbuf[i].data_vaddr = s_audio.ndsp.pcm[i];
        s_audio.ndsp.wbuf[i].nsamples = AUDIO_BUF_SAMPLES;
        s_audio.ndsp.wbuf[i].status = NDSP_WBUF_FREE;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndsp_apply_volume();
    return true;
}

static void ndsp_backend_exit(void) {
    if (s_audio.ndsp.initialized) {
        ndspChnWaveBufClear(0);
        ndspExit();
        s_audio.ndsp.initialized = false;
    }
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        if (s_audio.ndsp.pcm[i]) {
            linearFree(s_audio.ndsp.pcm[i]);
            s_audio.ndsp.pcm[i] = NULL;
        }
    }
}

// Rellena todos los wavebufs libres. Devuelve true si encoló alguno.
static bool ndsp_service(void) {
    bool queued = false;
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        ndspWaveBuf* wb = &s_audio.ndsp.wbuf[i];
        if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) continue;

        decode_block(s_audio.ndsp.pcm[i], AUDIO_BUF_SAMPLES);
        DSP_FlushDataCache(s_audio.ndsp.pcm[i], AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));
        ndspChnWaveBufAdd(0, wb);
        queued = true;
    }
    return queued;
}

static size_t ndsp_buffered_samples(void) {
    size_t total = 0;
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        u8 st = s_audio.ndsp.wbuf[i].status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) total += AUDIO_BUF_SAMPLES;
    }
    return total;
}

static void ndsp_reset(void) {
    ndspChnWaveBufClear(0);
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        s_audio.ndsp.wbuf[i].status = NDSP_WBUF_FREE;
        memset(s_audio.ndsp.pcm[i], 0, AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));
    }
}

// ---------------------------------------------------------------------------
// Backend CSND: anillo PCM en bucle, posicion estimada con el reloj del sistema.
// CSND no expone la posicion de reproduccion, pero el timer del canal deriva del
// mismo reloj: una muestra dura exactamente 4 * timer ticks de SYSCLOCK_ARM11.
// ---------------------------------------------------------------------------

static bool csnd_backend_init(void) {
    Result res = csndInit();
    if (R_FAILED(res)) {
        size_t len = strlen(s_audio_init_error);
        snprintf(s_audio_init_error + len, sizeof(s_audio_init_error) - len,
                 "%scsnd 0x%08lX", len ? " / " : "", (unsigned long)res);
        return false;
    }
    s_audio.csnd.initialized = true;

    int found_channels = 0;
    for (int channel = 0; channel < CSND_NUM_CHANNELS && found_channels < 2; channel++) {
        if (csndChannels & (1u << channel)) {
            if (found_channels == 0) s_audio.csnd.left_channel = channel;
            else s_audio.csnd.right_channel = channel;
            found_channels++;
        }
    }
    if (found_channels < 2) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "CSND needs 2 channels");
        return false;
    }

    size_t mono_bytes = CSND_RING_SAMPLES * sizeof(int16_t);
    s_audio.csnd.left = (int16_t*)linearAlloc(mono_bytes);
    s_audio.csnd.right = (int16_t*)linearAlloc(mono_bytes);
    s_audio.csnd.stereo_tmp = (int16_t*)malloc(AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));
    s_audio.csnd.rotate_tmp = (int16_t*)malloc(mono_bytes);
    if (!s_audio.csnd.left || !s_audio.csnd.right || !s_audio.csnd.stereo_tmp || !s_audio.csnd.rotate_tmp) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "csnd alloc");
        return false;
    }
    memset(s_audio.csnd.left, 0, mono_bytes);
    memset(s_audio.csnd.right, 0, mono_bytes);

    s_audio.csnd.timer = CSND_TIMER(AUDIO_SAMPLE_RATE);
    return true;
}

static void csnd_stop_channels(void) {
    CSND_SetPlayStateR((u32)s_audio.csnd.left_channel, 0);
    CSND_SetPlayStateR((u32)s_audio.csnd.right_channel, 0);
    csndExecCmds(true);
}

static void csnd_backend_exit(void) {
    if (s_audio.csnd.initialized) {
        csnd_stop_channels();
        csndExit();
        s_audio.csnd.initialized = false;
    }
    if (s_audio.csnd.left) { linearFree(s_audio.csnd.left); s_audio.csnd.left = NULL; }
    if (s_audio.csnd.right) { linearFree(s_audio.csnd.right); s_audio.csnd.right = NULL; }
    free(s_audio.csnd.stereo_tmp);
    s_audio.csnd.stereo_tmp = NULL;
    free(s_audio.csnd.rotate_tmp);
    s_audio.csnd.rotate_tmp = NULL;
}

static u64 csnd_played_samples(void) {
    if (!s_audio.csnd.running) return 0;
    u64 elapsed = svcGetSystemTick() - s_audio.csnd.start_tick;
    return elapsed / (4ULL * s_audio.csnd.timer);
}

// Decodifica y escribe en el anillo hasta la posicion absoluta `target`.
static void csnd_fill_until(u64 target) {
    while (s_audio.csnd.written < target) {
        size_t idx = (size_t)(s_audio.csnd.written % CSND_RING_SAMPLES);
        size_t chunk = (size_t)(target - s_audio.csnd.written);
        if (chunk > AUDIO_BUF_SAMPLES) chunk = AUDIO_BUF_SAMPLES;
        if (chunk > CSND_RING_SAMPLES - idx) chunk = CSND_RING_SAMPLES - idx;

        int16_t* st = s_audio.csnd.stereo_tmp;
        decode_block(st, chunk);
        for (size_t i = 0; i < chunk; i++) {
            s_audio.csnd.left[idx + i] = st[i * 2];
            s_audio.csnd.right[idx + i] = st[i * 2 + 1];
        }
        CSND_FlushDataCache(s_audio.csnd.left + idx, (u32)(chunk * sizeof(int16_t)));
        CSND_FlushDataCache(s_audio.csnd.right + idx, (u32)(chunk * sizeof(int16_t)));
        s_audio.csnd.written += chunk;
    }
}

static void csnd_start(void) {
    u32 bytes = CSND_RING_SAMPLES * sizeof(int16_t);
    u32 flags = SOUND_FORMAT_16BIT | SOUND_REPEAT | SOUND_ENABLE | (s_audio.csnd.timer << 16);
    u32 pl = osConvertVirtToPhys(s_audio.csnd.left);
    u32 pr = osConvertVirtToPhys(s_audio.csnd.right);
    u32 vl = CSND_VOL(s_audio.volume, -1.0f);
    u32 vr = CSND_VOL(s_audio.volume, 1.0f);

    // Ambos canales en el mismo lote de comandos para que arranquen sincronizados
    CSND_SetChnRegs(flags | SOUND_CHANNEL(s_audio.csnd.left_channel), pl, pl, bytes, vl, vl);
    CSND_SetChnRegs(flags | SOUND_CHANNEL(s_audio.csnd.right_channel), pr, pr, bytes, vr, vr);
    csndExecCmds(true);

    s_audio.csnd.start_tick = svcGetSystemTick();
    s_audio.csnd.running = true;
}

// Detiene los canales conservando el audio aun no reproducido al inicio del anillo,
// para que al reanudar no se pierda ni se repita audio.
static void csnd_pause(void) {
    if (!s_audio.csnd.running) return;
    u64 played = csnd_played_samples();
    csnd_stop_channels();
    s_audio.csnd.running = false;

    if (played > s_audio.csnd.written) played = s_audio.csnd.written;
    size_t unplayed = (size_t)(s_audio.csnd.written - played);
    size_t from = (size_t)(played % CSND_RING_SAMPLES);
    int16_t* bufs[2] = { s_audio.csnd.left, s_audio.csnd.right };
    for (int c = 0; c < 2; c++) {
        size_t first = CSND_RING_SAMPLES - from;
        if (first > unplayed) first = unplayed;
        memcpy(s_audio.csnd.rotate_tmp, bufs[c] + from, first * sizeof(int16_t));
        memcpy(s_audio.csnd.rotate_tmp + first, bufs[c], (unplayed - first) * sizeof(int16_t));
        memcpy(bufs[c], s_audio.csnd.rotate_tmp, unplayed * sizeof(int16_t));
    }
    s_audio.csnd.written = unplayed;
}

static bool csnd_service(void) {
    if (!s_audio.csnd.running) {
        csnd_fill_until(CSND_RING_SAMPLES);
        csnd_start();
        return true;
    }

    u64 played = csnd_played_samples();
    if (played >= s_audio.csnd.written) {
        // El decodificador quedo atras (CPU saturada): reiniciar el anillo limpio
        csnd_stop_channels();
        s_audio.csnd.running = false;
        s_audio.csnd.written = 0;
        return true;
    }

    u64 target = played + CSND_RING_SAMPLES - CSND_GUARD_SAMPLES;
    if (target < s_audio.csnd.written + AUDIO_BUF_SAMPLES) return false;
    csnd_fill_until(target);
    return true;
}

static size_t csnd_buffered_samples(void) {
    u64 played = csnd_played_samples();
    return played >= s_audio.csnd.written ? 0 : (size_t)(s_audio.csnd.written - played);
}

static void csnd_reset(void) {
    if (s_audio.csnd.running) csnd_stop_channels();
    s_audio.csnd.running = false;
    s_audio.csnd.written = 0;
    memset(s_audio.csnd.left, 0, CSND_RING_SAMPLES * sizeof(int16_t));
    memset(s_audio.csnd.right, 0, CSND_RING_SAMPLES * sizeof(int16_t));
}

// ---------------------------------------------------------------------------
// Hilo worker y API publica
// ---------------------------------------------------------------------------

static void audio_worker_func(void* arg) {
    while (!s_audio.thread_exit) {
        if (!s_audio.is_playing) {
            svcSleepThread(10 * 1000 * 1000LL);
            continue;
        }

        LightLock_Lock(&s_audio.decode_lock);
        bool did_work = false;
        if (s_audio.is_playing && !s_audio.thread_exit) {
            did_work = (s_audio.backend == AUDIO_BACKEND_NDSP) ? ndsp_service() : csnd_service();
        }
        LightLock_Unlock(&s_audio.decode_lock);

        if (!did_work) svcSleepThread(4 * 1000 * 1000LL);
    }
}

bool audio_init(void) {
    if (s_audio.initialized) return true;

    memset(&s_audio, 0, sizeof(s_audio));
    s_audio_init_error[0] = '\0';
    s_audio.volume = 0.85f;
    LightLock_Init(&s_audio.ring_lock);
    LightLock_Init(&s_audio.decode_lock);
    LightLock_Init(&s_audio.pcm_lock);

    APT_SetAppCpuTimeLimit(30);

    // NDSP es el backend preferido (cola sin huecos, pausa real) pero necesita
    // sdmc:/3ds/dspfirm.cdc. Sin ese archivo se usa CSND como respaldo.
    if (ndsp_backend_init()) {
        s_audio.backend = AUDIO_BACKEND_NDSP;
    } else {
        ndsp_backend_exit();
        if (csnd_backend_init()) {
            s_audio.backend = AUDIO_BACKEND_CSND;
        } else {
            csnd_backend_exit();
            return false;
        }
    }

    mp3dec_init(&s_audio.mp3d);

    // Hilo de audio con prioridad ligeramente superior al principal y fallback de afinidad
    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    int target_prio = main_prio - 1;
    if (target_prio < 0x18) target_prio = 0x18;
    if (target_prio > 0x3F) target_prio = 0x3F;

    s_audio.thread_exit = false;
    s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, target_prio, -2, false);
    if (!s_audio.worker_thread)
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, -2, false);
    if (!s_audio.worker_thread)
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, 0, false);
    if (!s_audio.worker_thread)
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, 1, false);

    if (!s_audio.worker_thread) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "threadCreate");
        audio_exit();
        return false;
    }

    s_audio.initialized = true;
    return true;
}

void audio_exit(void) {
    s_audio.thread_exit = true;
    s_audio.is_playing = false;

    if (s_audio.worker_thread) {
        threadJoin(s_audio.worker_thread, U64_MAX);
        threadFree(s_audio.worker_thread);
        s_audio.worker_thread = NULL;
    }

    ndsp_backend_exit();
    csnd_backend_exit();
    s_audio.backend = AUDIO_BACKEND_NONE;
    s_audio.initialized = false;
}

void audio_play(void) {
    if (!s_audio.initialized) return;
    LightLock_Lock(&s_audio.decode_lock);
    if (s_audio.backend == AUDIO_BACKEND_NDSP) ndspChnSetPaused(0, false);
    // CSND: el worker vuelve a arrancar el anillo con el audio conservado en la pausa
    s_audio.is_playing = true;
    LightLock_Unlock(&s_audio.decode_lock);
}

void audio_pause(void) {
    if (!s_audio.initialized) return;
    LightLock_Lock(&s_audio.decode_lock);
    s_audio.is_playing = false;
    if (s_audio.backend == AUDIO_BACKEND_NDSP) ndspChnSetPaused(0, true);
    else csnd_pause();
    LightLock_Unlock(&s_audio.decode_lock);
}

void audio_stop(void) {
    audio_pause();
    audio_clear_buffers();
}

bool audio_is_playing(void) {
    return s_audio.is_playing;
}

void audio_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_audio.volume = vol;

    if (!s_audio.initialized) return;
    LightLock_Lock(&s_audio.decode_lock);
    if (s_audio.backend == AUDIO_BACKEND_NDSP) {
        ndsp_apply_volume();
    } else if (s_audio.csnd.running) {
        CSND_SetVol((u32)s_audio.csnd.left_channel, CSND_VOL(vol, -1.0f), CSND_VOL(vol, -1.0f));
        CSND_SetVol((u32)s_audio.csnd.right_channel, CSND_VOL(vol, 1.0f), CSND_VOL(vol, 1.0f));
        csndExecCmds(false);
    }
    LightLock_Unlock(&s_audio.decode_lock);
}

float audio_get_volume(void) {
    return s_audio.volume;
}

size_t audio_feed_mp3_data(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;

    LightLock_Lock(&s_audio.ring_lock);
    size_t space_free = AUDIO_RING_SIZE - s_audio.ring_used;
    size_t to_write = (len < space_free) ? len : space_free;

    if (to_write > 0) {
        size_t first_chunk = AUDIO_RING_SIZE - s_audio.ring_head;
        if (first_chunk > to_write) first_chunk = to_write;

        memcpy(&s_audio.ring_buf[s_audio.ring_head], data, first_chunk);
        if (to_write > first_chunk) {
            memcpy(&s_audio.ring_buf[0], data + first_chunk, to_write - first_chunk);
        }

        s_audio.ring_head = (s_audio.ring_head + to_write) % AUDIO_RING_SIZE;
        s_audio.ring_used += to_write;
    }
    LightLock_Unlock(&s_audio.ring_lock);

    return to_write;
}

void audio_clear_buffers(void) {
    LightLock_Lock(&s_audio.decode_lock);
    s_audio.is_playing = false;
    if (s_audio.backend == AUDIO_BACKEND_NDSP) ndsp_reset();
    else if (s_audio.backend == AUDIO_BACKEND_CSND) csnd_reset();

    LightLock_Lock(&s_audio.ring_lock);
    s_audio.ring_head = 0;
    s_audio.ring_tail = 0;
    s_audio.ring_used = 0;
    s_audio.pending_skip = 0;
    LightLock_Unlock(&s_audio.ring_lock);

    mp3dec_init(&s_audio.mp3d);
    s_audio.remainder_count = 0;
    s_audio.remainder_offset = 0;
    s_audio.decoded_real = 0;
    s_audio.starved = false;

    LightLock_Lock(&s_audio.pcm_lock);
    memset(s_audio.recent_pcm, 0, sizeof(s_audio.recent_pcm));
    LightLock_Unlock(&s_audio.pcm_lock);

    LightLock_Unlock(&s_audio.decode_lock);
}

bool audio_is_initialized(void) {
    return s_audio.initialized;
}

const char* audio_get_init_error(void) {
    return s_audio_init_error[0] ? s_audio_init_error : "unknown";
}

AudioBackend audio_get_backend(void) {
    return s_audio.backend;
}

const char* audio_get_backend_name(void) {
    switch (s_audio.backend) {
        case AUDIO_BACKEND_NDSP: return "NDSP";
        case AUDIO_BACKEND_CSND: return "CSND";
        default: return "--";
    }
}

uint32_t audio_get_position_ms(void) {
    if (!s_audio.initialized) return 0;
    LightLock_Lock(&s_audio.decode_lock);
    size_t buffered = (s_audio.backend == AUDIO_BACKEND_NDSP) ? ndsp_buffered_samples() : csnd_buffered_samples();
    u64 real = s_audio.decoded_real;
    LightLock_Unlock(&s_audio.decode_lock);

    u64 played = real > buffered ? real - buffered : 0;
    return (uint32_t)((played * 1000ULL) / AUDIO_SAMPLE_RATE);
}

bool audio_is_starved(void) {
    return s_audio.starved;
}

int audio_get_buffer_fill_percent(void) {
    LightLock_Lock(&s_audio.ring_lock);
    size_t used = s_audio.ring_used;
    LightLock_Unlock(&s_audio.ring_lock);

    return (int)((used * 100) / AUDIO_RING_SIZE);
}

void audio_get_recent_samples(int16_t* out_samples, size_t num_samples) {
    if (!out_samples || num_samples == 0) return;

    size_t copy_samples = (num_samples < AUDIO_BUF_SAMPLES) ? num_samples : AUDIO_BUF_SAMPLES;
    LightLock_Lock(&s_audio.pcm_lock);
    memcpy(out_samples, s_audio.recent_pcm, copy_samples * 2 * sizeof(int16_t));
    LightLock_Unlock(&s_audio.pcm_lock);
}
