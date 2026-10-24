#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "audio.h"
#include "aacdec.h"
#include "debuglog.h"
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
    volatile float decode_load; // tiempo de decodificación / duración del audio (media móvil)

    Thread worker_thread;
    AudioCodec codec;
    int sample_rate;        // frecuencia de salida actual (sigue a la de la pista)
    volatile int pcm_rate;  // modo PCM: frecuencia del audio entregado
    mp3dec_t mp3d;
    HAACDecoder aacd;
    uint8_t aac_in[4096];   // trama ADTS completa copiada del ring buffer
    int16_t aac_out[2048 * 2];

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

static void apply_sample_rate(int rate);

// Registra cuánto tardó en decodificarse una trama respecto de lo que dura
static void track_decode_load(u64 start_tick, int samples) {
    if (samples <= 0 || s_audio.sample_rate <= 0) return;
    double spent = (double)(svcGetSystemTick() - start_tick);
    double lasts = (double)samples * SYSCLOCK_ARM11 / s_audio.sample_rate;
    s_audio.decode_load = s_audio.decode_load * 0.95f + (float)(spent / lasts) * 0.05f;
}

// Siguiente trama MP3 del ring buffer, en estéreo intercalado. Devuelve las muestras
// obtenidas, 0 si solo se descartaron bytes, o -1 si faltan datos.
static int next_mp3_frame(int16_t* frame_stereo) {
    int16_t temp_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint8_t decode_in[1500];

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
        return -1;
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
        return 0;
    }
    LightLock_Unlock(&s_audio.ring_lock);

    mp3dec_frame_info_t info;
    u64 t0 = svcGetSystemTick();
    int samples = mp3dec_decode_frame(&s_audio.mp3d, decode_in, (int)to_read, temp_pcm, &info);
    track_decode_load(t0, samples);

    LightLock_Lock(&s_audio.ring_lock);
    ring_consume(info.frame_bytes > 0 ? (size_t)info.frame_bytes : 1);
    LightLock_Unlock(&s_audio.ring_lock);

    if (info.frame_bytes <= 0 || samples <= 0) return 0;
    if (info.hz > 0 && info.hz != s_audio.sample_rate) apply_sample_rate(info.hz);

    if (info.channels == 2) {
        memcpy(frame_stereo, temp_pcm, (size_t)samples * 2 * sizeof(int16_t));
    } else {
        for (int i = 0; i < samples; i++) {
            frame_stereo[i * 2] = temp_pcm[i];
            frame_stereo[i * 2 + 1] = temp_pcm[i];
        }
    }
    return samples;
}

// Siguiente trama AAC (con cabecera ADTS) del ring buffer. Mismo contrato que next_mp3_frame.
static int next_aac_frame(int16_t* frame_stereo) {
    uint8_t* in = s_audio.aac_in;

    LightLock_Lock(&s_audio.ring_lock);
    size_t avail = s_audio.ring_used;
    if (avail < 7) {
        LightLock_Unlock(&s_audio.ring_lock);
        return -1;
    }
    size_t to_read = avail > sizeof(s_audio.aac_in) ? sizeof(s_audio.aac_in) : avail;
    ring_peek(in, to_read);

    int sync = AACFindSyncWord(in, (int)to_read);
    if (sync != 0) {
        // Basura antes de la cabecera: descartarla (sin sincronía se conserva el último byte)
        ring_consume(sync > 0 ? (size_t)sync : to_read - 1);
        LightLock_Unlock(&s_audio.ring_lock);
        return 0;
    }

    size_t frame_len = ((size_t)(in[3] & 3) << 11) | ((size_t)in[4] << 3) | (in[5] >> 5);
    if (frame_len < 8 || frame_len > sizeof(s_audio.aac_in)) {
        ring_consume(1);
        LightLock_Unlock(&s_audio.ring_lock);
        return 0;
    }
    if (to_read < frame_len) {
        LightLock_Unlock(&s_audio.ring_lock);
        return -1; // la trama aún no llegó completa
    }
    LightLock_Unlock(&s_audio.ring_lock);

    unsigned char* p = in;
    int left = (int)frame_len;
    u64 t0 = svcGetSystemTick();
    int err = AACDecode(s_audio.aacd, &p, &left, s_audio.aac_out);

    LightLock_Lock(&s_audio.ring_lock);
    ring_consume(frame_len);
    LightLock_Unlock(&s_audio.ring_lock);
    if (err) return 0;

    AACFrameInfo fi;
    AACGetLastFrameInfo(s_audio.aacd, &fi);
    if (fi.nChans < 1 || fi.nChans > 2 || fi.outputSamps <= 0) return 0;
    if (fi.sampRateOut > 0 && fi.sampRateOut != s_audio.sample_rate) apply_sample_rate(fi.sampRateOut);

    int samples = fi.outputSamps / fi.nChans;
    track_decode_load(t0, samples);
    if (fi.nChans == 2) {
        memcpy(frame_stereo, s_audio.aac_out, (size_t)samples * 2 * sizeof(int16_t));
    } else {
        for (int i = 0; i < samples; i++) {
            frame_stereo[i * 2] = s_audio.aac_out[i];
            frame_stereo[i * 2 + 1] = s_audio.aac_out[i];
        }
    }
    return samples;
}

// Siguiente bloque PCM (estéreo 16 bits ya decodificado, p. ej. FLAC) del ring buffer
static int next_pcm_frame(int16_t* frame_stereo) {
    if (s_audio.pcm_rate > 0 && s_audio.pcm_rate != s_audio.sample_rate) apply_sample_rate(s_audio.pcm_rate);
    LightLock_Lock(&s_audio.ring_lock);
    size_t frames = s_audio.ring_used / 4;
    if (frames == 0) {
        LightLock_Unlock(&s_audio.ring_lock);
        return -1;
    }
    if (frames > MINIMP3_MAX_SAMPLES_PER_FRAME) frames = MINIMP3_MAX_SAMPLES_PER_FRAME;
    ring_peek((uint8_t*)frame_stereo, frames * 4);
    ring_consume(frames * 4);
    LightLock_Unlock(&s_audio.ring_lock);
    return (int)frames;
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

    int16_t frame_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    while (filled < n) {
        int samples = s_audio.codec == AUDIO_CODEC_AAC ? next_aac_frame(frame_stereo)
                    : s_audio.codec == AUDIO_CODEC_PCM ? next_pcm_frame(frame_stereo)
                                                       : next_mp3_frame(frame_stereo);
        if (samples < 0) break;     // faltan datos: el resto del bloque será silencio
        if (samples == 0) continue; // se descartaron bytes sin audio

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

// Al cerrar la consola el sistema manda dormir al DSP (APTCMD_DSP_SLEEP) aunque
// la app haya rechazado el reposo: libctru descarga el componente y NDSP se queda
// esperando el despertar, así que la música se corta. Lo detectamos con un hook y
// lo despertamos nosotros con la misma rutina que usa libctru al abrir la tapa.
extern void aptDspWakeup(void);
static dspHookCookie s_dsp_cookie;
static volatile bool s_dsp_slept = false;

static void audio_dsp_hook(DSP_HookType hook) {
    s_dsp_slept = (hook == DSPHOOK_ONSLEEP);
    dlog("dsp hook %s", hook == DSPHOOK_ONSLEEP ? "sleep" : hook == DSPHOOK_ONWAKEUP ? "wakeup" : "cancel");
}

void audio_keep_dsp_awake(void) {
    // aptDspWakeup no hace nada si el DSP aún no terminó de dormirse; se reintenta
    // en cada vuelta hasta que el hook reciba DSPHOOK_ONWAKEUP.
    if (s_dsp_slept) {
        dlog("dsp wakeup forzado");
        aptDspWakeup();
    }
}

// Con la tapa cerrada el códec manda el audio solo a los audífonos. Este es el mismo
// registro que usa Luma3DS para "force audio output" (página 100, registro 69):
// 0x20 = parlantes, 0x30 = audífonos. Al abrir la tapa el códec lo restablece solo.
#define CDC_OUT_PAGE      100
#define CDC_OUT_REG       69
#define CDC_OUT_SPEAKERS  0x20

static bool s_cdc_normal_known = false;
static u8 s_cdc_normal = 0;

// cdc:CHK sin bloquear: si el servicio no existe (emuladores) o está ocupado, falla
// al instante en vez de dejar la app esperando
static bool cdc_open(void) {
    srvSetBlockingPolicy(true);
    Result res = cdcChkInit();
    srvSetBlockingPolicy(false);
    return R_SUCCEEDED(res);
}

static bool cdc_read_out(u8* val) {
    if (!cdc_open()) return false;
    bool ok = R_SUCCEEDED(CDCCHK_ReadRegisters2(CDC_OUT_PAGE, CDC_OUT_REG, val, 1));
    cdcChkExit();
    return ok;
}

static void cdc_write_out(u8 val) {
    if (!cdc_open()) return;
    Result res = CDCCHK_WriteRegisters2(CDC_OUT_PAGE, CDC_OUT_REG, &val, 1);
    cdcChkExit();
    dlog("cdc salida <- 0x%02X (0x%08lX)", val, (unsigned long)res);
}

void audio_remember_output_route(void) {
    if (cdc_read_out(&s_cdc_normal)) {
        s_cdc_normal_known = true;
        dlog("cdc salida normal = 0x%02X", s_cdc_normal);
    } else {
        dlog("cdc:CHK no disponible");
    }
}

void audio_route_for_lid(bool closed) {
    if (!s_cdc_normal_known) return;
    u8 cur;
    if (!cdc_read_out(&cur)) return;
    bool headphones = false;
    DSP_GetHeadphoneStatus(&headphones);
    if (closed && !headphones) {
        if (cur != CDC_OUT_SPEAKERS) cdc_write_out(CDC_OUT_SPEAKERS);
    } else if (cur == CDC_OUT_SPEAKERS && s_cdc_normal != CDC_OUT_SPEAKERS) {
        // Tapa abierta (o audífonos conectados) y seguimos forzando parlantes: deshacerlo
        cdc_write_out(s_cdc_normal);
    }
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
    dspHook(&s_dsp_cookie, audio_dsp_hook);
    return true;
}

static void ndsp_backend_exit(void) {
    if (s_audio.ndsp.initialized) {
        dspUnhook(&s_dsp_cookie);
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

// La pista cambió de frecuencia (44,1 o 48 kHz): ajustar la salida. Se llama
// desde el worker con decode_lock tomado.
static void apply_sample_rate(int rate) {
    s_audio.sample_rate = rate;
    if (s_audio.backend == AUDIO_BACKEND_NDSP) {
        ndspChnSetRate(0, (float)rate);
    } else if (s_audio.backend == AUDIO_BACKEND_CSND) {
        s_audio.csnd.timer = CSND_TIMER(rate);
        // El anillo se reinicia con el nuevo timer en la siguiente vuelta del worker
        if (s_audio.csnd.running) {
            csnd_stop_channels();
            s_audio.csnd.running = false;
            s_audio.csnd.written = 0;
        }
    }
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
    s_audio.sample_rate = AUDIO_SAMPLE_RATE;
    s_audio.codec = AUDIO_CODEC_MP3;
    s_audio.aacd = AACInitDecoder();
    if (!s_audio.aacd) {
        snprintf(s_audio_init_error, sizeof(s_audio_init_error), "AACInitDecoder");
        audio_exit();
        return false;
    }

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
    if (s_audio.aacd) {
        AACFreeDecoder(s_audio.aacd);
        s_audio.aacd = NULL;
    }
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
    if (s_audio.aacd) AACFlushCodec(s_audio.aacd);
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
    return (uint32_t)((played * 1000ULL) / (u64)s_audio.sample_rate);
}

void audio_set_codec(AudioCodec codec) {
    LightLock_Lock(&s_audio.decode_lock);
    s_audio.codec = codec;
    LightLock_Unlock(&s_audio.decode_lock);
}

int audio_get_decode_load_percent(void) {
    return (int)(s_audio.decode_load * 100.0f + 0.5f);
}

AudioCodec audio_get_codec(void) {
    return s_audio.codec;
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

void audio_set_pcm_rate(int rate) {
    s_audio.pcm_rate = rate;
}

void audio_report_decode_load(float load) {
    s_audio.decode_load = s_audio.decode_load * 0.9f + load * 0.1f;
}
