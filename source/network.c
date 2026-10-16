#include "network.h"
#include "audio.h"
#include "fmp4.h"
#include "hls.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <curl/curl.h>
#include <jansson.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// Client ID público verificado de SoundCloud
static const char* s_client_id = "A0cxVCVBD6HAZsm0P3nxKaPaZtrGqkXN";
static u32* s_soc_buffer = NULL;

#define NET_THREAD_STACK_SZ (32 * 1024)
#define SEARCH_JSON_MAX     (512 * 1024)
#define SEARCH_LIMIT        50
#define PLAYLIST_MAX        (2 * 1024 * 1024)
#define SEGMENT_MAX         (4 * 1024 * 1024)

// ndm:u bloqueado en modo infraestructura: así el Wi-Fi no se apaga al cerrar la tapa
static bool s_ndm_locked = false;

static struct {
    bool initialized;
    volatile bool is_streaming;
    volatile bool stop_requested;
    Thread stream_thread;
    StreamKind kind;
    char current_stream_url[1024];
    char resolved_url[1024];    // URL directa del CDN o de la lista HLS, reutilizada al adelantar

    // MP3 progresivo
    volatile u64 known_total;   // tamaño completo del MP3 (0 si se desconoce)
    u64* header_total;          // destino del callback de cabeceras de la petición en curso
    u64 start_offset;           // byte desde el que se pide el stream (Range)

    // HLS: inicio de cada segmento, conocido tras leer la lista
    uint32_t* hls_starts;
    volatile int hls_count;
    int hls_start_segment;
} s_net;

// Trabajo de red en segundo plano (búsqueda o carátula)
typedef struct {
    Thread thread;
    volatile bool running;
    volatile bool cancel;
    bool ok;
} NetJob;

static struct {
    NetJob job;
    char query[128];
    SearchResultSet results;
    char error[64];
} s_search;

static char s_stream_error[64];

static struct {
    NetJob job;
    char url[512];
    uint8_t* rgba;
    int w, h;
} s_artwork;

// Buffer que crece hasta max_size; lo que excede se descarta
typedef struct {
    char* memory;
    size_t size;
    size_t cap;
    size_t max_size;
} MemoryBuffer;

static size_t write_memory_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer* mem = (MemoryBuffer*)userp;

    size_t take = realsize;
    if (mem->size + take + 1 > mem->max_size) take = mem->max_size - mem->size - 1;

    if (mem->size + take + 1 > mem->cap) {
        size_t ncap = mem->cap ? mem->cap : 16 * 1024;
        while (ncap < mem->size + take + 1) ncap *= 2;
        if (ncap > mem->max_size) ncap = mem->max_size;
        char* grown = (char*)realloc(mem->memory, ncap);
        if (!grown) return 0;
        mem->memory = grown;
        mem->cap = ncap;
    }
    if (take > 0) {
        memcpy(&mem->memory[mem->size], contents, take);
        mem->size += take;
        mem->memory[mem->size] = 0;
    }
    return realsize;
}

// Aborta la transferencia en curso cuando se pide cancelar (libcurl la llama
// periódicamente incluso mientras espera la conexión)
static int abort_xferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow) {
    volatile bool* cancel = (volatile bool*)clientp;
    return (cancel && *cancel) ? 1 : 0;
}

static void set_common_options(CURL* curl, volatile bool* cancel) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)cancel);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
}

// GET completo a memoria. out->memory queda terminado en '\0'; liberar con free().
static CURLcode http_get(const char* url, size_t max_size, long timeout_s, volatile bool* cancel,
                         MemoryBuffer* out, long* http_code) {
    memset(out, 0, sizeof(*out));
    out->max_size = max_size;
    if (http_code) *http_code = 0;

    CURL* curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    set_common_options(curl, cancel);

    CURLcode res = curl_easy_perform(curl);
    if (http_code) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && http_code && *http_code >= 400) res = CURLE_HTTP_RETURNED_ERROR;
    if (res != CURLE_OK || !out->memory) {
        free(out->memory);
        out->memory = NULL;
        out->size = 0;
        if (res == CURLE_OK) res = CURLE_GOT_NOTHING;
    }
    return res;
}

// Crea un hilo de red con prioridad algo menor que la del hilo principal
static Thread net_thread_create(ThreadFunc func) {
    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    int net_prio = main_prio + 1;
    if (net_prio > 0x3F) net_prio = 0x3F;

    Thread t = threadCreate(func, NULL, NET_THREAD_STACK_SZ, net_prio, -2, false);
    if (!t) t = threadCreate(func, NULL, NET_THREAD_STACK_SZ, main_prio, -2, false);
    if (!t) t = threadCreate(func, NULL, NET_THREAD_STACK_SZ, main_prio, 0, false);
    return t;
}

static void job_join(NetJob* job) {
    if (job->thread) {
        threadJoin(job->thread, U64_MAX);
        threadFree(job->thread);
        job->thread = NULL;
    }
    job->running = false;
}

static void job_cancel(NetJob* job) {
    job->cancel = true;
    job_join(job);
    job->cancel = false;
}

// Entrega bytes al ring buffer de audio, esperando mientras esté casi lleno.
// Devuelve false si se pidió detener el stream.
static bool feed_audio(const uint8_t* data, size_t len) {
    while (!s_net.stop_requested && audio_get_buffer_fill_percent() > 80) {
        svcSleepThread(15 * 1000 * 1000LL);
    }
    size_t written = 0;
    while (!s_net.stop_requested && written < len) {
        written += audio_feed_mp3_data(data + written, len - written);
        if (written < len && !s_net.stop_requested) svcSleepThread(10 * 1000 * 1000LL);
    }
    return !s_net.stop_requested;
}

bool network_init(void) {
    if (s_net.initialized) return true;

    memset(&s_net, 0, sizeof(s_net));

    s_soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!s_soc_buffer) return false;

    Result res = socInit(s_soc_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(res)) {
        free(s_soc_buffer);
        s_soc_buffer = NULL;
        return false;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    // Mantener el Wi-Fi activo aunque se cierre la tapa (como ftpd). Si el servicio no
    // está disponible la app funciona igual, solo que el stream se corta en reposo.
    if (R_SUCCEEDED(ndmuInit())) {
        if (R_SUCCEEDED(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE))) {
            if (R_SUCCEEDED(NDMU_LockState())) s_ndm_locked = true;
            else NDMU_LeaveExclusiveState();
        }
    }

    s_net.initialized = true;
    return true;
}

void network_exit(void) {
    if (!s_net.initialized) return;

    network_stop_stream();
    job_cancel(&s_search.job);
    job_cancel(&s_artwork.job);
    if (s_artwork.rgba) {
        stbi_image_free(s_artwork.rgba);
        s_artwork.rgba = NULL;
    }
    free(s_net.hls_starts);
    s_net.hls_starts = NULL;
    curl_global_cleanup();
    if (s_ndm_locked) {
        NDMU_UnlockState();
        NDMU_LeaveExclusiveState();
        s_ndm_locked = false;
    }
    ndmuExit();
    socExit();

    if (s_soc_buffer) {
        free(s_soc_buffer);
        s_soc_buffer = NULL;
    }
    s_net.initialized = false;
}

const char* network_get_client_id(void) {
    return s_client_id;
}

// ---------------------------------------------------------------------------
// Búsqueda
// ---------------------------------------------------------------------------

static const char* json_str(json_t* obj, const char* key) {
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : NULL;
}

// Elige la mejor transcodificación reproducible de una pista:
// MP3 progresivo completo > HLS AAC completo > fragmento MP3 de 30 s.
static int pick_transcoding(json_t* transcodings, const char** url, StreamKind* kind, bool* preview) {
    int best = 0;
    size_t i;
    json_t* tc;
    json_array_foreach(transcodings, i, tc) {
        json_t* fmt = json_object_get(tc, "format");
        const char* protocol = json_str(fmt, "protocol");
        const char* mime = json_str(fmt, "mime_type");
        const char* tc_url = json_str(tc, "url");
        bool snipped = json_is_true(json_object_get(tc, "snipped"));
        if (!protocol || !mime || !tc_url) continue;

        int score = 0;
        StreamKind k = STREAM_NONE;
        if (strcmp(protocol, "progressive") == 0 && strncmp(mime, "audio/mpeg", 10) == 0) {
            score = snipped ? 1 : 4;
            k = STREAM_MP3;
        } else if (strcmp(protocol, "hls") == 0 && strncmp(mime, "audio/mp4", 9) == 0 && !snipped) {
            score = 3;
            k = STREAM_HLS_AAC;
        }
        if (score > best) {
            best = score;
            *url = tc_url;
            *kind = k;
            *preview = snipped;
        }
    }
    return best;
}

static bool search_tracks(const char* query, SearchResultSet* out_results, volatile bool* cancel) {
    if (!s_net.initialized || !query || !out_results) return false;
    memset(out_results, 0, sizeof(SearchResultSet));

    char* esc_q = curl_easy_escape(NULL, query, 0);
    char url[512];
    snprintf(url, sizeof(url), "https://api-v2.soundcloud.com/search/tracks?q=%s&client_id=%s&limit=%d",
             esc_q ? esc_q : query, s_client_id, SEARCH_LIMIT);
    if (esc_q) curl_free(esc_q);

    MemoryBuffer body;
    long http_code = 0;
    CURLcode res = http_get(url, SEARCH_JSON_MAX, 15L, cancel, &body, &http_code);
    if (res != CURLE_OK) {
        if (http_code >= 400)
            snprintf(s_search.error, sizeof(s_search.error), "SoundCloud respondio HTTP %ld", http_code);
        else
            snprintf(s_search.error, sizeof(s_search.error), "Red: %s", curl_easy_strerror(res));
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loadb(body.memory, body.size, 0, &jerr);
    free(body.memory);
    if (!root) {
        snprintf(s_search.error, sizeof(s_search.error), "Respuesta invalida (linea %d)", jerr.line);
        return false;
    }

    int count = 0, seen = 0;
    size_t i;
    json_t* track;
    json_array_foreach(json_object_get(root, "collection"), i, track) {
        if (count >= MAX_SEARCH_RESULTS) break;
        seen++;

        const char* stream_url = NULL;
        StreamKind kind = STREAM_NONE;
        bool preview = false;
        json_t* transcodings = json_object_get(json_object_get(track, "media"), "transcodings");
        if (!pick_transcoding(transcodings, &stream_url, &kind, &preview)) continue;

        SearchResultTrack* t = &out_results->tracks[count];
        memset(t, 0, sizeof(*t));
        t->kind = kind;
        t->is_preview = preview;
        snprintf(t->stream_endpoint, sizeof(t->stream_endpoint), "%s", stream_url);

        const char* title = json_str(track, "title");
        snprintf(t->title, sizeof(t->title), "%s", title ? title : "?");

        json_t* user = json_object_get(track, "user");
        const char* artist = json_str(user, "username");
        snprintf(t->artist, sizeof(t->artist), "%s", artist ? artist : "");

        json_t* duration = json_object_get(track, "full_duration");
        if (!json_is_integer(duration)) duration = json_object_get(track, "duration");
        t->duration_ms = json_is_integer(duration) ? (uint32_t)json_integer_value(duration) : 0;

        const char* art = json_str(track, "artwork_url");
        if (!art) art = json_str(user, "avatar_url");
        if (art) snprintf(t->artwork_url, sizeof(t->artwork_url), "%s", art);

        count++;
    }
    json_decref(root);

    if (count == 0) {
        if (seen > 0)
            snprintf(s_search.error, sizeof(s_search.error), "%d resultados, ninguno reproducible", seen);
        else
            snprintf(s_search.error, sizeof(s_search.error), "Sin resultados");
    }

    out_results->count = count;
    return count > 0;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

// Resuelve el endpoint de api-v2 a la URL firmada del CDN (MP3 o lista HLS).
static bool resolve_stream_url(const char* endpoint, char* out, size_t out_sz) {
    if (!strstr(endpoint, "api-v2.soundcloud.com")) {
        snprintf(out, out_sz, "%s", endpoint);
        return strncmp(out, "http", 4) == 0;
    }

    char resolve_url[1100];
    snprintf(resolve_url, sizeof(resolve_url), "%s?client_id=%s", endpoint, s_client_id);

    MemoryBuffer body;
    if (http_get(resolve_url, 16 * 1024, 10L, &s_net.stop_requested, &body, NULL) != CURLE_OK) return false;

    bool ok = false;
    json_t* root = json_loadb(body.memory, body.size, 0, NULL);
    free(body.memory);
    if (root) {
        const char* url = json_str(root, "url");
        if (url && strncmp(url, "http", 4) == 0) {
            snprintf(out, out_sz, "%s", url);
            ok = true;
        }
        json_decref(root);
    }
    return ok;
}

static size_t mp3_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    if (s_net.stop_requested) return 0;

    // Las cabeceras ya llegaron: el tamaño total queda disponible para adelantar
    if (s_net.header_total && *s_net.header_total > 0) s_net.known_total = *s_net.header_total;

    return feed_audio((const uint8_t*)contents, realsize) ? realsize : 0;
}

// Lee el tamaño total del MP3 de las cabeceras: Content-Range ".../total" cuando
// se pidió un rango, o Content-Length en una petición completa.
static size_t mp3_header_callback(char* buf, size_t size, size_t nitems, void* userp) {
    size_t len = size * nitems;
    u64* total = (u64*)userp;
    if (len >= 5 && strncmp(buf, "HTTP/", 5) == 0) {
        *total = 0; // nueva respuesta (redirección): descartar valores anteriores
    } else if (len > 14 && strncasecmp(buf, "Content-Range:", 14) == 0) {
        const char* slash = memchr(buf, '/', len);
        if (slash && slash[1] != '*') *total = strtoull(slash + 1, NULL, 10);
    } else if (len > 15 && strncasecmp(buf, "Content-Length:", 15) == 0 && s_net.start_offset == 0) {
        *total = strtoull(buf + 15, NULL, 10);
    }
    return len;
}

static CURLcode stream_mp3_from(const char* url) {
    CURL* curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    u64 total = 0;
    char range[32];
    s_net.header_total = &total;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mp3_write_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, mp3_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)&total);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    if (s_net.start_offset > 0) {
        snprintf(range, sizeof(range), "%llu-", (unsigned long long)s_net.start_offset);
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }
    set_common_options(curl, &s_net.stop_requested);
    // Cortar si la conexión queda colgada: menos de 1 KB/s durante 20 s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);

    CURLcode res = curl_easy_perform(curl);
    s_net.header_total = NULL;
    curl_easy_cleanup(curl);
    return res;
}

static CURLcode stream_mp3(bool cached) {
    CURLcode res = stream_mp3_from(s_net.resolved_url);

    // La URL firmada del CDN caduca: si una reanudación falla, resolver de nuevo una vez
    if (res == CURLE_HTTP_RETURNED_ERROR && cached && !s_net.stop_requested &&
        resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url))) {
        res = stream_mp3_from(s_net.resolved_url);
    }
    return res;
}

// Descarga y prepara la lista HLS (siguiendo una lista maestra si hace falta)
static bool hls_load_playlist(HlsPlaylist* pl) {
    char url[1024];
    snprintf(url, sizeof(url), "%s", s_net.resolved_url);

    for (int depth = 0; depth < 2; depth++) {
        MemoryBuffer body;
        CURLcode res = http_get(url, PLAYLIST_MAX, 15L, &s_net.stop_requested, &body, NULL);
        if (res != CURLE_OK) {
            if (!s_net.stop_requested)
                snprintf(s_stream_error, sizeof(s_stream_error), "Lista HLS: %s", curl_easy_strerror(res));
            return false;
        }
        bool ok = hls_parse(body.memory, url, pl);
        free(body.memory);
        if (!ok) {
            snprintf(s_stream_error, sizeof(s_stream_error), "Lista HLS no compatible");
            return false;
        }
        if (!pl->is_master) return true;
        snprintf(url, sizeof(url), "%s", pl->variant_uri);
        hls_free(pl);
    }
    snprintf(s_stream_error, sizeof(s_stream_error), "Lista HLS no compatible");
    return false;
}

typedef struct {
    Fmp4AudioConfig cfg;
} HlsFeedCtx;

static bool hls_on_frame(const uint8_t* frame, size_t size, void* user) {
    HlsFeedCtx* ctx = (HlsFeedCtx*)user;
    uint8_t hdr[7];
    fmp4_adts_header(hdr, &ctx->cfg, size);
    return feed_audio(hdr, sizeof(hdr)) && feed_audio(frame, size);
}

static CURLcode stream_hls(bool cached) {
    HlsPlaylist pl;
    bool loaded = hls_load_playlist(&pl);
    if (!loaded && cached && !s_net.stop_requested) {
        // La lista firmada caducó: pedir una nueva
        if (resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url)))
            loaded = hls_load_playlist(&pl);
    }
    if (!loaded) return CURLE_GOT_NOTHING;

    if (!pl.init_uri) {
        snprintf(s_stream_error, sizeof(s_stream_error), "HLS sin segmento de inicio");
        hls_free(&pl);
        return CURLE_GOT_NOTHING;
    }

    // Publicar los inicios de segmento para que la barra de progreso pueda saltar
    uint32_t* starts = (uint32_t*)malloc((size_t)pl.count * sizeof(uint32_t));
    if (starts) {
        for (int i = 0; i < pl.count; i++) starts[i] = pl.segments[i].start_ms;
        free(s_net.hls_starts);
        s_net.hls_starts = starts;
        s_net.hls_count = pl.count;
    }

    HlsFeedCtx ctx;
    MemoryBuffer seg;
    CURLcode res = http_get(pl.init_uri, 64 * 1024, 15L, &s_net.stop_requested, &seg, NULL);
    if (res != CURLE_OK || !fmp4_parse_init((const uint8_t*)seg.memory, seg.size, &ctx.cfg)) {
        if (res == CURLE_OK) snprintf(s_stream_error, sizeof(s_stream_error), "Formato de audio no compatible");
        free(seg.memory);
        hls_free(&pl);
        return res == CURLE_OK ? CURLE_GOT_NOTHING : res;
    }
    free(seg.memory);

    for (int i = s_net.hls_start_segment; i < pl.count && !s_net.stop_requested; i++) {
        res = http_get(pl.segments[i].uri, SEGMENT_MAX, 60L, &s_net.stop_requested, &seg, NULL);
        if (res != CURLE_OK) break;
        int frames = fmp4_parse_segment((const uint8_t*)seg.memory, seg.size, &ctx.cfg, hls_on_frame, &ctx);
        free(seg.memory);
        if (frames < 0 && !s_net.stop_requested) {
            snprintf(s_stream_error, sizeof(s_stream_error), "Segmento %d danado", i);
            res = CURLE_GOT_NOTHING;
            break;
        }
    }

    hls_free(&pl);
    return res;
}

static void stream_worker_func(void* arg) {
    bool cached = s_net.resolved_url[0] != '\0';

    if (!cached && !resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url))) {
        if (!s_net.stop_requested) snprintf(s_stream_error, sizeof(s_stream_error), "No se pudo obtener el stream");
        s_net.resolved_url[0] = '\0';
        s_net.is_streaming = false;
        return;
    }

    CURLcode res = CURLE_OK;
    if (!s_net.stop_requested) {
        res = (s_net.kind == STREAM_HLS_AAC) ? stream_hls(cached) : stream_mp3(cached);
    }

    if (res != CURLE_OK && !s_net.stop_requested && s_stream_error[0] == '\0') {
        snprintf(s_stream_error, sizeof(s_stream_error), "Stream: %s", curl_easy_strerror(res));
    }
    s_net.is_streaming = false;
}

static bool stream_thread_start(void) {
    s_stream_error[0] = '\0';
    s_net.stop_requested = false;
    s_net.is_streaming = true;
    s_net.stream_thread = net_thread_create(stream_worker_func);
    if (!s_net.stream_thread) {
        s_net.is_streaming = false;
        return false;
    }
    return true;
}

bool network_start_stream(const char* stream_endpoint, StreamKind kind) {
    if (!stream_endpoint || stream_endpoint[0] == '\0' || kind == STREAM_NONE) return false;

    network_stop_stream();
    audio_clear_buffers();
    audio_set_codec(kind == STREAM_HLS_AAC ? AUDIO_CODEC_AAC : AUDIO_CODEC_MP3);

    snprintf(s_net.current_stream_url, sizeof(s_net.current_stream_url), "%s", stream_endpoint);
    s_net.kind = kind;
    s_net.resolved_url[0] = '\0';
    s_net.known_total = 0;
    s_net.start_offset = 0;
    s_net.hls_count = 0;
    s_net.hls_start_segment = 0;
    return stream_thread_start();
}

bool network_seek_stream(float ratio, uint32_t duration_ms, uint32_t* start_ms) {
    if (s_net.current_stream_url[0] == '\0') return false;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 0.99f) ratio = 0.99f;

    if (s_net.kind == STREAM_HLS_AAC) {
        if (s_net.hls_count == 0 || !s_net.hls_starts) return false;
        network_stop_stream();

        uint32_t target = (uint32_t)((float)duration_ms * ratio);
        int idx = 0;
        while (idx + 1 < s_net.hls_count && s_net.hls_starts[idx + 1] <= target) idx++;
        s_net.hls_start_segment = idx;
        if (start_ms) *start_ms = s_net.hls_starts[idx];
    } else {
        u64 total = s_net.known_total;
        if (total == 0) return false;
        network_stop_stream();
        s_net.start_offset = (u64)((double)total * ratio);
        if (start_ms) *start_ms = (uint32_t)((float)duration_ms * ratio);
    }

    audio_clear_buffers();
    audio_set_codec(s_net.kind == STREAM_HLS_AAC ? AUDIO_CODEC_AAC : AUDIO_CODEC_MP3);
    return stream_thread_start();
}

void network_stop_stream(void) {
    s_net.stop_requested = true;

    if (s_net.stream_thread) {
        threadJoin(s_net.stream_thread, U64_MAX);
        threadFree(s_net.stream_thread);
        s_net.stream_thread = NULL;
    }

    s_net.is_streaming = false;
}

bool network_is_streaming(void) {
    return s_net.is_streaming;
}

// ---------------------------------------------------------------------------
// Carátula
// ---------------------------------------------------------------------------

static bool download_artwork(const char* url, uint8_t** out_rgba, int* out_w, int* out_h, volatile bool* cancel) {
    if (!url || url[0] == '\0' || strcmp(url, "null") == 0) return false;

    MemoryBuffer body;
    if (http_get(url, 512 * 1024, 10L, cancel, &body, NULL) != CURLE_OK) return false;

    int w = 0, h = 0, comp = 0;
    uint8_t* decoded = stbi_load_from_memory((const stbi_uc*)body.memory, (int)body.size, &w, &h, &comp, 4);
    free(body.memory);
    if (!decoded) return false;

    *out_rgba = decoded;
    *out_w = w;
    *out_h = h;
    return true;
}

void network_free_artwork(uint8_t* rgba) {
    if (rgba) stbi_image_free(rgba);
}

// ---------------------------------------------------------------------------
// Trabajos asíncronos: el hilo principal sigue dibujando mientras esperan
// ---------------------------------------------------------------------------

const char* network_get_search_error(void) {
    return s_search.error;
}

const char* network_get_stream_error(void) {
    return s_stream_error;
}

static void search_worker_func(void* arg) {
    s_search.job.ok = search_tracks(s_search.query, &s_search.results, &s_search.job.cancel);
    s_search.job.running = false;
}

bool network_search_async(const char* query) {
    if (!s_net.initialized || !query || query[0] == '\0') return false;
    if (s_search.job.running) return false;
    job_join(&s_search.job);

    snprintf(s_search.query, sizeof(s_search.query), "%s", query);
    s_search.job.ok = false;
    s_search.error[0] = '\0';
    s_search.job.cancel = false;
    s_search.job.running = true;
    s_search.job.thread = net_thread_create(search_worker_func);
    if (!s_search.job.thread) {
        s_search.job.running = false;
        return false;
    }
    return true;
}

NetJobStatus network_poll_search(SearchResultSet* out_results) {
    if (s_search.job.running) return NET_JOB_RUNNING;
    if (!s_search.job.thread) return NET_JOB_IDLE;

    job_join(&s_search.job);
    if (!s_search.job.ok) return NET_JOB_FAILED;
    if (out_results) *out_results = s_search.results;
    return NET_JOB_DONE;
}

static void artwork_worker_func(void* arg) {
    s_artwork.job.ok = download_artwork(s_artwork.url, &s_artwork.rgba, &s_artwork.w, &s_artwork.h,
                                        &s_artwork.job.cancel);
    s_artwork.job.running = false;
}

bool network_artwork_async(const char* url) {
    job_cancel(&s_artwork.job);
    if (s_artwork.rgba) {
        stbi_image_free(s_artwork.rgba);
        s_artwork.rgba = NULL;
    }
    if (!s_net.initialized || !url || url[0] == '\0' || strcmp(url, "null") == 0) return false;

    snprintf(s_artwork.url, sizeof(s_artwork.url), "%s", url);
    s_artwork.job.ok = false;
    s_artwork.job.running = true;
    s_artwork.job.thread = net_thread_create(artwork_worker_func);
    if (!s_artwork.job.thread) {
        s_artwork.job.running = false;
        return false;
    }
    return true;
}

NetJobStatus network_poll_artwork(uint8_t** out_rgba, int* out_w, int* out_h) {
    if (s_artwork.job.running) return NET_JOB_RUNNING;
    if (!s_artwork.job.thread) return NET_JOB_IDLE;

    job_join(&s_artwork.job);
    if (!s_artwork.job.ok || !s_artwork.rgba) return NET_JOB_FAILED;

    // El llamador pasa a ser dueño de los píxeles y los libera con network_free_artwork
    *out_rgba = s_artwork.rgba;
    *out_w = s_artwork.w;
    *out_h = s_artwork.h;
    s_artwork.rgba = NULL;
    return NET_JOB_DONE;
}
