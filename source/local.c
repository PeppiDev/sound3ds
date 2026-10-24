#include "local.h"
#include "audio.h"
#include "debuglog.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_STDIO_WIDE
#define DR_FLAC_NO_CRC          // la tarjeta SD no corrompe datos y el CRC cuesta CPU en el Old 3DS
#define DR_FLAC_BUFFER_SIZE (16 * 1024)
#include "dr_flac.h"

#define LOCAL_THREAD_STACK_SZ (16 * 1024)
#define FLAC_THREAD_STACK_SZ  (64 * 1024)  // dr_flac decodifica en este hilo
#define LOCAL_READ_CHUNK      (16 * 1024)
#define LOCAL_MAX_DEPTH       3
#define FLAC_READ_FRAMES      1024   // tramas PCM por lectura (múltiplo de 8 para el diezmado)
#define FLAC_MAX_OUT_RATE     48000  // por encima se divide la frecuencia por 2, 4 u 8

static const char* const s_music_dirs[] = {
    "sdmc:/music",
    "sdmc:/3ds/sound3ds/music",
};

static struct {
    Thread thread;
    volatile bool streaming;
    volatile bool stop;
    char path[256];
    float ratio;
    char error[64];
    bool core1;              // el hilo FLAC corre en el segundo núcleo
} s_local;

// ---------------------------------------------------------------------------
// Etiquetas ID3v2 (título y artista)
// ---------------------------------------------------------------------------

static size_t syncsafe(const uint8_t* p) {
    return ((size_t)(p[0] & 0x7F) << 21) | ((size_t)(p[1] & 0x7F) << 14) | ((size_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// Escribe un código Unicode como UTF-8; devuelve los bytes usados (0 si no cabe)
static size_t put_utf8(char* out, size_t room, uint32_t cp) {
    if (cp < 0x80 && room > 1) { out[0] = (char)cp; return 1; }
    if (cp < 0x800 && room > 2) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && room > 3) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

// Texto de un marco ID3 (byte de codificación + contenido) a UTF-8
static void id3_text(const uint8_t* p, size_t len, char* out, size_t out_sz) {
    size_t o = 0;
    if (len == 0 || out_sz == 0) return;
    uint8_t enc = p[0];
    p++;
    len--;

    if (enc == 0 || enc == 3) {
        // ISO-8859-1 (cada byte es su código) o UTF-8 tal cual
        for (size_t i = 0; i < len && p[i]; i++) {
            if (enc == 3) {
                if (o + 1 >= out_sz) break;
                out[o++] = (char)p[i];
            } else {
                size_t n = put_utf8(out + o, out_sz - o, p[i]);
                if (!n) break;
                o += n;
            }
        }
    } else {
        // UTF-16 con BOM (1) o big endian (2); solo plano básico
        bool big = (enc == 2);
        size_t i = 0;
        if (enc == 1 && len >= 2) {
            big = (p[0] == 0xFE && p[1] == 0xFF);
            i = 2;
        }
        for (; i + 1 < len; i += 2) {
            uint32_t cp = big ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
            if (cp == 0) break;
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            size_t n = put_utf8(out + o, out_sz - o, cp);
            if (!n) break;
            o += n;
        }
    }
    out[o] = '\0';
}

// Lee un marco de texto pequeño del archivo y lo convierte a UTF-8
static void read_text_frame(FILE* f, size_t fsize, char* out, size_t out_sz) {
    uint8_t buf[512];
    size_t n = fsize < sizeof(buf) ? fsize : sizeof(buf);
    if (fread(buf, 1, n, f) == n) id3_text(buf, n, out, out_sz);
}

// Recorre la etiqueta ID3v2 (2.2, 2.3 o 2.4) leyendo solo las cabeceras de los marcos:
// textos que interesan y la posición de la carátula, sin cargar la imagen
static void read_tags(FILE* f, LocalTrack* t, char* title, size_t title_sz, char* artist, size_t artist_sz) {
    uint8_t hdr[10];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) return;
    int ver = hdr[3];
    if (ver < 2 || ver > 4) return;
    long end = 10 + (long)syncsafe(hdr + 6);
    long pos = 10;

    // Cabecera extendida: se salta
    if ((hdr[5] & 0x40) && ver >= 3) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) return;
        size_t ext_size = ver == 4 ? syncsafe(ext) : ((size_t)ext[0] << 24 | (size_t)ext[1] << 16 | ext[2] << 8 | ext[3]) + 4;
        pos += (long)ext_size;
    }

    size_t id_len = (ver == 2) ? 3 : 4, hdr_len = (ver == 2) ? 6 : 10;
    while (pos + (long)hdr_len <= end) {
        uint8_t fr[10];
        fseek(f, pos, SEEK_SET);
        if (fread(fr, 1, hdr_len, f) != hdr_len || fr[0] == 0) break;
        size_t fsize = (ver == 2) ? ((size_t)fr[3] << 16 | fr[4] << 8 | fr[5])
                     : (ver == 4) ? syncsafe(fr + 4)
                     : ((size_t)fr[4] << 24 | (size_t)fr[5] << 16 | fr[6] << 8 | fr[7]);
        long data = pos + (long)hdr_len;
        if (fsize == 0 || data + (long)fsize > end) break;

#define IS(id4, id3) ((id_len == 4 && memcmp(fr, id4, 4) == 0) || (id_len == 3 && memcmp(fr, id3, 3) == 0))
        char num[16];
        if (IS("TIT2", "TT2")) read_text_frame(f, fsize, title, title_sz);
        else if (IS("TPE1", "TP1")) read_text_frame(f, fsize, artist, artist_sz);
        else if (IS("TALB", "TAL")) read_text_frame(f, fsize, t->album, sizeof(t->album));
        else if (IS("TPE2", "TP2")) read_text_frame(f, fsize, t->album_artist, sizeof(t->album_artist));
        else if (IS("TRCK", "TRK")) { num[0] = 0; read_text_frame(f, fsize, num, sizeof(num)); t->track_no = atoi(num); }
        else if (IS("TLEN", "TLE")) { num[0] = 0; read_text_frame(f, fsize, num, sizeof(num)); t->duration_ms = (uint32_t)strtoul(num, NULL, 10); }
        else if (IS("APIC", "PIC") && t->cover_len == 0) {
            // Encabezado de la imagen: codificación, tipo MIME (o formato de 3 letras en 2.2),
            // tipo de imagen y descripción; después vienen los bytes de la imagen
            uint8_t head[256];
            size_t hn = fsize < sizeof(head) ? fsize : sizeof(head);
            if (fread(head, 1, hn, f) == hn && hn > 4) {
                uint8_t enc = head[0];
                size_t i = 1;
                if (ver == 2) i += 3;
                else { while (i < hn && head[i]) i++; i++; }
                i++; // tipo de imagen
                if (enc == 1 || enc == 2) { while (i + 1 < hn && (head[i] || head[i + 1])) i += 2; i += 2; }
                else { while (i < hn && head[i]) i++; i++; }
                if (i < hn && i < fsize) {
                    t->cover_off = (uint32_t)(data + (long)i);
                    t->cover_len = (uint32_t)(fsize - i);
                }
            }
        }
#undef IS
        pos = data + (long)fsize;
    }
}

uint8_t* local_read_cover(const LocalTrack* t, size_t* out_len) {
    if (!t || t->cover_len == 0 || t->cover_len > 4 * 1024 * 1024) return NULL;
    FILE* f = fopen(t->path, "rb");
    if (!f) return NULL;
    uint8_t* buf = (uint8_t*)malloc(t->cover_len);
    bool ok = buf && fseek(f, (long)t->cover_off, SEEK_SET) == 0 && fread(buf, 1, t->cover_len, f) == t->cover_len;
    fclose(f);
    if (!ok) {
        free(buf);
        return NULL;
    }
    if (out_len) *out_len = t->cover_len;
    return buf;
}

// ---------------------------------------------------------------------------
// Escritura de etiquetas (descargas)
// ---------------------------------------------------------------------------

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// Marco de texto ID3v2.3 en UTF-16 con BOM (la versión 2.3 no admite UTF-8)
static size_t frame_text(uint8_t* out, size_t room, const char* id, const char* utf8) {
    if (!utf8 || !utf8[0]) return 0;
    uint8_t body[600];
    size_t n = 0;
    body[n++] = 1;      // UTF-16
    body[n++] = 0xFF;   // BOM little endian
    body[n++] = 0xFE;
    const uint8_t* p = (const uint8_t*)utf8;
    while (*p && n + 2 < sizeof(body) - 2) {
        uint32_t cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : (cp >= 0xE0 ? 2 : 1);
            cp &= (0x3F >> extra);
            for (int k = 0; k < extra && (*p & 0xC0) == 0x80; k++) cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp > 0xFFFF) cp = '?';
        body[n++] = (uint8_t)cp;
        body[n++] = (uint8_t)(cp >> 8);
    }
    if (10 + n > room) return 0;
    memcpy(out, id, 4);
    put_be32(out + 4, (uint32_t)n);
    out[8] = out[9] = 0;
    memcpy(out + 10, body, n);
    return 10 + n;
}

bool local_write_id3(FILE* f, const LocalTagInfo* info) {
    size_t cap = 4096 + (info->jpeg ? info->jpeg_len + 32 : 0);
    uint8_t* tag = (uint8_t*)malloc(cap);
    if (!tag) return false;
    size_t n = 10;
    char num[24];
    n += frame_text(tag + n, cap - n, "TIT2", info->title);
    n += frame_text(tag + n, cap - n, "TPE1", info->artist);
    n += frame_text(tag + n, cap - n, "TALB", info->album);
    n += frame_text(tag + n, cap - n, "TPE2", info->album_artist);
    if (info->track_no > 0) {
        snprintf(num, sizeof(num), "%d", info->track_no);
        n += frame_text(tag + n, cap - n, "TRCK", num);
    }
    if (info->duration_ms > 0) {
        snprintf(num, sizeof(num), "%lu", (unsigned long)info->duration_ms);
        n += frame_text(tag + n, cap - n, "TLEN", num);
    }
    if (info->sc_id && info->sc_id[0]) {
        // TXXX: codificación latin-1, descripción "SOUNDCLOUD_ID" y el número como valor.
        // Sirve para reconocer exactamente la misma pista aunque haya versiones con el mismo nombre.
        static const char desc[] = LOCAL_SC_ID_DESC;
        size_t vlen = strlen(info->sc_id);
        size_t body = 1 + sizeof(desc) + vlen;
        if (n + 10 + body < cap) {
            memcpy(tag + n, "TXXX", 4);
            put_be32(tag + n + 4, (uint32_t)body);
            tag[n + 8] = tag[n + 9] = 0;
            size_t o = n + 10;
            tag[o++] = 0;
            memcpy(tag + o, desc, sizeof(desc));
            o += sizeof(desc);
            memcpy(tag + o, info->sc_id, vlen);
            n = o + vlen;
        }
    }
    if (info->jpeg && info->jpeg_len > 0) {
        // APIC: codificación latin-1, "image/jpeg", portada (3), descripción vacía
        static const char mime[] = "image/jpeg";
        size_t body = 1 + sizeof(mime) + 1 + 1 + info->jpeg_len;
        memcpy(tag + n, "APIC", 4);
        put_be32(tag + n + 4, (uint32_t)body);
        tag[n + 8] = tag[n + 9] = 0;
        size_t o = n + 10;
        tag[o++] = 0;
        memcpy(tag + o, mime, sizeof(mime));
        o += sizeof(mime);
        tag[o++] = 3;
        tag[o++] = 0;
        memcpy(tag + o, info->jpeg, info->jpeg_len);
        n = o + info->jpeg_len;
    }
    // Cabecera: versión 2.3, tamaño en formato syncsafe
    size_t size = n - 10;
    memcpy(tag, "ID3", 3);
    tag[3] = 3;
    tag[4] = 0;
    tag[5] = 0;
    tag[6] = (uint8_t)((size >> 21) & 0x7F);
    tag[7] = (uint8_t)((size >> 14) & 0x7F);
    tag[8] = (uint8_t)((size >> 7) & 0x7F);
    tag[9] = (uint8_t)(size & 0x7F);
    bool ok = fwrite(tag, 1, n, f) == n;
    free(tag);
    return ok;
}

bool local_read_sc_id(const char* path, char* out, size_t out_sz) {
    if (out_sz) out[0] = '\0';
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[10];
    bool found = false;
    // Solo ID3v2.3 (lo que escriben las descargas); los marcos se recorren sin leer la carátula
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0 && hdr[3] == 3) {
        long end = 10 + (long)syncsafe(hdr + 6), pos = 10;
        while (!found && pos + 10 <= end) {
            uint8_t fr[10];
            fseek(f, pos, SEEK_SET);
            if (fread(fr, 1, 10, f) != 10 || fr[0] == 0) break;
            size_t fsize = (size_t)fr[4] << 24 | (size_t)fr[5] << 16 | fr[6] << 8 | fr[7];
            if (fsize == 0 || pos + 10 + (long)fsize > end) break;
            if (memcmp(fr, "TXXX", 4) == 0 && fsize < 128) {
                char body[128];
                static const char desc[] = LOCAL_SC_ID_DESC;
                if (fread(body, 1, fsize, f) == fsize && body[0] == 0 && fsize > sizeof(desc) &&
                    memcmp(body + 1, desc, sizeof(desc)) == 0) {
                    size_t vlen = fsize - 1 - sizeof(desc);
                    if (vlen >= out_sz) vlen = out_sz - 1;
                    memcpy(out, body + 1 + sizeof(desc), vlen);
                    out[vlen] = '\0';
                    found = vlen > 0;
                }
            }
            pos += 10 + (long)fsize;
        }
    }
    fclose(f);
    return found;
}

// ---------------------------------------------------------------------------
// FLAC: bloques de metadatos tras "fLaC" (STREAMINFO, VORBIS_COMMENT y PICTURE)
// ---------------------------------------------------------------------------

static uint32_t rd_le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// Deja f justo después de "fLaC" (saltando una etiqueta ID3 delante, que algunos programas ponen)
static bool flac_seek_marker(FILE* f) {
    uint8_t hdr[10];
    long pos = 0;
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) pos = 10 + (long)syncsafe(hdr + 6);
    fseek(f, pos, SEEK_SET);
    return fread(hdr, 1, 4, f) == 4 && memcmp(hdr, "fLaC", 4) == 0;
}

// Comentario Vorbis "CLAVE=valor": copia el valor si la clave coincide (sin distinguir mayúsculas)
static bool vorbis_field(const char* entry, const char* key, char* out, size_t out_sz) {
    size_t k = strlen(key);
    if (strncasecmp(entry, key, k) != 0 || entry[k] != '=') return false;
    snprintf(out, out_sz, "%s", entry + k + 1);
    return true;
}

static void read_vorbis_comments(FILE* f, long end, LocalTrack* t, char* title, size_t title_sz, char* artist,
                                 size_t artist_sz) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return;
    fseek(f, (long)rd_le32(b), SEEK_CUR); // vendor
    if (fread(b, 1, 4, f) != 4) return;
    uint32_t count = rd_le32(b);
    char entry[256];
    for (uint32_t i = 0; i < count && ftell(f) + 4 <= end; i++) {
        if (fread(b, 1, 4, f) != 4) return;
        uint32_t len = rd_le32(b);
        if (ftell(f) + (long)len > end) return;
        if (len >= sizeof(entry)) { // carátulas en base64 u otros campos largos: se saltan
            fseek(f, (long)len, SEEK_CUR);
            continue;
        }
        if (fread(entry, 1, len, f) != len) return;
        entry[len] = '\0';
        char num[16];
        if (vorbis_field(entry, "TITLE", title, title_sz) || vorbis_field(entry, "ARTIST", artist, artist_sz) ||
            vorbis_field(entry, "ALBUM", t->album, sizeof(t->album)) ||
            vorbis_field(entry, "ALBUMARTIST", t->album_artist, sizeof(t->album_artist)) ||
            vorbis_field(entry, "ALBUM ARTIST", t->album_artist, sizeof(t->album_artist)))
            continue;
        if (vorbis_field(entry, "TRACKNUMBER", num, sizeof(num))) t->track_no = atoi(num);
    }
}

// Datos de STREAMINFO: frecuencia y total de muestras. false si no es un FLAC válido.
static bool flac_streaminfo(FILE* f, uint32_t* rate, uint64_t* total) {
    uint8_t blk[4 + 34];
    if (!flac_seek_marker(f) || fread(blk, 1, sizeof(blk), f) != sizeof(blk) || (blk[0] & 0x7F) != 0) return false;
    const uint8_t* si = blk + 4;
    *rate = (uint32_t)si[10] << 12 | (uint32_t)si[11] << 4 | si[12] >> 4;
    *total = (uint64_t)(si[13] & 0x0F) << 32 | rd_be32(si + 14);
    return *rate > 0;
}

static void read_flac_tags(FILE* f, LocalTrack* t, char* title, size_t title_sz, char* artist, size_t artist_sz) {
    uint32_t rate;
    uint64_t total;
    if (flac_streaminfo(f, &rate, &total) && total > 0) t->duration_ms = (uint32_t)(total * 1000 / rate);

    flac_seek_marker(f);
    uint32_t best_type = 0xFFFFFFFF;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t h[4];
        if (fread(h, 1, 4, f) != 4) return;
        bool last = h[0] & 0x80;
        int type = h[0] & 0x7F;
        long start = ftell(f);
        long end = start + (long)((uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3]);
        if (end > (long)t->file_size) return;

        if (type == 4) {
            read_vorbis_comments(f, end, t, title, title_sz, artist, artist_sz);
        } else if (type == 6 && best_type != 3) {
            // PICTURE: tipo, MIME, descripción, 16 bytes de medidas y la imagen. La portada (3) manda.
            uint8_t b[4];
            uint32_t pic_type = 0, len = 0;
            bool ok = fread(b, 1, 4, f) == 4 && ((pic_type = rd_be32(b)), true) && fread(b, 1, 4, f) == 4 &&
                      fseek(f, (long)rd_be32(b), SEEK_CUR) == 0 && fread(b, 1, 4, f) == 4 &&
                      fseek(f, (long)rd_be32(b) + 16, SEEK_CUR) == 0 && fread(b, 1, 4, f) == 4;
            if (ok) {
                len = rd_be32(b);
                if (len > 0 && ftell(f) + (long)len <= end && (t->cover_len == 0 || pic_type == 3)) {
                    t->cover_off = (uint32_t)ftell(f);
                    t->cover_len = len;
                    best_type = pic_type;
                }
            }
        }
        if (last) return;
        fseek(f, end, SEEK_SET);
    }
}

// ---------------------------------------------------------------------------
// Escaneo de carpetas
// ---------------------------------------------------------------------------

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_audio_file(const char* name) {
    return has_ext(name, ".mp3") || has_ext(name, ".aac") || has_ext(name, ".flac");
}

static void fill_track(LocalTrack* t, const char* full, const char* dir, const char* name) {
    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", full);
    t->is_aac = has_ext(name, ".aac");
    t->is_flac = has_ext(name, ".flac");

    // Por defecto: "Artista - Título.mp3", o el nombre y la carpeta
    char base[128];
    snprintf(base, sizeof(base), "%s", name);
    char* dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    const char* dash = strstr(base, " - ");
    if (dash) {
        snprintf(t->artist, sizeof(t->artist), "%.*s", (int)(dash - base), base);
        snprintf(t->title, sizeof(t->title), "%s", dash + 3);
    } else {
        snprintf(t->title, sizeof(t->title), "%s", base);
        const char* slash = strrchr(dir, '/');
        snprintf(t->artist, sizeof(t->artist), "%s", slash ? slash + 1 : dir);
    }

    // La etiqueta ID3 manda si trae datos
    FILE* f = fopen(t->path, "rb");
    if (f) {
        char title[128] = "", artist[64] = "";
        fseek(f, 0, SEEK_END);
        t->file_size = (uint32_t)ftell(f);
        if (t->is_flac) read_flac_tags(f, t, title, sizeof(title), artist, sizeof(artist));
        else read_tags(f, t, title, sizeof(title), artist, sizeof(artist));
        fclose(f);
        if (title[0]) snprintf(t->title, sizeof(t->title), "%s", title);
        if (artist[0]) snprintf(t->artist, sizeof(t->artist), "%s", artist);
    }
}

static void scan_dir(const char* dir, int depth, LocalTrack* out, int max, int* count) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while (*count < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (strlen(full) >= sizeof(((LocalTrack*)0)->path)) continue; // ruta demasiado larga para guardarla

        bool is_dir = (e->d_type == DT_DIR);
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            if (depth < LOCAL_MAX_DEPTH) scan_dir(full, depth + 1, out, max, count);
        } else if (is_audio_file(e->d_name)) {
            fill_track(&out[(*count)++], full, dir, e->d_name);
        }
    }
    closedir(d);
}

// Orden de los álbumes: SoundCloud primero, luego alfabético, y las pistas sin álbum al final
static int album_rank(const LocalTrack* t) {
    if (!t->album[0]) return 2;
    return strcasecmp(t->album, LOCAL_SC_ALBUM) == 0 ? 0 : 1;
}

static int cmp_tracks(const void* a, const void* b) {
    const LocalTrack* ta = (const LocalTrack*)a;
    const LocalTrack* tb = (const LocalTrack*)b;
    int c = album_rank(ta) - album_rank(tb);
    if (c) return c;
    c = strcasecmp(ta->album, tb->album);
    if (c) return c;
    // Dentro del álbum: por número de pista (las que no tienen, al final) y título
    int na = ta->track_no > 0 ? ta->track_no : 100000, nb = tb->track_no > 0 ? tb->track_no : 100000;
    if (na != nb) return na < nb ? -1 : 1;
    c = strcasecmp(ta->title, tb->title);
    return c ? c : strcasecmp(ta->artist, tb->artist);
}

// ---------------------------------------------------------------------------
// Índice de la biblioteca: las etiquetas ya leídas quedan en la SD y solo se leen
// las canciones nuevas o cambiadas (otro tamaño). Las carpetas se recorren con la
// API de archivos de la consola, que entrega el tamaño sin abrir cada archivo.
// ---------------------------------------------------------------------------

#define INDEX_PATH    "sdmc:/3ds/sound3ds/library.idx"
#define INDEX_MAGIC   0x494C3353u   // "S3LI"
#define INDEX_VERSION 2

typedef struct {
    u32 magic, version, rec_size, count;
} IndexHeader;

static volatile int s_scan_found = 0;   // archivos de audio encontrados
static volatile int s_scan_read = 0;    // de esos, leídos de nuevo (no estaban en el índice)

void local_scan_progress(int* found, int* read_new) {
    if (found) *found = s_scan_found;
    if (read_new) *read_new = s_scan_read;
}

static u32 path_hash(const char* p) {
    u32 h = 2166136261u;
    while (*p) h = (h ^ (unsigned char)*p++) * 16777619u;
    return h;
}

typedef struct {
    LocalTrack* recs;
    int count;
    int* slots;      // tabla hash de índices en recs (-1 = vacío)
    int nslots;
} TrackIndex;

static void index_load(TrackIndex* ix) {
    memset(ix, 0, sizeof(*ix));
    FILE* f = fopen(INDEX_PATH, "rb");
    if (!f) return;
    IndexHeader h;
    if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == INDEX_MAGIC && h.version == INDEX_VERSION &&
        h.rec_size == sizeof(LocalTrack) && h.count > 0 && h.count <= LOCAL_MAX_TRACKS) {
        ix->recs = (LocalTrack*)malloc(h.count * sizeof(LocalTrack));
        if (ix->recs && fread(ix->recs, sizeof(LocalTrack), h.count, f) == h.count) ix->count = (int)h.count;
    }
    fclose(f);
    if (ix->count == 0) {
        free(ix->recs);
        ix->recs = NULL;
        return;
    }
    ix->nslots = 1;
    while (ix->nslots < ix->count * 2) ix->nslots <<= 1;
    ix->slots = (int*)malloc(ix->nslots * sizeof(int));
    if (!ix->slots) {
        free(ix->recs);
        memset(ix, 0, sizeof(*ix));
        return;
    }
    for (int i = 0; i < ix->nslots; i++) ix->slots[i] = -1;
    for (int i = 0; i < ix->count; i++) {
        ix->recs[i].path[sizeof(ix->recs[i].path) - 1] = '\0';
        u32 k = path_hash(ix->recs[i].path) & (ix->nslots - 1);
        while (ix->slots[k] >= 0) k = (k + 1) & (ix->nslots - 1);
        ix->slots[k] = i;
    }
}

static const LocalTrack* index_find(const TrackIndex* ix, const char* path) {
    if (!ix->slots) return NULL;
    u32 k = path_hash(path) & (ix->nslots - 1);
    while (ix->slots[k] >= 0) {
        const LocalTrack* t = &ix->recs[ix->slots[k]];
        if (strcmp(t->path, path) == 0) return t;
        k = (k + 1) & (ix->nslots - 1);
    }
    return NULL;
}

static void index_free(TrackIndex* ix) {
    free(ix->recs);
    free(ix->slots);
    memset(ix, 0, sizeof(*ix));
}

static void index_save(const LocalTrack* tracks, int count) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    FILE* f = fopen(INDEX_PATH ".tmp", "wb");
    if (!f) return;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    IndexHeader h = { INDEX_MAGIC, INDEX_VERSION, sizeof(LocalTrack), (u32)count };
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && (count == 0 || fwrite(tracks, sizeof(LocalTrack), count, f) == (size_t)count);
    ok = fclose(f) == 0 && ok;
    // Primero a un temporal y luego se renombra: un corte nunca deja el índice a medias
    if (ok) {
        remove(INDEX_PATH);
        rename(INDEX_PATH ".tmp", INDEX_PATH);
    } else {
        remove(INDEX_PATH ".tmp");
    }
}

typedef struct {
    FS_Archive arch;
    TrackIndex ix;
    LocalTrack* out;
    int max, count;
    bool changed;    // hubo canciones nuevas o cambiadas
} ScanCtx;

// Una canción encontrada: del índice si está igual, si no se leen sus etiquetas
static void scan_add(ScanCtx* c, const char* full, const char* dir, const char* name, u64 size) {
    LocalTrack* t = &c->out[c->count];
    const LocalTrack* old = index_find(&c->ix, full);
    if (old && old->file_size == (u32)size) {
        *t = *old;
    } else {
        fill_track(t, full, dir, name);
        c->changed = true;
        s_scan_read++;
    }
    c->count++;
    s_scan_found = c->count;
}

// Recorre una carpeta con la API de archivos (sdmc:/x/y -> /x/y en la SD)
static bool scan_dir_fs(ScanCtx* c, const char* dir, int depth) {
    const char* rel = strncmp(dir, "sdmc:", 5) == 0 ? dir + 5 : dir;
    static u16 wpath[512];
    ssize_t n = utf8_to_utf16(wpath, (const uint8_t*)rel, 511);
    if (n < 0) return false;
    wpath[n] = 0;
    Handle h;
    if (R_FAILED(FSUSER_OpenDirectory(&h, c->arch, fsMakePath(PATH_UTF16, wpath)))) return false;
    // Primero se juntan las entradas (la ruta en UTF-16 se reutiliza al bajar a subcarpetas)
    FS_DirectoryEntry* ents = (FS_DirectoryEntry*)malloc(32 * sizeof(FS_DirectoryEntry));
    char (*subdirs)[256] = NULL;
    int nsub = 0, capsub = 0;
    while (ents && c->count < c->max) {
        u32 got = 0;
        if (R_FAILED(FSDIR_Read(h, &got, 32, ents)) || got == 0) break;
        for (u32 i = 0; i < got && c->count < c->max; i++) {
            char name[262];
            ssize_t len = utf16_to_utf8((uint8_t*)name, ents[i].name, sizeof(name) - 1);
            if (len <= 0) continue;
            name[len] = '\0';
            if (name[0] == '.') continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", dir, name);
            if (strlen(full) >= sizeof(((LocalTrack*)0)->path)) continue;   // ruta demasiado larga para guardarla
            if (ents[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
                if (depth >= LOCAL_MAX_DEPTH) continue;
                if (nsub == capsub) {
                    capsub = capsub ? capsub * 2 : 8;
                    char (*g)[256] = realloc(subdirs, capsub * sizeof(*subdirs));
                    if (!g) continue;
                    subdirs = g;
                }
                snprintf(subdirs[nsub++], 256, "%s", full);
            } else if (is_audio_file(name)) {
                scan_add(c, full, dir, name, ents[i].fileSize);
            }
        }
    }
    free(ents);
    FSDIR_Close(h);
    for (int i = 0; i < nsub && c->count < c->max; i++) scan_dir_fs(c, subdirs[i], depth + 1);
    free(subdirs);
    return true;
}

int local_scan(LocalTrack* out, int max) {
    u64 t0 = osGetTime();
    ScanCtx c = { .out = out, .max = max };
    s_scan_found = s_scan_read = 0;
    index_load(&c.ix);
    int indexed = c.ix.count;
    bool fs_ok = R_SUCCEEDED(FSUSER_OpenArchive(&c.arch, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
    for (size_t i = 0; i < sizeof(s_music_dirs) / sizeof(s_music_dirs[0]); i++) {
        if (!fs_ok) {
            // Sin la API de archivos: recorrido normal leyendo cada canción
            scan_dir(s_music_dirs[i], 0, out, max, &c.count);
            c.changed = true;
            continue;
        }
        scan_dir_fs(&c, s_music_dirs[i], 0);
    }
    if (fs_ok) FSUSER_CloseArchive(c.arch);
    index_free(&c.ix);
    qsort(out, (size_t)c.count, sizeof(LocalTrack), cmp_tracks);
    // Se guarda si cambió algo (canciones nuevas, cambiadas o borradas)
    if (c.changed || c.count != indexed) index_save(out, c.count);
    dlog("biblioteca: %d canciones (%d leidas de nuevo, %d en el indice) en %lu ms", c.count, s_scan_read, indexed,
         (unsigned long)(osGetTime() - t0));
    return c.count;
}

// ---------------------------------------------------------------------------
// Duración: cabecera de la primera trama MP3
// ---------------------------------------------------------------------------

static const int s_bitrates_v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int s_bitrates_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const int s_rates[3] = {44100, 48000, 32000};

// Desplazamiento donde empieza el audio (tras la etiqueta ID3v2)
static long audio_start(FILE* f) {
    uint8_t hdr[10];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        return 10 + (long)syncsafe(hdr + 6) + ((hdr[5] & 0x10) ? 10 : 0);
    }
    return 0;
}

uint32_t local_estimate_duration_ms(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (has_ext(path, ".flac")) {
        uint32_t rate;
        uint64_t total;
        bool ok = flac_streaminfo(f, &rate, &total);
        fclose(f);
        return ok ? (uint32_t)(total * 1000 / rate) : 0;
    }
    if (has_ext(path, ".aac")) {
        // ADTS sin TLEN: se asume ~128 kbps (las descargas siempre traen TLEN)
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        long size = end - audio_start(f);
        fclose(f);
        return size > 0 ? (uint32_t)((uint64_t)size * 8 / 128) : 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = audio_start(f);

    uint8_t buf[4096];
    fseek(f, start, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 4 < n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        int version = (buf[i + 1] >> 3) & 3;   // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
        int layer = (buf[i + 1] >> 1) & 3;     // 1 = Layer III
        int br_idx = buf[i + 2] >> 4, sr_idx = (buf[i + 2] >> 2) & 3;
        if (version == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) continue;

        bool mpeg1 = (version == 3);
        int rate = s_rates[sr_idx] >> (mpeg1 ? 0 : (version == 2 ? 1 : 2));
        int kbps = mpeg1 ? s_bitrates_v1[br_idx] : s_bitrates_v2[br_idx];
        bool mono = ((buf[i + 3] >> 6) & 3) == 3;
        int samples_per_frame = mpeg1 ? 1152 : 576;

        // VBR: la cabecera Xing/Info dice cuántas tramas hay
        size_t xing = i + 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        if (xing + 12 <= n && (memcmp(buf + xing, "Xing", 4) == 0 || memcmp(buf + xing, "Info", 4) == 0) &&
            (buf[xing + 7] & 1)) {
            uint32_t frames = (uint32_t)buf[xing + 8] << 24 | buf[xing + 9] << 16 | buf[xing + 10] << 8 | buf[xing + 11];
            return (uint32_t)((uint64_t)frames * samples_per_frame * 1000 / rate);
        }
        // CBR (aproximado)
        return (uint32_t)((uint64_t)(size - start) * 8 / kbps);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Lectura en segundo plano
// ---------------------------------------------------------------------------

// FLAC: se decodifica aquí con dr_flac y al ring buffer va PCM estéreo de 16 bits.
// Mono se duplica, más de 2 canales se quedan en los 2 primeros y sobre 48 kHz se
// promedian grupos de 2, 4 u 8 muestras (la consola sale a ~32,7 kHz de todos modos).
static void local_worker_flac(void) {
    drflac* fl = drflac_open_file(s_local.path, NULL);
    if (!fl) {
        snprintf(s_local.error, sizeof(s_local.error), "No se pudo abrir el FLAC");
        s_local.streaming = false;
        return;
    }
    dlog("flac: abierto %luHz %dch %dbits desde %.2f", (unsigned long)fl->sampleRate, fl->channels,
         fl->bitsPerSample, (double)s_local.ratio);
    if (s_local.ratio > 0.0f && fl->totalPCMFrameCount > 0)
        drflac_seek_to_pcm_frame(fl, (drflac_uint64)((double)fl->totalPCMFrameCount * s_local.ratio));

    int ch = fl->channels, factor = 1;
    while (fl->sampleRate / factor > FLAC_MAX_OUT_RATE && factor < 8) factor *= 2;
    audio_set_pcm_rate((int)(fl->sampleRate / factor));

    int16_t* in = (int16_t*)malloc(FLAC_READ_FRAMES * ch * sizeof(int16_t));
    int16_t* out = (int16_t*)malloc(FLAC_READ_FRAMES * 2 * sizeof(int16_t));
    double tick_per_frame = (double)SYSCLOCK_ARM11 / fl->sampleRate;
    while (in && out && !s_local.stop) {
        if (audio_get_buffer_fill_percent() > 80) {
            svcSleepThread(15 * 1000 * 1000LL);
            continue;
        }
        u64 t0 = svcGetSystemTick();
        drflac_uint64 got = drflac_read_pcm_frames_s16(fl, FLAC_READ_FRAMES, in);
        if (got == 0) break;
        size_t n = 0;
        int right = ch > 1 ? 1 : 0;
        for (drflac_uint64 i = 0; i + factor <= got; i += factor, n++) {
            int32_t l = 0, r = 0;
            for (int k = 0; k < factor; k++) {
                const int16_t* fr = in + (i + k) * ch;
                l += fr[0];
                r += fr[right];
            }
            out[n * 2] = (int16_t)(l / factor);
            out[n * 2 + 1] = (int16_t)(r / factor);
        }
        audio_report_decode_load((float)((double)(svcGetSystemTick() - t0) / (got * tick_per_frame)));

        size_t bytes = n * 4, written = 0;
        while (!s_local.stop && written < bytes) {
            size_t w = audio_feed_mp3_data((const uint8_t*)out + written, bytes - written);
            written += w;
            if (w == 0) svcSleepThread(10 * 1000 * 1000LL);
        }
    }
    free(in);
    free(out);
    drflac_close(fl);
    dlog("flac: hilo terminado");
    s_local.streaming = false;
}

static void local_worker(void* arg) {
    if (has_ext(s_local.path, ".flac")) {
        local_worker_flac();
        return;
    }
    FILE* f = fopen(s_local.path, "rb");
    if (!f) {
        snprintf(s_local.error, sizeof(s_local.error), "No se pudo abrir el archivo");
        s_local.streaming = false;
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = audio_start(f);
    long offset = start + (long)((float)(size - start) * s_local.ratio);
    fseek(f, offset, SEEK_SET);

    uint8_t* chunk = (uint8_t*)malloc(LOCAL_READ_CHUNK);
    while (chunk && !s_local.stop) {
        size_t n = fread(chunk, 1, LOCAL_READ_CHUNK, f);
        if (n == 0) break;
        size_t written = 0;
        while (!s_local.stop && written < n) {
            // Como en red: esperar mientras el ring buffer esté casi lleno
            if (audio_get_buffer_fill_percent() > 80) {
                svcSleepThread(15 * 1000 * 1000LL);
                continue;
            }
            written += audio_feed_mp3_data(chunk + written, n - written);
        }
    }
    free(chunk);
    fclose(f);
    s_local.streaming = false;
}

bool local_start_stream(const char* path, float ratio) {
    local_stop_stream();
    if (!path || !path[0]) return false;

    snprintf(s_local.path, sizeof(s_local.path), "%s", path);
    s_local.ratio = ratio < 0.0f ? 0.0f : (ratio > 0.99f ? 0.99f : ratio);
    s_local.error[0] = '\0';
    s_local.stop = false;
    s_local.streaming = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (has_ext(path, ".flac")) {
        // FLAC se decodifica en este hilo: en el segundo núcleo (hasta 80 % de su tiempo) y,
        // si no se puede, en el principal con más prioridad que la interfaz, como el hilo de audio
        // El límite se sube una sola vez y queda así: cambiarlo de nuevo con la app andando
        // colgaba la consola al adelantar o cambiar de canción
        static bool s_cpu_limit_set = false;
        if (!s_cpu_limit_set) {
            Result rc = APT_SetAppCpuTimeLimit(80);
            dlog("flac: limite nucleo 1 -> 80 (0x%08lX)", (unsigned long)rc);
            s_cpu_limit_set = true;
        }
        s32 hi = prio - 1 < 0x18 ? 0x18 : prio - 1;
        s_local.thread = threadCreate(local_worker, NULL, FLAC_THREAD_STACK_SZ, hi, 1, false);
        s_local.core1 = s_local.thread != NULL;
        if (!s_local.thread) s_local.thread = threadCreate(local_worker, NULL, FLAC_THREAD_STACK_SZ, hi, -2, false);
        dlog("flac: hilo en nucleo %d", s_local.core1 ? 1 : 0);
    } else {
        s_local.thread = threadCreate(local_worker, NULL, LOCAL_THREAD_STACK_SZ, prio + 1 > 0x3F ? 0x3F : prio + 1, -2, false);
    }
    if (!s_local.thread) {
        s_local.streaming = false;
        snprintf(s_local.error, sizeof(s_local.error), "No se pudo crear el hilo de lectura");
        return false;
    }
    return true;
}

void local_stop_stream(void) {
    s_local.stop = true;
    if (s_local.thread) {
        dlog("local: deteniendo hilo (nucleo %d)", s_local.core1 ? 1 : 0);
        threadJoin(s_local.thread, U64_MAX);
        threadFree(s_local.thread);
        s_local.thread = NULL;
        dlog("local: hilo detenido");
    }
    s_local.core1 = false;
    s_local.streaming = false;
}

bool local_is_streaming(void) {
    return s_local.streaming;
}

const char* local_get_error(void) {
    return s_local.error;
}
