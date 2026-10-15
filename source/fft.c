#include "fft.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Tablas precalculadas en punto fijo Q14 (1.0 = 16384)
static int16_t s_cos_q14[FFT_POINTS / 2];
static int16_t s_sin_q14[FFT_POINTS / 2];
static uint8_t s_bit_rev[FFT_POINTS];
static bool s_fft_initialized = false;

// Estado de las 16 bandas
static float s_band_values[FFT_NUM_BANDS];
static float s_peak_values[FFT_NUM_BANDS];

// Rangos de bins para cada una de las 16 bandas (bins 1 a 63 de una FFT de 128 pts)
static const uint8_t s_band_start[FFT_NUM_BANDS] = {
    1,  2,  3,  4,  5,  6,  8, 10,
   13, 16, 20, 25, 31, 38, 46, 55
};

static const uint8_t s_band_end[FFT_NUM_BANDS] = {
    1,  2,  3,  4,  5,  7,  9, 12,
   15, 19, 24, 30, 37, 45, 54, 63
};

// Invertir 7 bits (para N = 128 = 2^7)
static uint8_t reverse_7bits(uint8_t x) {
    uint8_t res = 0;
    for (int i = 0; i < 7; i++) {
        res = (res << 1) | (x & 1);
        x >>= 1;
    }
    return res;
}

void fft_init(void) {
    if (s_fft_initialized) return;

    for (int i = 0; i < FFT_POINTS; i++) {
        s_bit_rev[i] = reverse_7bits((uint8_t)i);
    }

    for (int i = 0; i < FFT_POINTS / 2; i++) {
        double angle = -2.0 * M_PI * (double)i / (double)FFT_POINTS;
        s_cos_q14[i] = (int16_t)(cos(angle) * 16384.0);
        s_sin_q14[i] = (int16_t)(sin(angle) * 16384.0);
    }

    for (int i = 0; i < FFT_NUM_BANDS; i++) {
        s_band_values[i] = 0.0f;
        s_peak_values[i] = 0.0f;
    }

    s_fft_initialized = true;
}

void fft_process(const int16_t* pcm_stereo, size_t num_stereo_samples) {
    if (!s_fft_initialized) fft_init();

    int32_t real[FFT_POINTS];
    int32_t imag[FFT_POINTS];

    // 1. Extraer 128 muestras mono con ventana de Hann simplificada
    size_t count = (num_stereo_samples < FFT_POINTS) ? num_stereo_samples : FFT_POINTS;

    for (size_t i = 0; i < count; i++) {
        // Downmix estéreo a mono
        int32_t mono = ((int32_t)pcm_stereo[i * 2] + (int32_t)pcm_stereo[i * 2 + 1]) >> 1;
        
        // Ventana Hann aproximada: (1 - cos) usando tabla
        int window_idx = (i < FFT_POINTS / 2) ? i : (FFT_POINTS - 1 - i);
        int32_t w_scale = 16384 - s_cos_q14[window_idx]; // 0 a 32768
        
        real[s_bit_rev[i]] = (mono * (w_scale >> 2)) >> 14;
        imag[s_bit_rev[i]] = 0;
    }

    for (size_t i = count; i < FFT_POINTS; i++) {
        real[s_bit_rev[i]] = 0;
        imag[s_bit_rev[i]] = 0;
    }

    // 2. Cooley-Tukey Radix-2 en punto fijo (7 etapas)
    for (int len = 2; len <= FFT_POINTS; len <<= 1) {
        int half_len = len >> 1;
        int step = FFT_POINTS / len;

        for (int i = 0; i < FFT_POINTS; i += len) {
            int k = 0;
            for (int j = 0; j < half_len; j++) {
                int32_t c = s_cos_q14[k];
                int32_t s = s_sin_q14[k];

                int32_t u_r = real[i + j];
                int32_t u_i = imag[i + j];

                // Multiplicacion compleja en Q14
                int32_t v_r = (real[i + j + half_len] * c - imag[i + j + half_len] * s) >> 14;
                int32_t v_i = (real[i + j + half_len] * s + imag[i + j + half_len] * c) >> 14;

                real[i + j] = u_r + v_r;
                imag[i + j] = u_i + v_i;

                real[i + j + half_len] = u_r - v_r;
                imag[i + j + half_len] = u_i - v_i;

                k += step;
            }
        }
    }

    // 3. Magnitud aproximada y agrupacion en 16 bandas
    for (int b = 0; b < FFT_NUM_BANDS; b++) {
        int32_t band_sum = 0;
        int bin_count = s_band_end[b] - s_band_start[b] + 1;

        for (int bin = s_band_start[b]; bin <= s_band_end[b]; bin++) {
            int32_t ar = abs(real[bin]);
            int32_t ai = abs(imag[bin]);
            // Aproximacion Alpha-Beta: max(ar, ai) + 0.5 * min(ar, ai)
            int32_t mag = (ar > ai) ? (ar + (ai >> 1)) : (ai + (ar >> 1));
            band_sum += mag;
        }

        int32_t avg_mag = band_sum / bin_count;

        // Escalar a rango 0 - 255 con boost logarítmico para agudos
        int32_t boost = 10 + b * 2;
        int32_t scaled = (avg_mag * boost) >> 9;
        if (scaled > 255) scaled = 255;
        if (scaled < 0) scaled = 0;

        float target = (float)scaled;

        // Suavizado de subida rapida y caida suave
        if (target > s_band_values[b]) {
            s_band_values[b] = target;
        } else {
            s_band_values[b] = s_band_values[b] * 0.84f;
        }

        // Pico con caida mas lenta (peak cap)
        if (s_band_values[b] > s_peak_values[b]) {
            s_peak_values[b] = s_band_values[b];
        } else {
            s_peak_values[b] -= 2.5f;
            if (s_peak_values[b] < 0.0f) s_peak_values[b] = 0.0f;
        }
    }
}

void fft_get_bands(uint8_t out_bands[FFT_NUM_BANDS]) {
    for (int i = 0; i < FFT_NUM_BANDS; i++) {
        int v = (int)s_band_values[i];
        if (v > 255) v = 255;
        if (v < 0) v = 0;
        out_bands[i] = (uint8_t)v;
    }
}

void fft_get_peaks(uint8_t out_peaks[FFT_NUM_BANDS]) {
    for (int i = 0; i < FFT_NUM_BANDS; i++) {
        int v = (int)s_peak_values[i];
        if (v > 255) v = 255;
        if (v < 0) v = 0;
        out_peaks[i] = (uint8_t)v;
    }
}
