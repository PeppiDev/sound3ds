#ifndef FFT_H
#define FFT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FFT_POINTS  128
#define FFT_NUM_BANDS 16

// Inicializa las tablas precalculadas de senos/cosenos y permutacion de bits
void fft_init(void);

// Procesa una ventana de muestras PCM estéreo (interleaved L, R)
// y actualiza las 16 bandas de frecuencia
void fft_process(const int16_t* pcm_stereo, size_t num_stereo_samples);

// Obtiene los niveles actuales de las 16 bandas (valores normalizados 0 - 255)
void fft_get_bands(uint8_t out_bands[FFT_NUM_BANDS]);

// Obtiene los picos maximos con decaimiento suave para visualizador tipo LED/VU
void fft_get_peaks(uint8_t out_peaks[FFT_NUM_BANDS]);

#endif // FFT_H
