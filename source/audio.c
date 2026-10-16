#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "audio.h"
#include <3ds/services/csnd.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>

typedef struct {
    int16_t* pcmData;
    int16_t* left_pcm;
    int16_t* right_pcm;
    bool queued;
} AudioBufferSlot;

// Estructura interna del motor de audio
static struct {
    bool initialized;
    bool csnd_initialized;
    volatile bool is_playing;
    volatile bool thread_exit;
    float volume;

    // Buffers lineales para DSP
    AudioBufferSlot slots[AUDIO_NUM_BUFFERS];
    int current_slot;
    int left_channel;
    int right_channel;
    int active_slot;
    int paused_slot;

    // Ring buffer para bytes MP3 comprimidos
    uint8_t ring_buf[AUDIO_RING_SIZE];
    size_t ring_head; // posicion de escritura
    size_t ring_tail; // posicion de lectura
    volatile size_t ring_used;
    LightLock ring_lock;
    LightLock decode_lock;

    // Muestras restantes (remainder) para evitar descartar PCM al llenar slots
    int16_t remainder_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    size_t remainder_count;  // Muestras estéreo disponibles
    size_t remainder_offset; // Muestras estéreo ya consumidas

    // Hilo dedicado de decodificación y audio
    Thread worker_thread;

    // Decodificador minimp3
    mp3dec_t mp3d;

    // Snapshot para el visualizador FFT
    int16_t recent_pcm[AUDIO_BUF_SAMPLES * 2];
    LightLock pcm_lock;
} s_audio;

static char s_audio_init_error[32];

#define AUDIO_THREAD_STACK_SZ (32 * 1024)

// CSND consume PCM mono por canal. Dividimos cada bloque estéreo y usamos
// dos canales sincronizados, sin cargar el firmware privado que exige NDSP.
static Result audio_submit_slot(AudioBufferSlot* slot) {
    size_t mono_bytes = AUDIO_BUF_SAMPLES * sizeof(int16_t);
    CSND_FlushDataCache(slot->left_pcm, (u32)mono_bytes);
    CSND_FlushDataCache(slot->right_pcm, (u32)mono_bytes);

    u32 flags = SOUND_ONE_SHOT | SOUND_FORMAT_16BIT | SOUND_ENABLE;
    Result left = csndPlaySound(s_audio.left_channel, flags, AUDIO_SAMPLE_RATE,
                                s_audio.volume, -1.0f, slot->left_pcm, NULL, (u32)mono_bytes);
    if (R_FAILED(left)) return left;

    Result right = csndPlaySound(s_audio.right_channel, flags, AUDIO_SAMPLE_RATE,
                                 s_audio.volume, 1.0f, slot->right_pcm, NULL, (u32)mono_bytes);
    if (R_FAILED(right)) {
        CSND_SetPlayStateR((u32)s_audio.left_channel, 0);
        csndExecCmds(true);
    }
    return right;
}

static bool audio_csnd_is_playing(void) {
    u8 left = 0, right = 0;
    if (R_FAILED(CSND_UpdateInfo(false))) return true;
    if (R_FAILED(csndIsPlaying((u32)s_audio.left_channel, &left))) return true;
    if (R_FAILED(csndIsPlaying((u32)s_audio.right_channel, &right))) return true;
    return left || right;
}

// Hilo worker: decodifica un bloque y lo reproduce por CSND.
static void audio_worker_func(void* arg) {
    int16_t temp_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    while (!s_audio.thread_exit) {
        if (!s_audio.is_playing) {
            svcSleepThread(10 * 1000 * 1000LL);
            continue;
        }

        LightLock_Lock(&s_audio.decode_lock);
        if (!s_audio.is_playing || s_audio.thread_exit) {
            LightLock_Unlock(&s_audio.decode_lock);
            continue;
        }

        if (s_audio.active_slot >= 0) {
            if (audio_csnd_is_playing()) {
                LightLock_Unlock(&s_audio.decode_lock);
                svcSleepThread(2 * 1000 * 1000LL);
                continue;
            }
            s_audio.slots[s_audio.active_slot].queued = false;
            s_audio.active_slot = -1;
        }

        AudioBufferSlot* slot = NULL;
        int slot_index = -1;
        for (int n = 0; n < AUDIO_NUM_BUFFERS; n++) {
            int i = (s_audio.current_slot + n) % AUDIO_NUM_BUFFERS;
            if (!s_audio.slots[i].queued) {
                slot = &s_audio.slots[i];
                slot_index = i;
                break;
            }
        }
        if (!slot) {
            LightLock_Unlock(&s_audio.decode_lock);
            svcSleepThread(2 * 1000 * 1000LL);
            continue;
        }

        size_t samples_decoded = 0;
        int16_t* dst = slot->pcmData;

        if (s_audio.remainder_count > 0) {
            size_t avail_rem = s_audio.remainder_count - s_audio.remainder_offset;
            size_t to_take = avail_rem;
            if (to_take > AUDIO_BUF_SAMPLES) to_take = AUDIO_BUF_SAMPLES;
            memcpy(dst, s_audio.remainder_pcm + s_audio.remainder_offset * 2,
                   to_take * 2 * sizeof(int16_t));
            samples_decoded += to_take;
            s_audio.remainder_offset += to_take;
            if (s_audio.remainder_offset >= s_audio.remainder_count) {
                s_audio.remainder_count = 0;
                s_audio.remainder_offset = 0;
            }
        }

        while (samples_decoded < AUDIO_BUF_SAMPLES) {
            LightLock_Lock(&s_audio.ring_lock);
            size_t avail = s_audio.ring_used;
            LightLock_Unlock(&s_audio.ring_lock);

            if (avail < 512) {
                size_t remaining = AUDIO_BUF_SAMPLES - samples_decoded;
                memset(dst + samples_decoded * 2, 0, remaining * 2 * sizeof(int16_t));
                samples_decoded += remaining;
                break;
            }

            uint8_t decode_in[1500];
            size_t to_read = avail > sizeof(decode_in) ? sizeof(decode_in) : avail;
            LightLock_Lock(&s_audio.ring_lock);
            for (size_t b = 0; b < to_read; b++)
                decode_in[b] = s_audio.ring_buf[(s_audio.ring_tail + b) % AUDIO_RING_SIZE];
            LightLock_Unlock(&s_audio.ring_lock);

            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&s_audio.mp3d, decode_in, (int)to_read, temp_pcm, &info);
            if (info.frame_bytes <= 0) {
                LightLock_Lock(&s_audio.ring_lock);
                if (s_audio.ring_used > 0) {
                    s_audio.ring_tail = (s_audio.ring_tail + 1) % AUDIO_RING_SIZE;
                    s_audio.ring_used--;
                }
                LightLock_Unlock(&s_audio.ring_lock);
                continue;
            }

            LightLock_Lock(&s_audio.ring_lock);
            if (s_audio.ring_used >= (size_t)info.frame_bytes) {
                s_audio.ring_tail = (s_audio.ring_tail + info.frame_bytes) % AUDIO_RING_SIZE;
                s_audio.ring_used -= info.frame_bytes;
            }
            LightLock_Unlock(&s_audio.ring_lock);

            if (samples <= 0) continue;

            int16_t frame_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
            if (info.channels == 2) {
                memcpy(frame_stereo, temp_pcm, (size_t)samples * 2 * sizeof(int16_t));
            } else {
                for (int s = 0; s < samples; s++) {
                    frame_stereo[s * 2] = temp_pcm[s];
                    frame_stereo[s * 2 + 1] = temp_pcm[s];
                }
            }

            size_t needed = AUDIO_BUF_SAMPLES - samples_decoded;
            if ((size_t)samples <= needed) {
                memcpy(dst + samples_decoded * 2, frame_stereo,
                       (size_t)samples * 2 * sizeof(int16_t));
                samples_decoded += samples;
            } else {
                memcpy(dst + samples_decoded * 2, frame_stereo,
                       needed * 2 * sizeof(int16_t));
                samples_decoded += needed;
                size_t excess = (size_t)samples - needed;
                memcpy(s_audio.remainder_pcm, frame_stereo + needed * 2,
                       excess * 2 * sizeof(int16_t));
                s_audio.remainder_count = excess;
                s_audio.remainder_offset = 0;
            }
        }

        for (size_t i = 0; i < AUDIO_BUF_SAMPLES; i++) {
            slot->left_pcm[i] = dst[i * 2];
            slot->right_pcm[i] = dst[i * 2 + 1];
        }
        LightLock_Lock(&s_audio.pcm_lock);
        memcpy(s_audio.recent_pcm, dst, sizeof(s_audio.recent_pcm));
        LightLock_Unlock(&s_audio.pcm_lock);

        Result submit_result = audio_submit_slot(slot);
        if (R_SUCCEEDED(submit_result)) {
            slot->queued = true;
            s_audio.active_slot = slot_index;
            s_audio.current_slot = (slot_index + 1) % AUDIO_NUM_BUFFERS;
        }
        LightLock_Unlock(&s_audio.decode_lock);

        if (R_FAILED(submit_result)) svcSleepThread(5 * 1000 * 1000LL);
    }
}

bool audio_init(void) {
    if (s_audio.initialized) return true;

    s_audio_init_error[0] = '\0';

    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.volume = 0.85f;
    LightLock_Init(&s_audio.ring_lock);
    LightLock_Init(&s_audio.decode_lock);
    LightLock_Init(&s_audio.pcm_lock);

    // 1. Desbloquear tiempo de CPU en Syscore (Core 1)
    APT_SetAppCpuTimeLimit(30);

    // CSND reproduce PCM directamente desde buffers lineales y no requiere
    // sdmc:/3ds/dspfirm.cdc como el backend NDSP.
    Result res = csndInit();
    if (R_FAILED(res)) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "csnd 0x%08lX", (unsigned long)res);
        return false;
    }
    s_audio.csnd_initialized = true;

    u32 available_channels = csndChannels;
    int found_channels = 0;
    for (int channel = 0; channel < CSND_NUM_CHANNELS && found_channels < 2; channel++) {
        if (available_channels & (1u << channel)) {
            if (found_channels == 0) s_audio.left_channel = channel;
            else s_audio.right_channel = channel;
            found_channels++;
        }
    }
    if (found_channels < 2) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "CSND needs 2 channels");
        csndExit();
        s_audio.csnd_initialized = false;
        return false;
    }
    s_audio.active_slot = -1;
    s_audio.paused_slot = -1;

    // 3. Alocar memoria lineal para buffers de audio
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        size_t buf_bytes = AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t);
        s_audio.slots[i].pcmData = (int16_t*)linearAlloc(buf_bytes);
        s_audio.slots[i].left_pcm = (int16_t*)linearAlloc(AUDIO_BUF_SAMPLES * sizeof(int16_t));
        s_audio.slots[i].right_pcm = (int16_t*)linearAlloc(AUDIO_BUF_SAMPLES * sizeof(int16_t));
        if (!s_audio.slots[i].pcmData || !s_audio.slots[i].left_pcm || !s_audio.slots[i].right_pcm) {
            snprintf(s_audio_init_error, sizeof(s_audio_init_error), "linearAlloc %d", i);
            audio_exit();
            return false;
        }
        memset(s_audio.slots[i].pcmData, 0, buf_bytes);
        memset(s_audio.slots[i].left_pcm, 0, AUDIO_BUF_SAMPLES * sizeof(int16_t));
        memset(s_audio.slots[i].right_pcm, 0, AUDIO_BUF_SAMPLES * sizeof(int16_t));
    }

    // 4. Inicializar decoder minimp3
    mp3dec_init(&s_audio.mp3d);

    // 5. Crear hilo de audio con prioridad segura y fallback de afinidad
    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);

    int target_prio = main_prio - 1;
    if (target_prio < 0x18) target_prio = 0x18;
    if (target_prio > 0x3F) target_prio = 0x3F;

    s_audio.thread_exit = false;

    // Intento 1: Core default (-2) con prioridad ligeramente superior
    s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, target_prio, -2, false);

    // Intento 2: Core default (-2) con misma prioridad que el hilo principal
    if (!s_audio.worker_thread) {
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, -2, false);
    }

    // Intento 3: Core 0 explícito
    if (!s_audio.worker_thread) {
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, 0, false);
    }

    // Intento 4: Core 1 (SysCore si está habilitado por APT_SetAppCpuTimeLimit)
    if (!s_audio.worker_thread) {
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, 1, false);
    }

    // Intento 5: Core -1 (si el kernel lo permite)
    if (!s_audio.worker_thread) {
        s_audio.worker_thread = threadCreate(audio_worker_func, NULL, AUDIO_THREAD_STACK_SZ, main_prio, -1, false);
    }

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

    if (s_audio.csnd_initialized) {
        CSND_SetPlayStateR((u32)s_audio.left_channel, 0);
        CSND_SetPlayStateR((u32)s_audio.right_channel, 0);
        csndExecCmds(true);
    }

    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        if (s_audio.slots[i].pcmData) {
            linearFree(s_audio.slots[i].pcmData);
            s_audio.slots[i].pcmData = NULL;
        }
        if (s_audio.slots[i].left_pcm) {
            linearFree(s_audio.slots[i].left_pcm);
            s_audio.slots[i].left_pcm = NULL;
        }
        if (s_audio.slots[i].right_pcm) {
            linearFree(s_audio.slots[i].right_pcm);
            s_audio.slots[i].right_pcm = NULL;
        }
    }

    if (s_audio.csnd_initialized) {
        csndExit();
        s_audio.csnd_initialized = false;
    }
    s_audio.initialized = false;
}

void audio_play(void) {
    if (!s_audio.initialized) return;
    LightLock_Lock(&s_audio.decode_lock);
    if (s_audio.paused_slot >= 0) {
        int slot = s_audio.paused_slot;
        if (R_SUCCEEDED(audio_submit_slot(&s_audio.slots[slot]))) {
            s_audio.active_slot = slot;
            s_audio.paused_slot = -1;
        }
    }
    s_audio.is_playing = true;
    LightLock_Unlock(&s_audio.decode_lock);
}

void audio_pause(void) {
    if (!s_audio.initialized) return;
    LightLock_Lock(&s_audio.decode_lock);
    s_audio.is_playing = false;
    if (s_audio.active_slot >= 0) {
        s_audio.paused_slot = s_audio.active_slot;
        s_audio.active_slot = -1;
        CSND_SetPlayStateR((u32)s_audio.left_channel, 0);
        CSND_SetPlayStateR((u32)s_audio.right_channel, 0);
        csndExecCmds(true);
    }
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
    if (s_audio.csnd_initialized && s_audio.active_slot >= 0) {
        CSND_SetVol((u32)s_audio.left_channel, CSND_VOL(vol, -1.0f), 0);
        CSND_SetVol((u32)s_audio.right_channel, CSND_VOL(vol, 1.0f), 0);
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
        if (first_chunk > to_write) {
            first_chunk = to_write;
        }

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
    if (s_audio.csnd_initialized) {
        CSND_SetPlayStateR((u32)s_audio.left_channel, 0);
        CSND_SetPlayStateR((u32)s_audio.right_channel, 0);
        csndExecCmds(true);
    }
    LightLock_Lock(&s_audio.ring_lock);

    s_audio.ring_head = 0;
    s_audio.ring_tail = 0;
    s_audio.ring_used = 0;

    mp3dec_init(&s_audio.mp3d);
    s_audio.remainder_count = 0;
    s_audio.remainder_offset = 0;

    s_audio.active_slot = -1;
    s_audio.paused_slot = -1;
    s_audio.current_slot = 0;
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        if (s_audio.slots[i].pcmData) {
            memset(s_audio.slots[i].pcmData, 0, AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));
        }
        if (s_audio.slots[i].left_pcm)
            memset(s_audio.slots[i].left_pcm, 0, AUDIO_BUF_SAMPLES * sizeof(int16_t));
        if (s_audio.slots[i].right_pcm)
            memset(s_audio.slots[i].right_pcm, 0, AUDIO_BUF_SAMPLES * sizeof(int16_t));
        s_audio.slots[i].queued = false;
    }

    LightLock_Lock(&s_audio.pcm_lock);
    memset(s_audio.recent_pcm, 0, sizeof(s_audio.recent_pcm));
    LightLock_Unlock(&s_audio.pcm_lock);

    LightLock_Unlock(&s_audio.ring_lock);
    LightLock_Unlock(&s_audio.decode_lock);
}

bool audio_is_initialized(void) {
    return s_audio.initialized;
}

const char* audio_get_init_error(void) {
    return s_audio_init_error[0] ? s_audio_init_error : "unknown";
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
