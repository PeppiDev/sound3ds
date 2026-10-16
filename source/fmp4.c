#include "fmp4.h"
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static const int s_sample_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
};

static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t rd64(const uint8_t* p) {
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

typedef struct {
    const uint8_t* payload; // contenido tras la cabecera de la caja
    size_t payload_len;
    size_t start;           // desplazamiento de la caja dentro del buffer recorrido
    uint32_t type;
} Box;

// Lee la caja que empieza en *off dentro de [data, data+len) y avanza *off.
static bool next_box(const uint8_t* data, size_t len, size_t* off, Box* box) {
    if (*off + 8 > len) return false;
    const uint8_t* p = data + *off;
    uint64_t size = rd32(p);
    size_t hdr = 8;
    if (size == 1) {
        if (*off + 16 > len) return false;
        size = rd64(p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = len - *off;
    }
    if (size < hdr || size > len - *off) return false;

    box->payload = p + hdr;
    box->payload_len = (size_t)size - hdr;
    box->start = *off;
    box->type = rd32(p + 4);
    *off += (size_t)size;
    return true;
}

static bool find_child(const uint8_t* data, size_t len, uint32_t type, Box* out) {
    size_t off = 0;
    Box b;
    while (next_box(data, len, &off, &b)) {
        if (b.type == type) {
            *out = b;
            return true;
        }
    }
    return false;
}

// Longitud de un descriptor MPEG-4 (hasta 4 bytes de 7 bits)
static bool read_desc_header(const uint8_t** p, const uint8_t* end, uint8_t* tag, size_t* dlen) {
    if (*p >= end) return false;
    *tag = *(*p)++;
    size_t n = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        n = (n << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    if (n > (size_t)(end - *p)) return false;
    *dlen = n;
    return true;
}

// AudioSpecificConfig: tipo de objeto, frecuencia y canales
static bool parse_asc(const uint8_t* p, size_t len, Fmp4AudioConfig* cfg) {
    if (len < 2) return false;
    uint32_t bits = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    (len > 2 ? (uint32_t)p[2] << 8 : 0) | (len > 3 ? p[3] : 0);
    int pos = 0;
    #define TAKE(n) ((int)((bits << pos) >> (32 - (n)))); pos += (n)
    int aot = TAKE(5);
    if (aot == 31) { aot = 32 + TAKE(6); }
    int sfi = TAKE(4);
    if (sfi == 15) return false; // frecuencia explícita: ADTS no puede representarla
    int ch = TAKE(4);
    #undef TAKE
    if (sfi > 12 || ch == 0 || ch > 2) return false;

    cfg->object_type = aot;
    cfg->sample_rate_index = sfi;
    cfg->sample_rate = s_sample_rates[sfi];
    cfg->channels = ch;
    return true;
}

static bool parse_esds(const uint8_t* p, size_t len, Fmp4AudioConfig* cfg) {
    const uint8_t* end = p + len;
    if (len < 4) return false;
    p += 4; // versión + flags

    uint8_t tag;
    size_t dlen;
    if (!read_desc_header(&p, end, &tag, &dlen) || tag != 0x03) return false;
    const uint8_t* es_end = p + dlen;
    if (es_end - p < 3) return false;
    uint8_t flags = p[2];
    p += 3;
    if (flags & 0x80) p += 2;                       // streamDependence
    if ((flags & 0x40) && p < es_end) p += 1 + *p;  // URL
    if (flags & 0x20) p += 2;                       // OCR stream
    if (p > es_end) return false;

    if (!read_desc_header(&p, es_end, &tag, &dlen) || tag != 0x04 || dlen < 13) return false;
    const uint8_t* dc_end = p + dlen;
    p += 13; // objectType, streamType, bufferSize, max/avg bitrate

    if (!read_desc_header(&p, dc_end, &tag, &dlen) || tag != 0x05) return false;
    return parse_asc(p, dlen, cfg);
}

bool fmp4_parse_init(const uint8_t* data, size_t len, Fmp4AudioConfig* out) {
    memset(out, 0, sizeof(*out));

    Box moov;
    if (!find_child(data, len, FOURCC('m', 'o', 'o', 'v'), &moov)) return false;

    Box mvex, trex;
    if (find_child(moov.payload, moov.payload_len, FOURCC('m', 'v', 'e', 'x'), &mvex) &&
        find_child(mvex.payload, mvex.payload_len, FOURCC('t', 'r', 'e', 'x'), &trex) &&
        trex.payload_len >= 24) {
        out->default_sample_duration = rd32(trex.payload + 12);
        out->default_sample_size = rd32(trex.payload + 16);
    }

    // Primera pista con descripción de audio AAC
    size_t off = 0;
    Box trak;
    while (next_box(moov.payload, moov.payload_len, &off, &trak)) {
        if (trak.type != FOURCC('t', 'r', 'a', 'k')) continue;

        Box mdia, mdhd, minf, stbl, stsd;
        if (!find_child(trak.payload, trak.payload_len, FOURCC('m', 'd', 'i', 'a'), &mdia)) continue;
        if (find_child(mdia.payload, mdia.payload_len, FOURCC('m', 'd', 'h', 'd'), &mdhd) && mdhd.payload_len >= 24) {
            out->timescale = rd32(mdhd.payload + (mdhd.payload[0] == 1 ? 20 : 12));
        }
        if (!find_child(mdia.payload, mdia.payload_len, FOURCC('m', 'i', 'n', 'f'), &minf)) continue;
        if (!find_child(minf.payload, minf.payload_len, FOURCC('s', 't', 'b', 'l'), &stbl)) continue;
        if (!find_child(stbl.payload, stbl.payload_len, FOURCC('s', 't', 's', 'd'), &stsd)) continue;
        if (stsd.payload_len < 8) continue;

        // Entradas de muestra; 'enca' (cifrado) no se puede reproducir
        Box mp4a;
        if (!find_child(stsd.payload + 8, stsd.payload_len - 8, FOURCC('m', 'p', '4', 'a'), &mp4a)) continue;
        if (mp4a.payload_len < 28) continue;

        Box esds;
        if (!find_child(mp4a.payload + 28, mp4a.payload_len - 28, FOURCC('e', 's', 'd', 's'), &esds)) continue;
        if (parse_esds(esds.payload, esds.payload_len, out)) return true;
    }
    return false;
}

// Tamaños de muestra de un trun y desplazamiento de sus datos
static int walk_trun(const Box* trun, uint32_t default_size, const uint8_t* data, size_t len,
                     size_t* data_pos, size_t moof_start, Fmp4FrameCallback cb, void* user, bool* stop) {
    const uint8_t* p = trun->payload;
    const uint8_t* end = p + trun->payload_len;
    if (trun->payload_len < 8) return -1;
    uint32_t flags = rd32(p) & 0xFFFFFF;
    uint32_t count = rd32(p + 4);
    p += 8;

    if (flags & 0x1) {
        if (end - p < 4) return -1;
        int32_t data_offset = (int32_t)rd32(p);
        p += 4;
        if (data_offset < 0) return -1;
        *data_pos = moof_start + (size_t)data_offset;
    }
    if (flags & 0x4) p += 4;

    size_t per_sample = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) +
                        ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
    if (p > end || (per_sample && count > (size_t)(end - p) / per_sample)) return -1;

    int delivered = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = default_size;
        if (flags & 0x100) p += 4;
        if (flags & 0x200) { size = rd32(p); p += 4; }
        if (flags & 0x400) p += 4;
        if (flags & 0x800) p += 4;

        if (size == 0 || *data_pos > len || size > len - *data_pos) return -1;
        if (!*stop) {
            if (!cb(data + *data_pos, size, user)) *stop = true;
            else delivered++;
        }
        *data_pos += size;
    }
    return delivered;
}

int fmp4_parse_segment(const uint8_t* data, size_t len, const Fmp4AudioConfig* cfg,
                       Fmp4FrameCallback cb, void* user) {
    size_t off = 0;
    Box box;
    int total = 0;
    bool stop = false;

    while (!stop && next_box(data, len, &off, &box)) {
        if (box.type != FOURCC('m', 'o', 'o', 'f')) continue;
        size_t moof_start = box.start;

        // Si el trun no trae data_offset, los datos empiezan en el mdat siguiente
        size_t data_pos = 0;
        size_t peek = off;
        Box mdat;
        while (next_box(data, len, &peek, &mdat)) {
            if (mdat.type == FOURCC('m', 'd', 'a', 't')) {
                data_pos = (size_t)(mdat.payload - data);
                break;
            }
        }

        Box traf;
        if (!find_child(box.payload, box.payload_len, FOURCC('t', 'r', 'a', 'f'), &traf)) return -1;

        uint32_t default_size = cfg->default_sample_size;
        Box tfhd;
        if (find_child(traf.payload, traf.payload_len, FOURCC('t', 'f', 'h', 'd'), &tfhd) && tfhd.payload_len >= 8) {
            uint32_t tf = rd32(tfhd.payload) & 0xFFFFFF;
            const uint8_t* q = tfhd.payload + 8;
            const uint8_t* qend = tfhd.payload + tfhd.payload_len;
            if (tf & 0x1) {
                if (qend - q < 8) return -1;
                moof_start = (size_t)rd64(q);
                q += 8;
            }
            if (tf & 0x2) q += 4;
            if (tf & 0x8) q += 4;
            if ((tf & 0x10) && qend - q >= 4) default_size = rd32(q);
        }

        size_t toff = 0;
        Box trun;
        while (next_box(traf.payload, traf.payload_len, &toff, &trun)) {
            if (trun.type != FOURCC('t', 'r', 'u', 'n')) continue;
            int n = walk_trun(&trun, default_size, data, len, &data_pos, moof_start, cb, user, &stop);
            if (n < 0) return -1;
            total += n;
        }
    }
    return total;
}

void fmp4_adts_header(uint8_t out[7], const Fmp4AudioConfig* cfg, size_t frame_len) {
    // ADTS solo describe perfiles 1-4; HE-AAC (5, 29) lleva un núcleo AAC-LC
    int profile = (cfg->object_type >= 1 && cfg->object_type <= 4) ? cfg->object_type - 1 : 1;
    size_t flen = frame_len + 7;
    out[0] = 0xFF;
    out[1] = 0xF1; // MPEG-4, sin CRC
    out[2] = (uint8_t)((profile << 6) | (cfg->sample_rate_index << 2) | ((cfg->channels >> 2) & 1));
    out[3] = (uint8_t)(((cfg->channels & 3) << 6) | ((flen >> 11) & 3));
    out[4] = (uint8_t)((flen >> 3) & 0xFF);
    out[5] = (uint8_t)(((flen & 7) << 5) | 0x1F);
    out[6] = 0xFC;
}
