#ifndef AUDIO_H
#define AUDIO_H

#include <3ds.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_CHANNELS      2
#define AUDIO_BUF_SAMPLES   4096
#define AUDIO_NUM_BUFFERS   3
#define AUDIO_RING_SIZE     (256 * 1024) // 256 KB ring buffer para streaming MP3

typedef enum {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_NDSP,   // Preferido: requiere sdmc:/3ds/dspfirm.cdc
    AUDIO_BACKEND_CSND    // Respaldo sin firmware DSP
} AudioBackend;

// Inicializa NDSP (o CSND como respaldo) y el worker multihilo de decodificación PCM
bool audio_init(void);

// Comprueba si el subsistema de audio está correctamente inicializado
bool audio_is_initialized(void);
const char* audio_get_init_error(void);

// Backend de salida activo
AudioBackend audio_get_backend(void);
const char* audio_get_backend_name(void);

// Tiempo reproducido de la pista actual, según las muestras enviadas al hardware
uint32_t audio_get_position_ms(void);

// true si el decodificador se quedó sin datos MP3 y está emitiendo silencio
bool audio_is_starved(void);

// Detiene el audio y libera el backend, buffers lineales e hilo
void audio_exit(void);

// Control de transporte
void audio_play(void);
void audio_pause(void);
void audio_stop(void);
bool audio_is_playing(void);

// Control de volumen (0.0f = silencio, 1.0f = maximo)
void audio_set_volume(float vol);
float audio_get_volume(void);

// Alimenta el ring buffer con nuevos bytes MP3 recibidos desde la red
size_t audio_feed_mp3_data(const uint8_t* data, size_t len);

// Reinicia el ring buffer de audio (para saltar o cambiar de pista)
void audio_clear_buffers(void);

// Obtiene el porcentaje de llenado del ring buffer (0 - 100)
int audio_get_buffer_fill_percent(void);

// Obtiene las ultimas muestras PCM decodificadas para el visualizador FFT
// out_samples debe tener espacio para num_samples * 2 (estéreo)
void audio_get_recent_samples(int16_t* out_samples, size_t num_samples);

#endif // AUDIO_H
