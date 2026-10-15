#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "audio.h"
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>

typedef struct {
    ndspWaveBuf waveBuf;
    int16_t* pcmData;
} AudioBufferSlot;

// Estructura interna del motor de audio
static struct {
    bool initialized;
    volatile bool is_playing;
    volatile bool thread_exit;
    float volume;

    // Buffers lineales para DSP
    AudioBufferSlot slots[AUDIO_NUM_BUFFERS];
    int current_slot;

    // Ring buffer para bytes MP3 comprimidos
    uint8_t ring_buf[AUDIO_RING_SIZE];
    size_t ring_head; // posicion de escritura
    size_t ring_tail; // posicion de lectura
    volatile size_t ring_used;
    LightLock ring_lock;

    // Hilo dedicado en Core 1 (Syscore)
    Thread worker_thread;
    u8* thread_stack;

    // Decodificador minimp3
    mp3dec_t mp3d;

    // Snapshot para el visualizador FFT
    int16_t recent_pcm[AUDIO_BUF_SAMPLES * 2];
    LightLock pcm_lock;
} s_audio;

#define AUDIO_THREAD_STACK_SZ (32 * 1024)

// Hilo worker ejecutado con afinidad a Core 1
static void audio_worker_func(void* arg) {
    int16_t temp_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    while (!s_audio.thread_exit) {
        if (!s_audio.is_playing) {
            svcSleepThread(10 * 1000 * 1000LL); // 10 ms
            continue;
        }

        bool fed_any = false;

        for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
            AudioBufferSlot* slot = &s_audio.slots[i];

            if (slot->waveBuf.status == NDSP_WBUF_DONE || slot->waveBuf.status == NDSP_WBUF_FREE) {
                size_t samples_decoded = 0;
                int16_t* dst = slot->pcmData;

                // Llenar el buffer del DSP (AUDIO_BUF_SAMPLES = 4096 muestras estéreo)
                while (samples_decoded < AUDIO_BUF_SAMPLES) {
                    LightLock_Lock(&s_audio.ring_lock);
                    size_t avail = s_audio.ring_used;
                    LightLock_Unlock(&s_audio.ring_lock);

                    if (avail < 512) {
                        // Si no hay suficientes bytes en el ring buffer, rellenar con silencio
                        size_t remaining = AUDIO_BUF_SAMPLES - samples_decoded;
                        memset(dst + (samples_decoded * 2), 0, remaining * 2 * sizeof(int16_t));
                        samples_decoded += remaining;
                        break;
                    }

                    // Preparar chunk continuo desde el ring buffer
                    uint8_t decode_in[1500];
                    size_t to_read = avail > sizeof(decode_in) ? sizeof(decode_in) : avail;

                    LightLock_Lock(&s_audio.ring_lock);
                    for (size_t b = 0; b < to_read; b++) {
                        decode_in[b] = s_audio.ring_buf[(s_audio.ring_tail + b) % AUDIO_RING_SIZE];
                    }
                    LightLock_Unlock(&s_audio.ring_lock);

                    mp3dec_frame_info_t info;
                    int samples = mp3dec_decode_frame(&s_audio.mp3d, decode_in, to_read, temp_pcm, &info);

                    if (info.frame_bytes > 0) {
                        LightLock_Lock(&s_audio.ring_lock);
                        s_audio.ring_tail = (s_audio.ring_tail + info.frame_bytes) % AUDIO_RING_SIZE;
                        s_audio.ring_used -= info.frame_bytes;
                        LightLock_Unlock(&s_audio.ring_lock);
                    } else {
                        // Skip un byte corrupto si no es cabecera
                        LightLock_Lock(&s_audio.ring_lock);
                        s_audio.ring_tail = (s_audio.ring_tail + 1) % AUDIO_RING_SIZE;
                        s_audio.ring_used--;
                        LightLock_Unlock(&s_audio.ring_lock);
                        continue;
                    }

                    if (samples > 0) {
                        static int s_logged_frames = 0;
                        if (s_logged_frames < 5) {
                            s_logged_frames++;
                            FILE* f_log = fopen("sound3ds_audio.log", "a");
                            if (f_log) {
                                fprintf(f_log, "[AUDIO] Frame #%d: samples=%d, ch=%d, hz=%d, slot_status=%d\n",
                                        s_logged_frames, samples, info.channels, info.hz, slot->waveBuf.status);
                                fclose(f_log);
                            }
                        }

                        size_t to_copy = samples;
                        if (samples_decoded + to_copy > AUDIO_BUF_SAMPLES) {
                            to_copy = AUDIO_BUF_SAMPLES - samples_decoded;
                        }

                        if (info.channels == 2) {
                            memcpy(dst + (samples_decoded * 2), temp_pcm, to_copy * 2 * sizeof(int16_t));
                        } else if (info.channels == 1) {
                            // Duplicar mono a estéreo
                            for (size_t s = 0; s < to_copy; s++) {
                                dst[(samples_decoded + s) * 2] = temp_pcm[s];
                                dst[(samples_decoded + s) * 2 + 1] = temp_pcm[s];
                            }
                        }
                        samples_decoded += to_copy;
                    }
                }

                // Guardar snapshot de audio para el visualizador FFT
                LightLock_Lock(&s_audio.pcm_lock);
                memcpy(s_audio.recent_pcm, slot->pcmData, sizeof(s_audio.recent_pcm));
                LightLock_Unlock(&s_audio.pcm_lock);

                // Forzar flush de caché de CPU antes de entregar al hardware DSP
                DSP_FlushDataCache(slot->pcmData, AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));

                slot->waveBuf.nsamples = AUDIO_BUF_SAMPLES;
                ndspChnWaveBufAdd(0, &slot->waveBuf);
                fed_any = true;
            }
        }

        if (!fed_any) {
            svcSleepThread(3 * 1000 * 1000LL); // 3 ms
        }
    }
}

bool audio_init(void) {
    if (s_audio.initialized) return true;

    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.volume = 0.85f;
    LightLock_Init(&s_audio.ring_lock);
    LightLock_Init(&s_audio.pcm_lock);

    // 1. Desbloquear tiempo de CPU en Syscore (Core 1)
    APT_SetAppCpuTimeLimit(30);

    // 2. Inicializar DSP
    Result res = ndspInit();
    if (R_FAILED(res)) {
        return false;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(0, (float)AUDIO_SAMPLE_RATE);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);

    // Configurar volumen inicial
    audio_set_volume(s_audio.volume);

    // 3. Alocar memoria lineal para buffers de audio
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        size_t buf_bytes = AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t);
        s_audio.slots[i].pcmData = (int16_t*)linearAlloc(buf_bytes);
        if (!s_audio.slots[i].pcmData) {
            audio_exit();
            return false;
        }
        memset(s_audio.slots[i].pcmData, 0, buf_bytes);
        memset(&s_audio.slots[i].waveBuf, 0, sizeof(ndspWaveBuf));
        s_audio.slots[i].waveBuf.data_vaddr = s_audio.slots[i].pcmData;
        s_audio.slots[i].waveBuf.nsamples = AUDIO_BUF_SAMPLES;
        s_audio.slots[i].waveBuf.status = NDSP_WBUF_FREE;
    }

    // 4. Inicializar decoder minimp3
    mp3dec_init(&s_audio.mp3d);

    // 5. Crear hilo de audio con alta prioridad (0x2F) y afinidad libre (-1)
    s_audio.thread_stack = (u8*)memalign(32, AUDIO_THREAD_STACK_SZ);
    if (!s_audio.thread_stack) {
        audio_exit();
        return false;
    }

    s_audio.thread_exit = false;
    s_audio.worker_thread = threadCreate(
        audio_worker_func,
        NULL,
        AUDIO_THREAD_STACK_SZ,
        0x2F, // Mayor prioridad que el hilo principal (0x30)
        -1,   // Cualquier núcleo disponible
        false
    );

    if (!s_audio.worker_thread) {
        audio_exit();
        return false;
    }

    FILE* f_log = fopen("sound3ds_audio.log", "w");
    if (f_log) {
        fprintf(f_log, "[AUDIO] ndspInit: 0x%08lX, worker: %p, vol: %.2f\n",
                (unsigned long)res, s_audio.worker_thread, s_audio.volume);
        fclose(f_log);
    }

    s_audio.initialized = true;
    return true;
}

void audio_exit(void) {
    if (!s_audio.initialized) return;

    s_audio.thread_exit = true;
    s_audio.is_playing = false;

    if (s_audio.worker_thread) {
        threadJoin(s_audio.worker_thread, U64_MAX);
        threadFree(s_audio.worker_thread);
        s_audio.worker_thread = NULL;
    }

    if (s_audio.thread_stack) {
        free(s_audio.thread_stack);
        s_audio.thread_stack = NULL;
    }

    ndspChnReset(0);

    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        if (s_audio.slots[i].pcmData) {
            linearFree(s_audio.slots[i].pcmData);
            s_audio.slots[i].pcmData = NULL;
        }
    }

    ndspExit();
    s_audio.initialized = false;
}

void audio_play(void) {
    s_audio.is_playing = true;
}

void audio_pause(void) {
    s_audio.is_playing = false;
}

void audio_stop(void) {
    s_audio.is_playing = false;
    audio_clear_buffers();
}

bool audio_is_playing(void) {
    return s_audio.is_playing;
}

void audio_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_audio.volume = vol;

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = vol; // L
    mix[1] = vol; // R
    ndspChnSetMix(0, mix);
}

float audio_get_volume(void) {
    return s_audio.volume;
}

size_t audio_feed_mp3_data(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;

    LightLock_Lock(&s_audio.ring_lock);
    size_t space_free = AUDIO_RING_SIZE - s_audio.ring_used;
    size_t to_write = (len < space_free) ? len : space_free;

    for (size_t i = 0; i < to_write; i++) {
        s_audio.ring_buf[(s_audio.ring_head + i) % AUDIO_RING_SIZE] = data[i];
    }
    s_audio.ring_head = (s_audio.ring_head + to_write) % AUDIO_RING_SIZE;
    s_audio.ring_used += to_write;
    LightLock_Unlock(&s_audio.ring_lock);

    return to_write;
}

void audio_clear_buffers(void) {
    LightLock_Lock(&s_audio.ring_lock);
    s_audio.ring_head = 0;
    s_audio.ring_tail = 0;
    s_audio.ring_used = 0;
    LightLock_Unlock(&s_audio.ring_lock);

    mp3dec_init(&s_audio.mp3d);
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
