#include "network.h"
#include "audio.h"
#include "fmp4.h"
#include "hls.h"
#include "debuglog.h"
#include "local.h"
#include "i18n.h"
#include "account.h"
#include "subsonic.h"
#include "net_http.h"
#include <dirent.h>
#include <sys/stat.h>
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

// Trabajos de playlists y cola de descargas (más abajo)
static void playlist_jobs_cancel(void);
static void acclist_cancel(void);
static void recv_cancel(void);
static void dl_lock_init(void);
static void dl_load_state(void);

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
// auth_header: "Authorization: OAuth ..." para la cuenta conectada (NULL = sin cuenta)
static CURLcode http_get_auth(const char* url, size_t max_size, long timeout_s, volatile bool* cancel,
                              MemoryBuffer* out, long* http_code, const char* auth_header) {
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
    struct curl_slist* hdr = auth_header ? curl_slist_append(NULL, auth_header) : NULL;
    if (hdr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);

    CURLcode res = curl_easy_perform(curl);
    if (http_code) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdr);

    if (res == CURLE_OK && http_code && *http_code >= 400) res = CURLE_HTTP_RETURNED_ERROR;
    if (res != CURLE_OK || !out->memory) {
        free(out->memory);
        out->memory = NULL;
        out->size = 0;
        if (res == CURLE_OK) res = CURLE_GOT_NOTHING;
    }
    return res;
}

static CURLcode http_get(const char* url, size_t max_size, long timeout_s, volatile bool* cancel,
                         MemoryBuffer* out, long* http_code) {
    return http_get_auth(url, max_size, timeout_s, cancel, out, http_code, NULL);
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
    dlog("ndm lock %s", s_ndm_locked ? "ok" : "no");

    s_net.initialized = true;
    subsonic_init();
    dl_lock_init();
    dl_load_state();
    return true;
}

void network_exit(void) {
    if (!s_net.initialized) return;

    network_stop_stream();
    network_download_stop();
    job_cancel(&s_search.job);
    playlist_jobs_cancel();
    acclist_cancel();
    recv_cancel();
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

// Llena t con una pista de la API. false si no tiene un stream reproducible.
static bool parse_track(json_t* track, SearchResultTrack* t) {
    const char* stream_url = NULL;
    StreamKind kind = STREAM_NONE;
    bool preview = false;
    json_t* transcodings = json_object_get(json_object_get(track, "media"), "transcodings");
    if (!pick_transcoding(transcodings, &stream_url, &kind, &preview)) return false;

    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->is_preview = preview;
    snprintf(t->stream_endpoint, sizeof(t->stream_endpoint), "%s", stream_url);

    json_t* jid = json_object_get(track, "id");
    if (json_is_integer(jid)) snprintf(t->id, sizeof(t->id), "%lld", (long long)json_integer_value(jid));

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
    return true;
}

// Limpia memoria que tuvo un token
static void wipe_mem(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

// GET a la API v2 con el client_id; devuelve el JSON o NULL (con el error en err).
// path_or_url: "/ruta?consulta" o una URL completa de la API (el next_href de una página).
// auth: con la sesión de la cuenta conectada; tras un 401 se renueva la sesión y se
// reintenta una sola vez. Los tokens nunca van al registro.
static json_t* api_get_json_ex(const char* path_or_url, size_t max_size, volatile bool* cancel, char* err,
                               size_t err_sz, bool auth) {
    static const char* API = "https://api-v2.soundcloud.com";
    char url[1400];
    bool full = strncmp(path_or_url, API, strlen(API)) == 0;
    bool has_cid = strstr(path_or_url, "client_id=") != NULL;
    snprintf(url, sizeof(url), "%s%s", full ? "" : API, path_or_url);
    if (!has_cid) {
        size_t len = strlen(url);
        snprintf(url + len, sizeof(url) - len, "%sclient_id=%s", strchr(url, '?') ? "&" : "?", s_client_id);
    }

    char* header = NULL;
    u32 gen = 0;
    if (auth) {
        header = (char*)malloc(4200);
        if (!header || !account_token(header + 21, 4200 - 21, &gen)) {
            free(header);
            if (err) snprintf(err, err_sz, "%s", L("Conecta tu cuenta desde Sound3DS Share"));
            return NULL;
        }
        memcpy(header, "Authorization: OAuth ", 21);
    }

    MemoryBuffer body;
    long http_code = 0;
    CURLcode res = http_get_auth(url, max_size, 20L, cancel, &body, &http_code, header);
    if (auth && http_code == 401 && !(cancel && *cancel)) {
        // Token vencido o revocado: renovar (si otro hilo no lo hizo ya) y reintentar una vez
        if (account_on_unauthorized(gen) && account_token(header + 21, 4200 - 21, &gen))
            res = http_get_auth(url, max_size, 20L, cancel, &body, &http_code, header);
    }
    if (header) {
        wipe_mem(header, 4200);
        free(header);
    }
    if (res != CURLE_OK) {
        if (err) {
            if (auth && http_code == 401) snprintf(err, err_sz, "%s", L("Vuelve a conectar SoundCloud desde Sound3DS Share"));
            else if (http_code >= 400) snprintf(err, err_sz, L("SoundCloud respondio HTTP %ld"), http_code);
            else snprintf(err, err_sz, L("Red: %s"), curl_easy_strerror(res));
        }
        return NULL;
    }
    json_error_t jerr;
    json_t* root = json_loadb(body.memory, body.size, 0, &jerr);
    free(body.memory);
    if (!root && err) snprintf(err, err_sz, L("Respuesta invalida (linea %d)"), jerr.line);
    return root;
}

static json_t* api_get_json(const char* path_and_query, size_t max_size, volatile bool* cancel, char* err, size_t err_sz) {
    return api_get_json_ex(path_and_query, max_size, cancel, err, err_sz, false);
}

json_t* net_get_json(const char* url, size_t max_size, volatile bool* cancel, long* http_code, char* err,
                     size_t err_sz) {
    MemoryBuffer body;
    long code = 0;
    CURLcode res = http_get(url, max_size, 20L, cancel, &body, &code);
    if (http_code) *http_code = code;
    if (res != CURLE_OK) {
        if (err) {
            if (code >= 400) snprintf(err, err_sz, L("El servidor respondio HTTP %ld"), code);
            else snprintf(err, err_sz, L("Red: %s"), curl_easy_strerror(res));
        }
        return NULL;
    }
    json_error_t jerr;
    json_t* root = json_loadb(body.memory, body.size, 0, &jerr);
    free(body.memory);
    if (!root && err) snprintf(err, err_sz, L("Respuesta invalida (linea %d)"), jerr.line);
    return root;
}

// Servidor propio (Subsonic) en lugar de SoundCloud
static volatile bool s_server = false;

bool network_is_server(void) {
    return s_server;
}

static bool search_tracks(const char* query, SearchResultSet* out_results, volatile bool* cancel) {
    if (!s_net.initialized || !query || !out_results) return false;
    memset(out_results, 0, sizeof(SearchResultSet));

    char* esc_q = curl_easy_escape(NULL, query, 0);
    char path[400];
    snprintf(path, sizeof(path), "/search/tracks?q=%s&limit=%d", esc_q ? esc_q : query, SEARCH_LIMIT);
    if (esc_q) curl_free(esc_q);

    json_t* root = api_get_json(path, SEARCH_JSON_MAX, cancel, s_search.error, sizeof(s_search.error));
    if (!root) return false;

    int count = 0, seen = 0;
    size_t i;
    json_t* track;
    json_array_foreach(json_object_get(root, "collection"), i, track) {
        if (count >= MAX_SEARCH_RESULTS) break;
        seen++;
        if (parse_track(track, &out_results->tracks[count])) count++;
    }
    json_decref(root);

    if (count == 0) {
        if (seen > 0)
            snprintf(s_search.error, sizeof(s_search.error), L("%d resultados, ninguno reproducible"), seen);
        else
            snprintf(s_search.error, sizeof(s_search.error), "Sin resultados");
    }

    out_results->count = count;
    return count > 0;
}

// La API entrega carátulas "-large" de 100x100; la variante t300x300 se ve bien en grande
static void art_url_t300(const char* in, char* out, size_t out_sz) {
    snprintf(out, out_sz, "%s", in);
    char* large = strstr(out, "-large.");
    if (large) {
        char tail[64];
        snprintf(tail, sizeof(tail), "%s", large + 6);
        snprintf(large, out_sz - (size_t)(large - out), "-t300x300%s", tail);
    }
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------

#define PLAYLIST_JSON_MAX (2 * 1024 * 1024)
#define TRACKS_BATCH      50
#define PLAYLIST_SEARCH_LIMIT 30

static struct {
    NetJob job;
    char query[128];
    PlaylistResultSet results;
    char error[64];
} s_plsearch;

static struct {
    NetJob job;
    char id[48];
    bool auth;
    PlaylistDetail detail;
    uint8_t* rgba;
    int w, h;
} s_playlist;

static void playlist_jobs_cancel(void) {
    job_cancel(&s_plsearch.job);
    job_cancel(&s_playlist.job);
    if (s_playlist.rgba) {
        stbi_image_free(s_playlist.rgba);
        s_playlist.rgba = NULL;
    }
}

const char* network_get_playlist_error(void) {
    return s_plsearch.error;
}

static bool search_playlists(const char* query, PlaylistResultSet* out, volatile bool* cancel) {
    memset(out, 0, sizeof(*out));
    char* esc_q = curl_easy_escape(NULL, query, 0);
    char path[400];
    // Solo playlists de usuarios: los álbumes oficiales casi siempre son avances de 30 s
    // Cada playlist trae varias canciones completas: 30 resultados pesan ~1 MB
    snprintf(path, sizeof(path), "/search/playlists_without_albums?q=%s&limit=%d", esc_q ? esc_q : query,
             PLAYLIST_SEARCH_LIMIT);
    if (esc_q) curl_free(esc_q);

    json_t* root = api_get_json(path, PLAYLIST_JSON_MAX, cancel, s_plsearch.error, sizeof(s_plsearch.error));
    if (!root) {
        dlog("busqueda de playlists: %s", s_plsearch.error);
        return false;
    }
    size_t i;
    json_t* pl;
    json_array_foreach(json_object_get(root, "collection"), i, pl) {
        if (out->count >= MAX_SEARCH_RESULTS) break;
        json_t* jid = json_object_get(pl, "id");
        json_t* jcount = json_object_get(pl, "track_count");
        int tracks = json_is_integer(jcount) ? (int)json_integer_value(jcount) : 0;
        if (!json_is_integer(jid) || tracks <= 0) continue;
        PlaylistResult* r = &out->items[out->count++];
        snprintf(r->id, sizeof(r->id), "%lld", (long long)json_integer_value(jid));
        const char* title = json_str(pl, "title");
        snprintf(r->title, sizeof(r->title), "%s", title ? title : "?");
        const char* creator = json_str(json_object_get(pl, "user"), "username");
        snprintf(r->creator, sizeof(r->creator), "%s", creator ? creator : "");
        r->track_count = tracks;
        const char* art = json_str(pl, "artwork_url");
        if (art) snprintf(r->artwork_url, sizeof(r->artwork_url), "%s", art);
    }
    json_decref(root);
    if (out->count == 0) snprintf(s_plsearch.error, sizeof(s_plsearch.error), "Sin resultados");
    return out->count > 0;
}

// Carga la playlist: la API trae completas solo las primeras canciones y del resto solo
// el id, que se piden con /tracks?ids= en grupos de 50. Se conserva el orden original.
static bool load_playlist(const char* id, PlaylistDetail* d, volatile bool* cancel, char* err, size_t err_sz,
                          bool auth) {
    memset(d, 0, sizeof(*d));
    char path[64];
    snprintf(path, sizeof(path), "/playlists/%s", id);
    json_t* root = api_get_json_ex(path, PLAYLIST_JSON_MAX, cancel, err, err_sz, auth);
    if (!root) {
        dlog("playlist %s: %s", id, err);
        return false;
    }

    snprintf(d->id, sizeof(d->id), "%s", id);
    const char* title = json_str(root, "title");
    snprintf(d->title, sizeof(d->title), "%s", title ? title : "?");
    const char* creator = json_str(json_object_get(root, "user"), "username");
    snprintf(d->creator, sizeof(d->creator), "%s", creator ? creator : "");
    const char* art = json_str(root, "artwork_url");
    if (art) snprintf(d->artwork_url, sizeof(d->artwork_url), "%s", art);

    // Lugar de cada canción (en orden) y si ya se pudo leer
    static long long ids[MAX_PLAYLIST_TRACKS];
    static bool ready[MAX_PLAYLIST_TRACKS];
    int slots = 0;
    size_t i;
    json_t* tr;
    json_t* tracks = json_object_get(root, "tracks");
    d->total = (int)json_array_size(tracks);
    json_array_foreach(tracks, i, tr) {
        if (slots >= MAX_PLAYLIST_TRACKS) break;
        json_t* jid = json_object_get(tr, "id");
        ids[slots] = json_is_integer(jid) ? (long long)json_integer_value(jid) : 0;
        ready[slots] = json_object_get(tr, "title") != NULL && parse_track(tr, &d->tracks[slots]);
        // Una canción completa pero sin stream reproducible no se vuelve a pedir
        if (json_object_get(tr, "title") != NULL && !ready[slots]) ids[slots] = 0;
        d->position[slots] = slots + 1;
        slots++;
    }
    json_decref(root);

    // Las que faltan, en grupos
    for (int start = 0; start < slots && !*cancel; ) {
        char list[TRACKS_BATCH * 21 + 32];
        size_t len = 0;
        int taken = 0, k = start;
        for (; k < slots && taken < TRACKS_BATCH; k++) {
            if (ready[k] || ids[k] == 0) continue;
            len += (size_t)snprintf(list + len, sizeof(list) - len, "%s%lld", taken ? "%2C" : "", ids[k]);
            taken++;
        }
        start = k;
        if (taken == 0) continue;
        char tpath[sizeof(list) + 16];
        snprintf(tpath, sizeof(tpath), "/tracks?ids=%s", list);
        json_t* arr = api_get_json_ex(tpath, PLAYLIST_JSON_MAX, cancel, NULL, 0, auth);
        if (!arr) continue;
        json_array_foreach(arr, i, tr) {
            json_t* jid = json_object_get(tr, "id");
            if (!json_is_integer(jid)) continue;
            long long tid = (long long)json_integer_value(jid);
            for (int s = 0; s < slots; s++)
                if (!ready[s] && ids[s] == tid) {
                    ready[s] = parse_track(tr, &d->tracks[s]);
                    break;
                }
        }
        json_decref(arr);
    }
    if (*cancel) return false;

    // Compactar en orden, sin las que no se pueden reproducir
    for (int s = 0; s < slots; s++) {
        if (!ready[s]) {
            d->unavailable++;
            continue;
        }
        if (s != d->count) {
            d->tracks[d->count] = d->tracks[s];
            d->position[d->count] = d->position[s];
        }
        d->total_ms += d->tracks[d->count].duration_ms;
        d->count++;
    }
    d->unavailable += d->total - slots;   // más allá del límite
    if (!d->artwork_url[0] && d->count > 0) snprintf(d->artwork_url, sizeof(d->artwork_url), "%s", d->tracks[0].artwork_url);
    if (d->count == 0) snprintf(err, err_sz, "La playlist no tiene canciones reproducibles");
    return d->count > 0;
}

static void plsearch_worker(void* arg) {
    s_plsearch.job.ok = s_server ? subsonic_search_albums(s_plsearch.query, &s_plsearch.results, &s_plsearch.job.cancel,
                                                          s_plsearch.error, sizeof(s_plsearch.error))
                                 : search_playlists(s_plsearch.query, &s_plsearch.results, &s_plsearch.job.cancel);
    s_plsearch.job.running = false;
}

bool network_search_playlists_async(const char* query) {
    if (!s_net.initialized || !query || !query[0] || s_plsearch.job.running) return false;
    job_join(&s_plsearch.job);
    snprintf(s_plsearch.query, sizeof(s_plsearch.query), "%s", query);
    s_plsearch.error[0] = '\0';
    s_plsearch.job.ok = false;
    s_plsearch.job.cancel = false;
    s_plsearch.job.running = true;
    s_plsearch.job.thread = net_thread_create(plsearch_worker);
    if (!s_plsearch.job.thread) {
        s_plsearch.job.running = false;
        return false;
    }
    return true;
}

NetJobStatus network_poll_playlist_search(PlaylistResultSet* out) {
    if (s_plsearch.job.running) return NET_JOB_RUNNING;
    if (!s_plsearch.job.thread) return NET_JOB_IDLE;
    job_join(&s_plsearch.job);
    if (!s_plsearch.job.ok) return NET_JOB_FAILED;
    if (out) *out = s_plsearch.results;
    return NET_JOB_DONE;
}

static bool download_artwork(const char* url, uint8_t** out_rgba, int* out_w, int* out_h, volatile bool* cancel);

static void playlist_worker(void* arg) {
    s_playlist.job.ok = s_server ? subsonic_load_list(s_playlist.id, &s_playlist.detail, &s_playlist.job.cancel,
                                                      s_plsearch.error, sizeof(s_plsearch.error))
                                 : load_playlist(s_playlist.id, &s_playlist.detail, &s_playlist.job.cancel,
                                                 s_plsearch.error, sizeof(s_plsearch.error), s_playlist.auth);
    if (s_playlist.job.ok && s_playlist.detail.artwork_url[0]) {
        char big[560];
        art_url_t300(s_playlist.detail.artwork_url, big, sizeof(big));
        if (!download_artwork(big, &s_playlist.rgba, &s_playlist.w, &s_playlist.h, &s_playlist.job.cancel))
            download_artwork(s_playlist.detail.artwork_url, &s_playlist.rgba, &s_playlist.w, &s_playlist.h,
                             &s_playlist.job.cancel);
    }
    s_playlist.job.running = false;
}

bool network_playlist_async(const char* id) {
    return network_playlist_async_ex(id, false);
}

bool network_playlist_async_ex(const char* id, bool auth) {
    job_cancel(&s_playlist.job);
    if (s_playlist.rgba) {
        stbi_image_free(s_playlist.rgba);
        s_playlist.rgba = NULL;
    }
    if (!s_net.initialized || !id || !id[0]) return false;
    snprintf(s_playlist.id, sizeof(s_playlist.id), "%s", id);
    s_playlist.auth = auth;
    s_plsearch.error[0] = '\0';
    s_playlist.job.ok = false;
    s_playlist.job.running = true;
    s_playlist.job.thread = net_thread_create(playlist_worker);
    if (!s_playlist.job.thread) {
        s_playlist.job.running = false;
        return false;
    }
    return true;
}

NetJobStatus network_poll_playlist(PlaylistDetail* out, uint8_t** cover_rgba, int* cover_w, int* cover_h) {
    if (s_playlist.job.running) return NET_JOB_RUNNING;
    if (!s_playlist.job.thread) return NET_JOB_IDLE;
    job_join(&s_playlist.job);
    if (!s_playlist.job.ok) return NET_JOB_FAILED;
    if (out) *out = s_playlist.detail;
    *cover_rgba = s_playlist.rgba;   // el llamador la libera con network_free_artwork
    *cover_w = s_playlist.w;
    *cover_h = s_playlist.h;
    s_playlist.rgba = NULL;
    return NET_JOB_DONE;
}

// ---------------------------------------------------------------------------
// Cuenta: likes, playlists de la biblioteca y feed (de a 50, con next_href)
// ---------------------------------------------------------------------------

static struct {
    NetJob job;
    AccListKind kind;
    char next[1024];     // "" = primera página
    AccPage page;
    char error[96];
} s_acclist;

static void acclist_cancel(void) {
    job_cancel(&s_acclist.job);
}

const char* network_get_account_error(void) {
    return s_acclist.error;
}

static void parse_playlist_result(json_t* pl, PlaylistResult* r) {
    memset(r, 0, sizeof(*r));
    snprintf(r->id, sizeof(r->id), "%lld", (long long)json_integer_value(json_object_get(pl, "id")));
    const char* title = json_str(pl, "title");
    snprintf(r->title, sizeof(r->title), "%s", title ? title : "?");
    const char* creator = json_str(json_object_get(pl, "user"), "username");
    snprintf(r->creator, sizeof(r->creator), "%s", creator ? creator : "");
    json_t* jcount = json_object_get(pl, "track_count");
    r->track_count = json_is_integer(jcount) ? (int)json_integer_value(jcount) : 0;
    const char* art = json_str(pl, "artwork_url");
    if (art) snprintf(r->artwork_url, sizeof(r->artwork_url), "%s", art);
}

static bool load_account_list(AccListKind kind, const char* next, AccPage* out, volatile bool* cancel) {
    memset(out, 0, sizeof(*out));
    char path[128];
    if (next[0]) {
        snprintf(path, sizeof(path), "%s", "");   // se usa next tal cual
    } else if (kind == ACC_LIST_LIKES) {
        snprintf(path, sizeof(path), "/users/%lld/track_likes?limit=%d&linked_partitioning=1", account_user_id(),
                 ACC_PAGE_SIZE);
    } else if (kind == ACC_LIST_PLAYLISTS) {
        snprintf(path, sizeof(path), "/me/library/all?limit=%d&linked_partitioning=1", ACC_PAGE_SIZE);
    } else {
        snprintf(path, sizeof(path), "/stream?limit=%d&linked_partitioning=1", ACC_PAGE_SIZE);
    }
    json_t* root = api_get_json_ex(next[0] ? next : path, PLAYLIST_JSON_MAX, cancel, s_acclist.error,
                                   sizeof(s_acclist.error), true);
    if (!root) {
        dlog("cuenta: lista %d: %s", (int)kind, s_acclist.error);
        return false;
    }
    size_t i;
    json_t* it;
    json_array_foreach(json_object_get(root, "collection"), i, it) {
        if (out->count >= ACC_PAGE_SIZE) break;
        const char* type = json_str(it, "type");
        if (kind == ACC_LIST_PLAYLISTS) {
            // Playlists propias y guardadas; los álbumes y las listas del sistema no
            json_t* pl = json_object_get(it, "playlist");
            if (!type || (strcmp(type, "playlist") != 0 && strcmp(type, "playlist-like") != 0) ||
                !json_is_integer(json_object_get(pl, "id")) || json_is_true(json_object_get(pl, "is_album"))) {
                out->skipped++;
                continue;
            }
            parse_playlist_result(pl, &out->playlists[out->count++]);
        } else {
            // Feed: solo canciones (publicadas o reposteadas); likes: la canción
            bool repost = type && strcmp(type, "track-repost") == 0;
            if (kind == ACC_LIST_FEED && (!type || (strcmp(type, "track") != 0 && !repost))) {
                out->skipped++;
                continue;
            }
            if (!parse_track(json_object_get(it, "track"), &out->tracks[out->count])) {
                out->skipped++;
                continue;
            }
            out->repost[out->count] = repost;
            out->count++;
        }
    }
    out->seen = (int)json_array_size(json_object_get(root, "collection"));
    const char* nh = json_str(root, "next_href");
    // Una página vacía no tiene más detrás aunque traiga next_href
    if (nh && out->seen > 0) snprintf(out->next_href, sizeof(out->next_href), "%s", nh);
    json_decref(root);
    return true;
}

static void acclist_worker(void* arg) {
    s_acclist.job.ok = s_server ? subsonic_account_list(s_acclist.kind, s_acclist.next, &s_acclist.page,
                                                        &s_acclist.job.cancel, s_acclist.error, sizeof(s_acclist.error))
                                : load_account_list(s_acclist.kind, s_acclist.next, &s_acclist.page, &s_acclist.job.cancel);
    s_acclist.job.running = false;
}

bool network_account_list_async(AccListKind kind, const char* next_href) {
    job_cancel(&s_acclist.job);
    if (!s_net.initialized || !(s_server ? subsonic_configured() : account_connected())) return false;
    s_acclist.kind = kind;
    snprintf(s_acclist.next, sizeof(s_acclist.next), "%s", next_href ? next_href : "");
    s_acclist.error[0] = '\0';
    s_acclist.job.ok = false;
    s_acclist.job.running = true;
    s_acclist.job.thread = net_thread_create(acclist_worker);
    if (!s_acclist.job.thread) {
        s_acclist.job.running = false;
        return false;
    }
    return true;
}

NetJobStatus network_poll_account_list(AccListKind* kind, AccPage* out) {
    if (s_acclist.job.running) return NET_JOB_RUNNING;
    if (!s_acclist.job.thread) return NET_JOB_IDLE;
    job_join(&s_acclist.job);
    if (kind) *kind = s_acclist.kind;
    if (!s_acclist.job.ok) return NET_JOB_FAILED;
    if (out) *out = s_acclist.page;
    return NET_JOB_DONE;
}

// Estrella en el servidor propio (un pedido a la vez)
static struct {
    NetJob job;
    char id[32];
    bool star;
} s_star;

static void star_worker(void* arg) {
    char err[96];
    s_star.job.ok = subsonic_star(s_star.id, s_star.star, &s_star.job.cancel, err, sizeof(err));
    if (!s_star.job.ok) dlog("estrella: %s", err);
    s_star.job.running = false;
}

bool network_star_async(const char* song_id, bool star) {
    if (!s_net.initialized || !song_id || !song_id[0] || s_star.job.running) return false;
    job_join(&s_star.job);
    snprintf(s_star.id, sizeof(s_star.id), "%s", song_id);
    s_star.star = star;
    s_star.job.ok = false;
    s_star.job.cancel = false;
    s_star.job.running = true;
    s_star.job.thread = net_thread_create(star_worker);
    if (!s_star.job.thread) {
        s_star.job.running = false;
        return false;
    }
    return true;
}

NetJobStatus network_poll_star(char* id_out, size_t id_sz, bool* starred_out) {
    if (s_star.job.running) return NET_JOB_RUNNING;
    if (!s_star.job.thread) return NET_JOB_IDLE;
    job_join(&s_star.job);
    snprintf(id_out, id_sz, "%s", s_star.id);
    *starred_out = s_star.star;
    return s_star.job.ok ? NET_JOB_DONE : NET_JOB_FAILED;
}

void network_set_server(bool server) {
    if (server == s_server) return;
    job_cancel(&s_search.job);
    playlist_jobs_cancel();
    acclist_cancel();
    job_cancel(&s_star.job);
    s_server = server;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

// Resuelve el endpoint de api-v2 a la URL firmada del CDN (MP3 o lista HLS).
static bool resolve_stream_url(const char* endpoint, char* out, size_t out_sz, volatile bool* cancel) {
    if (!strstr(endpoint, "api-v2.soundcloud.com")) {
        snprintf(out, out_sz, "%s", endpoint);
        return strncmp(out, "http", 4) == 0;
    }

    char resolve_url[1100];
    snprintf(resolve_url, sizeof(resolve_url), "%s?client_id=%s", endpoint, s_client_id);

    MemoryBuffer body;
    if (http_get(resolve_url, 16 * 1024, 10L, cancel, &body, NULL) != CURLE_OK) return false;

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
        resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url), &s_net.stop_requested)) {
        res = stream_mp3_from(s_net.resolved_url);
    }
    return res;
}

// Descarga y prepara la lista HLS (siguiendo una lista maestra si hace falta)
static bool hls_load_playlist(const char* start_url, HlsPlaylist* pl, volatile bool* cancel, char* err, size_t err_sz) {
    char url[1024];
    snprintf(url, sizeof(url), "%s", start_url);

    for (int depth = 0; depth < 2; depth++) {
        MemoryBuffer body;
        CURLcode res = http_get(url, PLAYLIST_MAX, 15L, cancel, &body, NULL);
        if (res != CURLE_OK) {
            if (!*cancel) snprintf(err, err_sz, L("Lista HLS: %s"), curl_easy_strerror(res));
            return false;
        }
        bool ok = hls_parse(body.memory, url, pl);
        free(body.memory);
        if (!ok) {
            snprintf(err, err_sz, "Lista HLS no compatible");
            return false;
        }
        if (!pl->is_master) return true;
        snprintf(url, sizeof(url), "%s", pl->variant_uri);
        hls_free(pl);
    }
    snprintf(err, err_sz, "Lista HLS no compatible");
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
    bool loaded = hls_load_playlist(s_net.resolved_url, &pl, &s_net.stop_requested, s_stream_error, sizeof(s_stream_error));
    if (!loaded && cached && !s_net.stop_requested) {
        // La lista firmada caducó: pedir una nueva
        if (resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url), &s_net.stop_requested))
            loaded = hls_load_playlist(s_net.resolved_url, &pl, &s_net.stop_requested, s_stream_error,
                                       sizeof(s_stream_error));
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
            snprintf(s_stream_error, sizeof(s_stream_error), L("Segmento %d danado"), i);
            res = CURLE_GOT_NOTHING;
            break;
        }
    }

    hls_free(&pl);
    return res;
}

static void stream_worker_func(void* arg) {
    bool cached = s_net.resolved_url[0] != '\0';

    if (!cached && !resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url), &s_net.stop_requested)) {
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
    } else if (strstr(s_net.current_stream_url, "/rest/stream?")) {
        // Servidor propio: el MP3 se genera al vuelo y no tiene tamaño; se pide desde ese segundo
        unsigned secs = (unsigned)((float)duration_ms * ratio / 1000.0f);
        char url[sizeof(s_net.current_stream_url)];
        if (!subsonic_stream_at(s_net.current_stream_url, secs, url, sizeof(url))) return false;
        network_stop_stream();
        snprintf(s_net.current_stream_url, sizeof(s_net.current_stream_url), "%s", url);
        s_net.resolved_url[0] = '\0';
        s_net.start_offset = 0;
        if (start_ms) *start_ms = secs * 1000u;
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
    s_search.job.ok = s_server ? subsonic_search_songs(s_search.query, &s_search.results, &s_search.job.cancel,
                                                       s_search.error, sizeof(s_search.error))
                               : search_tracks(s_search.query, &s_search.results, &s_search.job.cancel);
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

// ---------------------------------------------------------------------------
// Descargas a la tarjeta SD
// ---------------------------------------------------------------------------

#define DL_STATE_PATH  "sdmc:/3ds/sound3ds/downloads.json"
#define DL_STATE_DIR   "sdmc:/3ds/sound3ds"
#define DL_GROUPS_MAX  16

typedef struct {
    SearchResultTrack t;
    char dir[192];           // carpeta destino
    char album[128];
    char album_artist[64];
    char art_url[512];       // carátula para la etiqueta ("" = la de la pista)
    int track_no;            // 0 = siguiente número de la carpeta
    char group[24];          // id de la playlist ("" = descarga suelta)
    bool direct;             // archivo desde Sound3DS Share en el PC: t.stream_endpoint es su URL,
                             // t.title el nombre del archivo; se guarda tal cual, con su etiqueta
} DlItem;

// Avance de una playlist en la cola
typedef struct {
    char id[48];
    char name[128];
    int total, ok, skipped, remaining;
} DlGroup;

typedef enum { DLO_OK, DLO_ALREADY, DLO_PREVIEW, DLO_FAILED, DLO_NET_DOWN, DLO_NO_SPACE, DLO_CANCELLED } DlOutcome;

static struct {
    NetJob job;
    LightLock lock;
    bool lock_ready;
    DlItem* items;           // la cola; items[0] es la que se baja
    int count, cap;
    DlGroup groups[DL_GROUPS_MAX];
    int group_count;
    DlItem current;          // copia de items[0] para el hilo
    volatile bool active;
    volatile bool paused;    // sin red, sin espacio o pendiente de una sesión anterior
    volatile int percent;
    char message[128];
    volatile bool message_new;
    volatile bool library_changed;
} s_dl;

static void dl_lock_init(void) {
    if (!s_dl.lock_ready) {
        LightLock_Init(&s_dl.lock);
        s_dl.lock_ready = true;
    }
}

// Texto seguro para FAT: sin caracteres prohibidos, sin cortar UTF-8, sin espacio o punto al final
static void dl_sanitize(const char* raw, char* out, size_t out_sz, size_t max_len) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)raw; *p && o + 1 < out_sz && o < max_len; p++) {
        unsigned char c = *p;
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) c = '_';
        out[o++] = (char)c;
    }
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;
    if (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0xC0) o--;
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = '\0';
}

// Nombre de archivo: "Artista - Título"
static void dl_file_name(const SearchResultTrack* t, char* out, size_t out_sz) {
    char raw[200];
    if (t->artist[0]) snprintf(raw, sizeof(raw), "%s - %s", t->artist, t->title);
    else snprintf(raw, sizeof(raw), "%s", t->title);
    dl_sanitize(raw, out, out_sz, 110);
    if (!out[0]) snprintf(out, out_sz, "cancion_%s", t->id[0] ? t->id : "sc");
}

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// Elige el archivo destino. Si ya hay un archivo con ese nombre se mira el ID de SoundCloud de
// su etiqueta: si es la misma pista, ya está (false + *already); si no (otra versión con el
// mismo nombre, o una descarga antigua sin ID), se prueba "Nombre (2)", "(3)"...
static bool dl_pick_path(const DlItem* it, char* path, size_t path_sz, bool* already) {
    char base[128], id[32];
    if (it->direct) {
        // Del PC: mismo nombre que en el PC; si ya está en la SD se salta
        snprintf(path, path_sz, "%s/%s", it->dir, it->t.title);
        *already = file_exists(path);
        return !*already;
    }
    const char* ext = it->t.kind == STREAM_HLS_AAC ? "aac" : "mp3";
    dl_file_name(&it->t, base, sizeof(base));
    *already = false;
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(path, path_sz, "%s/%s.%s", it->dir, base, ext);
        else snprintf(path, path_sz, "%s/%s (%d).%s", it->dir, base, n, ext);
        if (!file_exists(path)) return true;
        if (local_read_sc_id(path, id, sizeof(id)) && strcmp(id, it->t.id) == 0) {
            *already = true;
            return false;
        }
    }
    return false;
}

static void dl_item_single(DlItem* it, const SearchResultTrack* t) {
    memset(it, 0, sizeof(*it));
    it->t = *t;
    snprintf(it->dir, sizeof(it->dir), "%s", LOCAL_SC_DIR);
    snprintf(it->album, sizeof(it->album), "%s", LOCAL_SC_ALBUM);
    snprintf(it->album_artist, sizeof(it->album_artist), "%s", LOCAL_SC_ALBUM);
}

// Carpeta de la playlist: sdmc:/music/SoundCloud/<nombre>
static void dl_playlist_dir(const PlaylistDetail* pl, char* out, size_t out_sz) {
    char name[96];
    dl_sanitize(pl->title, name, sizeof(name), 80);
    if (!name[0]) snprintf(name, sizeof(name), "Playlist %s", pl->id);
    snprintf(out, out_sz, "%s/%s", LOCAL_SC_DIR, name);
}

static void dl_item_playlist(DlItem* it, const PlaylistDetail* pl, int index) {
    memset(it, 0, sizeof(*it));
    it->t = pl->tracks[index];
    dl_playlist_dir(pl, it->dir, sizeof(it->dir));
    snprintf(it->album, sizeof(it->album), "%s", pl->title);
    snprintf(it->album_artist, sizeof(it->album_artist), "%s", pl->creator);
    snprintf(it->art_url, sizeof(it->art_url), "%s", pl->artwork_url);
    it->track_no = pl->position[index];
    snprintf(it->group, sizeof(it->group), "%s", pl->id);
}

static bool dl_item_exists(const DlItem* it) {
    char path[320];
    bool already = false;
    dl_pick_path(it, path, sizeof(path), &already);
    return already;
}

void network_download_path(const SearchResultTrack* t, char* out, size_t out_sz) {
    char name[128];
    dl_file_name(t, name, sizeof(name));
    snprintf(out, out_sz, "%s/%s.%s", LOCAL_SC_DIR, name, t->kind == STREAM_HLS_AAC ? "aac" : "mp3");
}

bool network_download_exists(const SearchResultTrack* t) {
    DlItem it;
    dl_item_single(&it, t);
    return dl_item_exists(&it);
}

bool network_download_exists_in_playlist(const PlaylistDetail* pl, int index) {
    if (!pl || index < 0 || index >= pl->count) return false;
    static DlItem it;   // solo desde el hilo principal
    dl_item_playlist(&it, pl, index);
    return dl_item_exists(&it);
}

static int dl_count_files(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.' && !strstr(e->d_name, ".part") && e->d_type != DT_DIR) n++;
    closedir(d);
    return n;
}

static u64 sd_free_bytes(void) {
    FS_ArchiveResource res;
    if (R_FAILED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD))) return UINT64_MAX;
    return (u64)res.freeClusters * res.clusterSize;
}

// Errores de libcurl que indican que no hay conexión (y no un problema de esa pista)
static bool curl_net_down(CURLcode res) {
    return res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_COULDNT_CONNECT || res == CURLE_OPERATION_TIMEDOUT ||
           res == CURLE_SEND_ERROR || res == CURLE_RECV_ERROR || res == CURLE_GOT_NOTHING;
}

// --- Cola guardada en la SD ---

static void dl_save_locked(void) {
    if (s_dl.count == 0) {
        remove(DL_STATE_PATH);
        return;
    }
    json_t* root = json_object();
    json_t* groups = json_array();
    for (int g = 0; g < s_dl.group_count; g++) {
        const DlGroup* gr = &s_dl.groups[g];
        json_array_append_new(groups, json_pack("{s:s,s:s,s:i,s:i,s:i,s:i}", "id", gr->id, "n", gr->name, "t", gr->total,
                                                "o", gr->ok, "s", gr->skipped, "r", gr->remaining));
    }
    json_t* items = json_array();
    for (int i = 0; i < s_dl.count; i++) {
        const DlItem* it = &s_dl.items[i];
        json_array_append_new(items, json_pack("{s:s,s:s,s:s,s:I,s:s,s:s,s:i,s:b,s:s,s:s,s:s,s:s,s:i,s:s}",
            "id", it->t.id, "ti", it->t.title, "ar", it->t.artist, "du", (json_int_t)it->t.duration_ms,
            "ep", it->t.stream_endpoint, "aw", it->t.artwork_url, "k", (int)it->t.kind, "pv", it->t.is_preview,
            "dir", it->dir, "al", it->album, "aa", it->album_artist, "au", it->art_url, "no", it->track_no,
            "g", it->group));
        if (it->direct) json_object_set_new(json_array_get(items, json_array_size(items) - 1), "dx", json_true());
    }
    json_object_set_new(root, "groups", groups);
    json_object_set_new(root, "items", items);
    mkdir(DL_STATE_DIR, 0777);
    // Primero a un temporal y luego se renombra: un corte nunca deja la cola a medias
    if (json_dump_file(root, DL_STATE_PATH ".tmp", JSON_COMPACT) == 0) {
        remove(DL_STATE_PATH);
        rename(DL_STATE_PATH ".tmp", DL_STATE_PATH);
    }
    json_decref(root);
}

static bool dl_reserve_locked(int n) {
    if (s_dl.count + n <= s_dl.cap) return true;
    int cap = s_dl.cap ? s_dl.cap : 32;
    while (cap < s_dl.count + n) cap *= 2;
    DlItem* grown = (DlItem*)realloc(s_dl.items, (size_t)cap * sizeof(DlItem));
    if (!grown) return false;
    s_dl.items = grown;
    s_dl.cap = cap;
    return true;
}

static void cpy(char* dst, size_t sz, const char* src) {
    snprintf(dst, sz, "%s", src ? src : "");
}

static void dl_load_state(void) {
    json_t* root = json_load_file(DL_STATE_PATH, 0, NULL);
    if (!root) return;
    LightLock_Lock(&s_dl.lock);
    size_t i;
    json_t* v;
    json_array_foreach(json_object_get(root, "groups"), i, v) {
        if (s_dl.group_count >= DL_GROUPS_MAX) break;
        DlGroup* g = &s_dl.groups[s_dl.group_count++];
        cpy(g->id, sizeof(g->id), json_str(v, "id"));
        cpy(g->name, sizeof(g->name), json_str(v, "n"));
        g->total = (int)json_integer_value(json_object_get(v, "t"));
        g->ok = (int)json_integer_value(json_object_get(v, "o"));
        g->skipped = (int)json_integer_value(json_object_get(v, "s"));
        g->remaining = (int)json_integer_value(json_object_get(v, "r"));
    }
    json_t* items = json_object_get(root, "items");
    if (dl_reserve_locked((int)json_array_size(items))) {
        json_array_foreach(items, i, v) {
            DlItem* it = &s_dl.items[s_dl.count];
            memset(it, 0, sizeof(*it));
            cpy(it->t.id, sizeof(it->t.id), json_str(v, "id"));
            cpy(it->t.title, sizeof(it->t.title), json_str(v, "ti"));
            cpy(it->t.artist, sizeof(it->t.artist), json_str(v, "ar"));
            it->t.duration_ms = (uint32_t)json_integer_value(json_object_get(v, "du"));
            cpy(it->t.stream_endpoint, sizeof(it->t.stream_endpoint), json_str(v, "ep"));
            cpy(it->t.artwork_url, sizeof(it->t.artwork_url), json_str(v, "aw"));
            it->t.kind = (StreamKind)json_integer_value(json_object_get(v, "k"));
            it->t.is_preview = json_is_true(json_object_get(v, "pv"));
            cpy(it->dir, sizeof(it->dir), json_str(v, "dir"));
            cpy(it->album, sizeof(it->album), json_str(v, "al"));
            cpy(it->album_artist, sizeof(it->album_artist), json_str(v, "aa"));
            cpy(it->art_url, sizeof(it->art_url), json_str(v, "au"));
            it->track_no = (int)json_integer_value(json_object_get(v, "no"));
            cpy(it->group, sizeof(it->group), json_str(v, "g"));
            it->direct = json_is_true(json_object_get(v, "dx"));
            if (it->t.id[0] && it->t.stream_endpoint[0] && it->dir[0]) s_dl.count++;
        }
    }
    // Esperan a que el usuario elija reanudar o descartar
    s_dl.paused = s_dl.count > 0;
    LightLock_Unlock(&s_dl.lock);
    json_decref(root);
    dlog("descargas pendientes: %d", s_dl.count);
}

static DlGroup* dl_group_locked(const char* id) {
    for (int g = 0; g < s_dl.group_count; g++)
        if (strcmp(s_dl.groups[g].id, id) == 0) return &s_dl.groups[g];
    return NULL;
}

// --- Transferencia de una pista ---

typedef struct {
    FILE* f;
    uint8_t head[10];
    size_t head_len;
    u64 skip;          // bytes de una etiqueta ID3 de origen que se descartan
    bool head_done;
    bool write_error;
} DlMp3Ctx;

static size_t dl_mp3_write(void* contents, size_t size, size_t nmemb, void* userp) {
    DlMp3Ctx* c = (DlMp3Ctx*)userp;
    size_t len = size * nmemb;
    const uint8_t* p = (const uint8_t*)contents;
    size_t left = len;
    // Si el MP3 ya trae una etiqueta ID3v2, se reemplaza por la nuestra
    while (!c->head_done && left > 0) {
        c->head[c->head_len++] = *p++;
        left--;
        if (c->head_len == 10) {
            c->head_done = true;
            if (memcmp(c->head, "ID3", 3) == 0) {
                c->skip = ((u64)(c->head[6] & 0x7F) << 21 | (c->head[7] & 0x7F) << 14 | (c->head[8] & 0x7F) << 7 |
                           (c->head[9] & 0x7F)) + ((c->head[5] & 0x10) ? 10 : 0);
            } else if (fwrite(c->head, 1, 10, c->f) != 10) {
                c->write_error = true;
                return 0;
            }
        }
    }
    if (c->skip > 0) {
        size_t s = left < c->skip ? left : (size_t)c->skip;
        c->skip -= s;
        p += s;
        left -= s;
    }
    if (left > 0 && fwrite(p, 1, left, c->f) != left) {
        c->write_error = true;
        return 0;
    }
    return len;
}

static int dl_xferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    if (dltotal > 0) s_dl.percent = (int)(dlnow * 100 / dltotal);
    return s_dl.job.cancel ? 1 : 0;
}

static bool dl_mp3(const char* url, FILE* f, char* err, size_t err_sz, bool* net_down, bool* write_error) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    DlMp3Ctx ctx = { .f = f };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_mp3_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&ctx);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    set_common_options(curl, &s_dl.job.cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_xferinfo);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    *write_error = ctx.write_error;
    if (ctx.write_error) snprintf(err, err_sz, "No se pudo escribir en la SD");
    else if (res != CURLE_OK && !s_dl.job.cancel) {
        snprintf(err, err_sz, L("Red: %s"), curl_easy_strerror(res));
        *net_down = curl_net_down(res);
    }
    return res == CURLE_OK && !ctx.write_error;
}

typedef struct {
    Fmp4AudioConfig cfg;
    FILE* f;
    bool error;
} DlHlsCtx;

static bool dl_hls_frame(const uint8_t* frame, size_t size, void* user) {
    DlHlsCtx* c = (DlHlsCtx*)user;
    uint8_t hdr[7];
    fmp4_adts_header(hdr, &c->cfg, size);
    if (fwrite(hdr, 1, 7, c->f) != 7 || fwrite(frame, 1, size, c->f) != size) {
        c->error = true;
        return false;
    }
    return !s_dl.job.cancel;
}

static bool dl_hls(const char* url, FILE* f, char* err, size_t err_sz, bool* net_down, bool* write_error) {
    HlsPlaylist pl;
    if (!hls_load_playlist(url, &pl, &s_dl.job.cancel, err, err_sz)) return false;
    if (!pl.init_uri) {
        snprintf(err, err_sz, "HLS sin segmento de inicio");
        hls_free(&pl);
        return false;
    }
    DlHlsCtx ctx = { .f = f };
    MemoryBuffer seg;
    CURLcode res = http_get(pl.init_uri, 64 * 1024, 15L, &s_dl.job.cancel, &seg, NULL);
    bool ok = res == CURLE_OK && fmp4_parse_init((const uint8_t*)seg.memory, seg.size, &ctx.cfg);
    free(seg.memory);
    if (!ok) {
        if (res != CURLE_OK) *net_down = curl_net_down(res);
        snprintf(err, err_sz, "Formato de audio no compatible");
        hls_free(&pl);
        return false;
    }
    for (int i = 0; i < pl.count && ok && !s_dl.job.cancel; i++) {
        s_dl.percent = i * 100 / pl.count;
        res = http_get(pl.segments[i].uri, SEGMENT_MAX, 60L, &s_dl.job.cancel, &seg, NULL);
        if (res != CURLE_OK) {
            if (!s_dl.job.cancel) {
                snprintf(err, err_sz, L("Red: %s"), curl_easy_strerror(res));
                *net_down = curl_net_down(res);
            }
            ok = false;
            break;
        }
        int frames = fmp4_parse_segment((const uint8_t*)seg.memory, seg.size, &ctx.cfg, dl_hls_frame, &ctx);
        free(seg.memory);
        if (ctx.error) {
            snprintf(err, err_sz, "No se pudo escribir en la SD");
            *write_error = true;
            ok = false;
        } else if (frames < 0 && !s_dl.job.cancel) {
            snprintf(err, err_sz, L("Segmento %d dañado"), i);
            ok = false;
        }
    }
    hls_free(&pl);
    return ok && !s_dl.job.cancel;
}

static size_t dl_raw_write(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t len = size * nmemb;
    return fwrite(contents, 1, len, (FILE*)userp) == len ? len : 0;
}

// Canción desde Sound3DS Share: el PC ya le puso etiqueta y carátula, se copia tal cual
static DlOutcome dl_direct(const DlItem* it, char* msg, size_t msg_sz) {
    mkdir(LOCAL_MUSIC_HINT, 0777);
    mkdir(it->dir, 0777);
    char path[320], part[330];
    bool already = false;
    if (!dl_pick_path(it, path, sizeof(path), &already)) {
        snprintf(msg, msg_sz, L("Ya estaba en la SD: %.60s"), it->t.title);
        return DLO_ALREADY;
    }
    if (sd_free_bytes() < 4 * 1024 * 1024) return DLO_NO_SPACE;
    snprintf(part, sizeof(part), "%s.part", path);
    FILE* f = fopen(part, "wb");
    if (!f) {
        snprintf(msg, msg_sz, "No se pudo crear el archivo en la SD");
        return DLO_FAILED;
    }
    // Escribir a la SD en bloques grandes: con los 16 KB por defecto de curl va mucho más lento
    setvbuf(f, NULL, _IOFBF, 128 * 1024);
    CURL* curl = curl_easy_init();
    CURLcode res = CURLE_FAILED_INIT;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, it->t.stream_endpoint);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_raw_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)f);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 64L * 1024);
        set_common_options(curl, &s_dl.job.cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_xferinfo);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    bool closed = fclose(f) == 0;
    if (res == CURLE_OK && closed && rename(part, path) == 0) {
        snprintf(msg, msg_sz, L("Descargada: %.70s"), it->t.title);
        return DLO_OK;
    }
    remove(part);
    if (s_dl.job.cancel) return DLO_CANCELLED;
    if (res == CURLE_WRITE_ERROR || !closed) {
        if (sd_free_bytes() < 2 * 1024 * 1024) return DLO_NO_SPACE;
        snprintf(msg, msg_sz, "No se pudo escribir en la SD");
        return DLO_FAILED;
    }
    if (curl_net_down(res)) {
        // ¿Sin Wi-Fi, o solo se cerró Sound3DS Share? Con internet, es el PC: se salta
        MemoryBuffer probe;
        CURLcode pr = http_get("https://api-v2.soundcloud.com/", 4096, 10L, &s_dl.job.cancel, &probe, NULL);
        free(probe.memory);
        if (curl_net_down(pr)) return DLO_NET_DOWN;
    }
    snprintf(msg, msg_sz, L("No se pudo bajar del PC: %.50s"), it->t.title);
    return DLO_FAILED;
}

// Descarga una pista con su etiqueta: título, artista, álbum, número, duración, carátula e ID
static DlOutcome dl_one(const DlItem* it, char* msg, size_t msg_sz) {
    if (it->direct) return dl_direct(it, msg, msg_sz);
    const SearchResultTrack* t = &it->t;
    if (t->is_preview) return DLO_PREVIEW;
    mkdir(LOCAL_MUSIC_HINT, 0777);
    mkdir(LOCAL_SC_DIR, 0777);
    mkdir(it->dir, 0777);

    char path[320], part[330], err[64] = "";
    bool already = false;
    if (!dl_pick_path(it, path, sizeof(path), &already)) {
        snprintf(msg, msg_sz, L("Ya estaba descargada: %.60s"), t->title);
        return already ? DLO_ALREADY : DLO_FAILED;
    }
    snprintf(part, sizeof(part), "%s.part", path);

    // Espacio: unos 24 KB por segundo de audio, más un margen
    u64 need = (u64)(t->duration_ms / 1000 + 1) * 24 * 1024 + 2 * 1024 * 1024;
    if (sd_free_bytes() < need) return DLO_NO_SPACE;

    char url[1024];
    if (!resolve_stream_url(t->stream_endpoint, url, sizeof(url), &s_dl.job.cancel)) {
        if (s_dl.job.cancel) return DLO_CANCELLED;
        // ¿Sin red, o solo esta pista? Se prueba la API con una petición mínima
        MemoryBuffer probe;
        CURLcode pr = http_get("https://api-v2.soundcloud.com/", 4096, 10L, &s_dl.job.cancel, &probe, NULL);
        free(probe.memory);
        if (curl_net_down(pr)) return DLO_NET_DOWN;
        snprintf(msg, msg_sz, L("No se pudo obtener: %.60s"), t->title);
        return DLO_FAILED;
    }

    // Carátula en 300x300 para la etiqueta: la de la playlist o la de la pista
    MemoryBuffer art = { 0 };
    const char* src_art = it->art_url[0] ? it->art_url : t->artwork_url;
    if (src_art[0] && strcmp(src_art, "null") != 0) {
        char art_url[560];
        art_url_t300(src_art, art_url, sizeof(art_url));
        if (http_get(art_url, 512 * 1024, 15L, &s_dl.job.cancel, &art, NULL) != CURLE_OK)
            http_get(src_art, 512 * 1024, 15L, &s_dl.job.cancel, &art, NULL);
    }

    FILE* f = fopen(part, "wb");
    if (!f) {
        free(art.memory);
        if (sd_free_bytes() < 1024 * 1024) return DLO_NO_SPACE;
        snprintf(msg, msg_sz, "No se pudo crear el archivo en la SD");
        return DLO_FAILED;
    }
    LocalTagInfo tag = {
        .title = t->title,
        .artist = t->artist,
        .album = it->album,
        .album_artist = it->album_artist,
        .track_no = it->track_no > 0 ? it->track_no : dl_count_files(it->dir) + 1,
        .duration_ms = t->duration_ms,
        .jpeg = (const uint8_t*)art.memory,
        .jpeg_len = art.memory ? art.size : 0,
        .sc_id = t->id,
    };
    bool ok = local_write_id3(f, &tag);
    bool write_error = !ok, net_down = false;
    free(art.memory);
    if (ok) ok = t->kind == STREAM_HLS_AAC ? dl_hls(url, f, err, sizeof(err), &net_down, &write_error)
                                         : dl_mp3(url, f, err, sizeof(err), &net_down, &write_error);
    if (fclose(f) != 0) {
        ok = false;
        write_error = true;
    }

    if (ok && rename(part, path) == 0) {
        snprintf(msg, msg_sz, L("Descargada: %.70s"), t->title);
        return DLO_OK;
    }
    remove(part);
    if (s_dl.job.cancel) return DLO_CANCELLED;
    if (write_error && sd_free_bytes() < 2 * 1024 * 1024) return DLO_NO_SPACE;
    if (net_down) return DLO_NET_DOWN;
    snprintf(msg, msg_sz, L("Falló la descarga%s%s"), err[0] ? ": " : "", err);
    return DLO_FAILED;
}

// Cuenta el resultado en su playlist y avisa al terminarla
static void dl_finish_item_locked(const DlItem* it, DlOutcome r, const char* msg) {
    DlGroup* g = it->group[0] ? dl_group_locked(it->group) : NULL;
    if (!g) {
        if (r != DLO_PREVIEW) {
            snprintf(s_dl.message, sizeof(s_dl.message), "%s", msg);
            s_dl.message_new = true;
        }
        return;
    }
    if (r == DLO_OK || r == DLO_ALREADY) g->ok++;
    else g->skipped++;
    if (--g->remaining > 0) return;
    if (strncmp(g->id, "pc", 2) == 0) {
        if (g->skipped == 0) snprintf(s_dl.message, sizeof(s_dl.message), L("Desde el PC: %d canciones listas en Local"), g->ok);
        else snprintf(s_dl.message, sizeof(s_dl.message), L("Desde el PC: %d de %d descargadas"), g->ok, g->total);
        s_dl.message_new = true;
        *g = s_dl.groups[--s_dl.group_count];
        return;
    }
    if (g->skipped == 0) snprintf(s_dl.message, sizeof(s_dl.message), L("Playlist %.60s: %d canciones descargadas"), g->name, g->ok);
    else snprintf(s_dl.message, sizeof(s_dl.message), L("Playlist %.50s: %d de %d descargadas"), g->name, g->ok, g->total);
    s_dl.message_new = true;
    *g = s_dl.groups[--s_dl.group_count];
}

static void dl_worker(void* arg) {
    for (;;) {
        LightLock_Lock(&s_dl.lock);
        bool have = s_dl.count > 0 && !s_dl.job.cancel && !s_dl.paused;
        if (have) {
            s_dl.current = s_dl.items[0];
            s_dl.percent = 0;
            s_dl.active = true;
        }
        LightLock_Unlock(&s_dl.lock);
        if (!have) break;

        char msg[128] = "";
        DlOutcome r = dl_one(&s_dl.current, msg, sizeof(msg));
        dlog("descarga %s: %d %s", s_dl.current.t.id, (int)r, msg);

        LightLock_Lock(&s_dl.lock);
        s_dl.active = false;
        if (r == DLO_CANCELLED) {
            // Al salir de la app: la pista queda en la cola para la próxima vez
            LightLock_Unlock(&s_dl.lock);
            break;
        }
        if (r == DLO_NET_DOWN || r == DLO_NO_SPACE) {
            s_dl.paused = true;
            snprintf(s_dl.message, sizeof(s_dl.message), "%s",
                     r == DLO_NO_SPACE ? "Sin espacio en la SD: descargas en pausa" : "Sin conexión: descargas en pausa");
            s_dl.message_new = true;
            LightLock_Unlock(&s_dl.lock);
            break;
        }
        if (r == DLO_OK) s_dl.library_changed = true;
        if (s_dl.count > 0) {
            memmove(&s_dl.items[0], &s_dl.items[1], (size_t)(s_dl.count - 1) * sizeof(DlItem));
            s_dl.count--;
        }
        dl_finish_item_locked(&s_dl.current, r, msg);
        dl_save_locked();
        LightLock_Unlock(&s_dl.lock);
    }
    s_dl.job.running = false;
}

static bool dl_start_worker(void) {
    if (s_dl.job.running) return true;
    job_join(&s_dl.job);
    s_dl.job.cancel = false;
    s_dl.job.running = true;
    s_dl.job.thread = net_thread_create(dl_worker);
    if (!s_dl.job.thread) {
        s_dl.job.running = false;
        return false;
    }
    return true;
}

static bool dl_in_queue_locked(const DlItem* it) {
    for (int i = 0; i < s_dl.count; i++)
        if (strcmp(s_dl.items[i].t.id, it->t.id) == 0 && strcmp(s_dl.items[i].dir, it->dir) == 0) return true;
    return false;
}

static DownloadResult dl_enqueue_item(const DlItem* it) {
    if (!s_net.initialized) return DL_REJECTED;
    if (it->t.is_preview) return DL_PREVIEW_ONLY;
    if (dl_item_exists(it)) return DL_ALREADY_THERE;
    dl_lock_init();
    LightLock_Lock(&s_dl.lock);
    DownloadResult r = DL_QUEUED;
    if (dl_in_queue_locked(it)) r = DL_ALREADY_QUEUED;
    else if (!dl_reserve_locked(1)) r = DL_REJECTED;
    else {
        s_dl.items[s_dl.count++] = *it;
        DlGroup* g = it->group[0] ? dl_group_locked(it->group) : NULL;
        if (g) {
            g->total++;
            g->remaining++;
        }
        s_dl.paused = false;
        dl_save_locked();
    }
    LightLock_Unlock(&s_dl.lock);
    if (r == DL_QUEUED && !dl_start_worker()) return DL_REJECTED;
    return r;
}

DownloadResult network_download_enqueue(const SearchResultTrack* t) {
    if (!t) return DL_REJECTED;
    DlItem it;
    dl_item_single(&it, t);
    return dl_enqueue_item(&it);
}

DownloadResult network_download_enqueue_from_playlist(const PlaylistDetail* pl, int index) {
    if (!pl || index < 0 || index >= pl->count) return DL_REJECTED;
    static DlItem it;
    dl_item_playlist(&it, pl, index);
    it.group[0] = '\0';   // una sola canción: aviso como descarga suelta
    return dl_enqueue_item(&it);
}

int network_download_enqueue_playlist(const PlaylistDetail* pl, int* skipped) {
    if (skipped) *skipped = 0;
    if (!s_net.initialized || !pl || pl->count == 0) return 0;
    dl_lock_init();
    static DlItem it;
    int queued = 0, skip = 0;
    LightLock_Lock(&s_dl.lock);
    DlGroup* g = dl_group_locked(pl->id);
    if (!g && s_dl.group_count < DL_GROUPS_MAX) {
        g = &s_dl.groups[s_dl.group_count++];
        memset(g, 0, sizeof(*g));
        snprintf(g->id, sizeof(g->id), "%s", pl->id);
        snprintf(g->name, sizeof(g->name), "%s", pl->title);
    }
    for (int i = 0; i < pl->count; i++) {
        dl_item_playlist(&it, pl, i);
        if (dl_in_queue_locked(&it)) continue;
        if (it.t.is_preview || dl_item_exists(&it)) {
            skip++;
            continue;
        }
        if (!dl_reserve_locked(1)) break;
        if (!g) it.group[0] = '\0';
        s_dl.items[s_dl.count++] = it;
        if (g) {
            g->total++;
            g->remaining++;
        }
        queued++;
    }
    if (g && g->remaining == 0) *g = s_dl.groups[--s_dl.group_count];   // nada nuevo que bajar
    if (queued > 0) {
        s_dl.paused = false;
        dl_save_locked();
    }
    LightLock_Unlock(&s_dl.lock);
    if (queued > 0) dl_start_worker();
    if (skipped) *skipped = skip;
    return queued;
}

// ---------------------------------------------------------------------------
// Recibir desde el PC (QR de Sound3DS Share): canciones o la cuenta de SoundCloud
// ---------------------------------------------------------------------------

#define RECV_LIST_MAX (256 * 1024)
#define RECV_URL_LEN  512

static struct {
    NetJob job;
    char qr[1024];
    ReceiveResult result;
} s_recv;

// ¿El QR es el enlace de un solo uso de la cuenta: http://<ip>:<puerto>/account/<código>?
// Enlace de un solo uso de Sound3DS Share: http://<pc>:<puerto>/<tipo>/<código>
static bool recv_is_link(const char* qr, const char* kind) {
    if (strncmp(qr, "http://", 7) != 0) return false;
    char seg[24];
    snprintf(seg, sizeof(seg), "/%s/", kind);
    const char* p = strstr(qr + 7, seg);
    if (!p || p != strchr(qr + 7, '/')) return false;
    p += strlen(seg);
    size_t n = strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
    return n >= 16 && n <= 64 && (p[n] == 0 || p[n] == '\r' || p[n] == '\n');
}

static bool recv_is_account(const char* qr) {
    return recv_is_link(qr, "account");
}

static bool recv_is_server(const char* qr) {
    return recv_is_link(qr, "server");
}

static bool recv_audio_ext(const char* url) {
    size_t n = strcspn(url, "?#");
    return n > 4 && (strncasecmp(url + n - 4, ".mp3", 4) == 0 || strncasecmp(url + n - 4, ".aac", 4) == 0);
}

// Nombre del archivo desde la URL: último tramo, %XX decodificado y sin caracteres que FAT no acepta
static void recv_file_name(const char* url, char* out, size_t out_sz) {
    size_t n = strcspn(url, "?#");
    const char* start = url;
    for (size_t i = 0; i < n; i++)
        if (url[i] == '/') start = url + i + 1;
    char raw[160];
    size_t o = 0;
    for (const char* p = start; p < url + n && o + 1 < sizeof(raw); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' && p + 2 < url + n) {
            char hex[3] = { p[1], p[2], 0 };
            c = (unsigned char)strtol(hex, NULL, 16);
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        raw[o++] = (char)c;
    }
    raw[o] = '\0';
    dl_sanitize(raw, out, out_sz, 120);
    if (!out[0]) snprintf(out, out_sz, "cancion.mp3");
}

// Línea de la lista -> URL absoluta (relativa a la URL de la lista si hace falta)
static void recv_absolute(const char* base, const char* line, char* out, size_t out_sz) {
    if (strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
        snprintf(out, out_sz, "%s", line);
        return;
    }
    const char* scheme = strstr(base, "://");
    const char* host_end = scheme ? strchr(scheme + 3, '/') : NULL;
    if (line[0] == '/') {
        int n = host_end ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s", n, base, line);
    } else {
        const char* slash = strrchr(base, '/');
        int n = slash && scheme && slash > scheme + 2 ? (int)(slash - base + 1) : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s%s", n, base, slash ? "" : "/", line);
    }
}

static u32 fnv1a(const char* s) {
    u32 h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

// Pone una canción del PC en la cola, dentro del grupo del envío
static void recv_add_song(const char* url, const char* group) {
    static DlItem it;   // solo desde el hilo de recepción
    memset(&it, 0, sizeof(it));
    it.direct = true;
    recv_file_name(url, it.t.title, sizeof(it.t.title));
    snprintf(it.t.id, sizeof(it.t.id), "pc:%08lx", (unsigned long)fnv1a(it.t.title));
    snprintf(it.t.stream_endpoint, sizeof(it.t.stream_endpoint), "%s", url);
    snprintf(it.dir, sizeof(it.dir), "%s", LOCAL_MUSIC_HINT);
    snprintf(it.group, sizeof(it.group), "%s", group);
    s_recv.result.found++;
    if (dl_item_exists(&it)) {
        s_recv.result.existing++;
        return;
    }
    LightLock_Lock(&s_dl.lock);
    if (!dl_in_queue_locked(&it) && dl_reserve_locked(1)) {
        DlGroup* g = dl_group_locked(group);
        if (!g) it.group[0] = '\0';
        s_dl.items[s_dl.count++] = it;
        if (g) {
            g->total++;
            g->remaining++;
        }
        s_recv.result.queued++;
    }
    LightLock_Unlock(&s_dl.lock);
}

static void recv_songs(void) {
    ReceiveResult* r = &s_recv.result;
    r->kind = RECV_SONGS;
    dl_lock_init();
    char group[24];
    snprintf(group, sizeof(group), "pc%llu", (unsigned long long)osGetTime());
    LightLock_Lock(&s_dl.lock);
    if (s_dl.group_count < DL_GROUPS_MAX) {
        DlGroup* g = &s_dl.groups[s_dl.group_count++];
        memset(g, 0, sizeof(*g));
        snprintf(g->id, sizeof(g->id), "%s", group);
        snprintf(g->name, sizeof(g->name), "Sound3DS Share");
    }
    LightLock_Unlock(&s_dl.lock);

    bool list_failed = false;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", s_recv.qr);
    char* save = NULL;
    for (char* line = strtok_r(buf, "\r\n", &save); line && !s_recv.job.cancel; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ') line++;
        if (strncmp(line, "http", 4) != 0) continue;
        if (recv_audio_ext(line)) {
            recv_add_song(line, group);
            continue;
        }
        // No es una canción: es la lista que sirve Sound3DS Share (una URL por línea)
        MemoryBuffer body;
        long code = 0;
        if (http_get(line, RECV_LIST_MAX, 20L, &s_recv.job.cancel, &body, &code) != CURLE_OK) {
            list_failed = true;
            continue;
        }
        char* save2 = NULL;
        for (char* l = strtok_r(body.memory, "\r\n", &save2); l; l = strtok_r(NULL, "\r\n", &save2)) {
            while (*l == ' ') l++;
            if (!*l || *l == '#') continue;
            char abs[RECV_URL_LEN];
            recv_absolute(line, l, abs, sizeof(abs));
            if (recv_audio_ext(abs)) recv_add_song(abs, group);
        }
        free(body.memory);
    }

    LightLock_Lock(&s_dl.lock);
    DlGroup* g = dl_group_locked(group);
    if (g && g->remaining == 0) *g = s_dl.groups[--s_dl.group_count];   // nada nuevo que bajar
    if (r->queued > 0) {
        s_dl.paused = false;
        dl_save_locked();
    }
    LightLock_Unlock(&s_dl.lock);
    if (r->queued > 0) dl_start_worker();

    if (r->found == 0)
        snprintf(r->error, sizeof(r->error), "%s",
                 list_failed ? "No se pudo leer la lista del PC" : "El QR no trae canciones .mp3 ni .aac");
    r->ok = r->found > 0;
}

static void recv_account(void) {
    ReceiveResult* r = &s_recv.result;
    r->kind = RECV_ACCOUNT;
    char url[RECV_URL_LEN];
    snprintf(url, sizeof(url), "%.*s", (int)strcspn(s_recv.qr, "\r\n"), s_recv.qr);
    MemoryBuffer body;
    long code = 0;
    CURLcode res = http_get(url, 64 * 1024, 20L, &s_recv.job.cancel, &body, &code);
    if (res != CURLE_OK) {
        // 404: el enlace ya se usó o venció; lo demás es de red
        snprintf(r->error, sizeof(r->error), "%s",
                 code == 404 ? "El QR ya se usó o venció. Toca Conectar SoundCloud de nuevo."
                             : "No se pudo conectar con el PC. ¿Misma red Wi-Fi?");
        return;
    }
    r->ok = account_import(body.memory, r->username, sizeof(r->username), r->error, sizeof(r->error));
    wipe_mem(body.memory, body.size);   // tenía los tokens
    free(body.memory);
}

// Servidor propio: el enlace entrega la URL, el usuario y el token (nunca la contraseña)
static void recv_server(void) {
    ReceiveResult* r = &s_recv.result;
    r->kind = RECV_SERVER;
    char url[RECV_URL_LEN];
    snprintf(url, sizeof(url), "%.*s", (int)strcspn(s_recv.qr, "\r\n"), s_recv.qr);
    MemoryBuffer body;
    long code = 0;
    CURLcode res = http_get(url, 16 * 1024, 20L, &s_recv.job.cancel, &body, &code);
    if (res != CURLE_OK) {
        snprintf(r->error, sizeof(r->error), "%s",
                 code == 404 ? "El QR ya se usó o venció. Genera uno nuevo en Share."
                             : "No se pudo conectar con el PC. ¿Misma red Wi-Fi?");
        return;
    }
    r->ok = subsonic_import(body.memory, r->username, sizeof(r->username), r->error, sizeof(r->error));
    wipe_mem(body.memory, body.size);   // tenía el token
    free(body.memory);
}

static void recv_worker(void* arg) {
    memset(&s_recv.result, 0, sizeof(s_recv.result));
    if (recv_is_server(s_recv.qr)) recv_server();
    else if (recv_is_account(s_recv.qr)) recv_account();
    else recv_songs();
    dlog("recibir: tipo %d ok %d, %d canciones (%d en cola, %d ya estaban)", (int)s_recv.result.kind, (int)s_recv.result.ok,
         s_recv.result.found, s_recv.result.queued, s_recv.result.existing);
    s_recv.job.running = false;
}

bool network_receive_async(const char* qr_text) {
    if (!s_net.initialized || !qr_text || !qr_text[0] || s_recv.job.running) return false;
    job_join(&s_recv.job);
    snprintf(s_recv.qr, sizeof(s_recv.qr), "%s", qr_text);
    s_recv.job.cancel = false;
    s_recv.job.running = true;
    s_recv.job.thread = net_thread_create(recv_worker);
    if (!s_recv.job.thread) {
        s_recv.job.running = false;
        return false;
    }
    return true;
}

bool network_receive_is_account(const char* qr_text) {
    return qr_text && recv_is_account(qr_text);
}

bool network_receive_is_server(const char* qr_text) {
    return qr_text && recv_is_server(qr_text);
}

NetJobStatus network_poll_receive(ReceiveResult* out) {
    if (s_recv.job.running) return NET_JOB_RUNNING;
    if (!s_recv.job.thread) return NET_JOB_IDLE;
    job_join(&s_recv.job);
    if (out) *out = s_recv.result;
    return s_recv.result.ok ? NET_JOB_DONE : NET_JOB_FAILED;
}

static void recv_cancel(void) {
    job_cancel(&s_recv.job);
}

bool network_download_group_progress(const char* playlist_id, int* done, int* total) {
    if (!s_dl.lock_ready || !playlist_id || !playlist_id[0]) return false;
    LightLock_Lock(&s_dl.lock);
    DlGroup* g = dl_group_locked(playlist_id);
    if (g) {
        *done = g->ok + g->skipped;
        *total = g->total;
    }
    LightLock_Unlock(&s_dl.lock);
    return g != NULL;
}

bool network_download_status(char* title, size_t title_sz, int* percent, int* queued) {
    if (!s_dl.lock_ready) return false;
    LightLock_Lock(&s_dl.lock);
    bool active = s_dl.active;
    if (title) snprintf(title, title_sz, "%s", active ? s_dl.current.t.title : "");
    if (percent) *percent = s_dl.percent;
    int waiting = s_dl.count - (active ? 1 : 0);
    if (queued) *queued = waiting > 0 ? waiting : 0;
    LightLock_Unlock(&s_dl.lock);
    return active;
}

bool network_download_take_message(char* out, size_t out_sz) {
    if (!s_dl.lock_ready || !s_dl.message_new) return false;
    LightLock_Lock(&s_dl.lock);
    snprintf(out, out_sz, "%s", s_dl.message);
    s_dl.message_new = false;
    LightLock_Unlock(&s_dl.lock);
    return true;
}

bool network_download_take_library_changed(void) {
    if (!s_dl.library_changed) return false;
    s_dl.library_changed = false;
    return true;
}

bool network_download_paused(void) {
    return s_dl.paused && s_dl.count > 0;
}

int network_download_pending(char* summary, size_t summary_sz) {
    if (!s_dl.lock_ready) return 0;
    LightLock_Lock(&s_dl.lock);
    int n = s_dl.count;
    if (summary && summary_sz) {
        if (s_dl.group_count == 1 && s_dl.groups[0].remaining == n) {
            const DlGroup* g = &s_dl.groups[0];
            snprintf(summary, summary_sz, L("%.60s (%d de %d)"), g->name, g->ok + g->skipped, g->total);
        } else {
            snprintf(summary, summary_sz, L("%d canciones en cola"), n);
        }
    }
    LightLock_Unlock(&s_dl.lock);
    return n;
}

void network_download_resume(void) {
    if (!s_dl.lock_ready) return;
    LightLock_Lock(&s_dl.lock);
    s_dl.paused = false;
    bool any = s_dl.count > 0;
    LightLock_Unlock(&s_dl.lock);
    if (any) dl_start_worker();
}

void network_download_discard(void) {
    if (!s_dl.lock_ready) return;
    job_cancel(&s_dl.job);
    LightLock_Lock(&s_dl.lock);
    // Lo que quedó a medias de la primera
    if (s_dl.count > 0) {
        char path[320], part[330];
        bool already;
        if (dl_pick_path(&s_dl.items[0], path, sizeof(path), &already)) {
            snprintf(part, sizeof(part), "%s.part", path);
            remove(part);
        }
    }
    s_dl.count = 0;
    s_dl.group_count = 0;
    s_dl.paused = false;
    dl_save_locked();
    LightLock_Unlock(&s_dl.lock);
}

void network_download_stop(void) {
    if (!s_dl.lock_ready) return;
    job_cancel(&s_dl.job);
}
