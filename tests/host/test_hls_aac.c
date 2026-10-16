// Prueba en el PC del camino HLS de Sound3DS: lista m3u8 -> init.mp4 -> segmentos
// fMP4 -> tramas AAC con cabecera ADTS -> Helix AAC -> PCM.
// Uso: test_hls_aac <dir> con playlist.m3u8, init.mp4 y seg0.m4s, seg1.m4s, ...
// Escribe <dir>/out.adts y <dir>/out.pcm (s16le estéreo) para compararlos con ffmpeg.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmp4.h"
#include "hls.h"
#include "aacdec.h"

static unsigned char* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

typedef struct {
    Fmp4AudioConfig cfg;
    FILE* adts;
    unsigned char* stream;
    size_t stream_len, stream_cap;
    int frames;
} Ctx;

static bool on_frame(const uint8_t* frame, size_t size, void* user) {
    Ctx* c = user;
    uint8_t hdr[7];
    fmp4_adts_header(hdr, &c->cfg, size);
    if (c->stream_len + size + 7 > c->stream_cap) {
        c->stream_cap = (c->stream_len + size + 7) * 2;
        c->stream = realloc(c->stream, c->stream_cap);
    }
    memcpy(c->stream + c->stream_len, hdr, 7);
    memcpy(c->stream + c->stream_len + 7, frame, size);
    c->stream_len += size + 7;
    fwrite(hdr, 1, 7, c->adts);
    fwrite(frame, 1, size, c->adts);
    c->frames++;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <dir>\n", argv[0]); return 2; }
    char path[1024];
    size_t len;

    snprintf(path, sizeof(path), "%s/playlist.m3u8", argv[1]);
    char* text = (char*)read_file(path, &len);
    HlsPlaylist pl;
    if (!text || !hls_parse(text, "https://example.com/a/b/playlist.m3u8", &pl)) { fprintf(stderr, "FALLO hls_parse\n"); return 1; }
    printf("lista: %d segmentos, %u ms, init=%s\n", pl.count, pl.total_ms, pl.init_uri ? "si" : "no");
    printf("segmento en 25 s: %d\n", hls_segment_at(&pl, 25000));

    Ctx c = {0};
    snprintf(path, sizeof(path), "%s/init.mp4", argv[1]);
    unsigned char* init = read_file(path, &len);
    if (!init || !fmp4_parse_init(init, len, &c.cfg)) { fprintf(stderr, "FALLO fmp4_parse_init\n"); return 1; }
    printf("init: aot=%d rate=%d ch=%d timescale=%u\n", c.cfg.object_type, c.cfg.sample_rate, c.cfg.channels, c.cfg.timescale);

    snprintf(path, sizeof(path), "%s/out.adts", argv[1]);
    c.adts = fopen(path, "wb");
    for (int i = 0;; i++) {
        snprintf(path, sizeof(path), "%s/seg%d.m4s", argv[1], i);
        unsigned char* seg = read_file(path, &len);
        if (!seg) break;
        int n = fmp4_parse_segment(seg, len, &c.cfg, on_frame, &c);
        printf("seg%d: %d tramas\n", i, n);
        if (n <= 0) { fprintf(stderr, "FALLO fmp4_parse_segment\n"); return 1; }
        free(seg);
    }
    fclose(c.adts);

    // Decodificar el flujo ADTS como lo hará el hilo de audio
    HAACDecoder dec = AACInitDecoder();
    snprintf(path, sizeof(path), "%s/out.pcm", argv[1]);
    FILE* pcm = fopen(path, "wb");
    short out[2048 * 2];
    unsigned char* p = c.stream;
    int left = (int)c.stream_len, decoded = 0, errors = 0;
    while (left > 0) {
        int sync = AACFindSyncWord(p, left);
        if (sync < 0) break;
        p += sync; left -= sync;
        int err = AACDecode(dec, &p, &left, out);
        if (err) { errors++; if (left > 0) { p++; left--; } continue; }
        AACFrameInfo fi;
        AACGetLastFrameInfo(dec, &fi);
        fwrite(out, sizeof(short), (size_t)fi.outputSamps, pcm);
        decoded++;
    }
    fclose(pcm);
    AACFreeDecoder(dec);
    printf("tramas: %d demuxeadas, %d decodificadas, %d errores\n", c.frames, decoded, errors);
    hls_free(&pl);
    return (decoded == c.frames && errors == 0) ? 0 : 1;
}
