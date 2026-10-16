#include "network.h"
#include "audio.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <curl/curl.h>

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

static struct {
    bool initialized;
    volatile bool is_streaming;
    volatile bool stop_requested;
    Thread stream_thread;
    char current_stream_url[1024];
    char resolved_url[1024];    // URL directa del CDN, reutilizada al adelantar
    volatile u64 known_total;   // último tamaño completo conocido de la pista
    u64* header_total;          // destino del callback de cabeceras de la petición en curso
    u64 start_offset;           // byte desde el que se pide el stream (Range)
} s_net;

#define NET_THREAD_STACK_SZ (32 * 1024)
#define SEARCH_JSON_MAX     (256 * 1024)
#define SEARCH_LIMIT        20

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

typedef struct {
    char* memory;
    size_t size;
    size_t max_size;
} MemoryBuffer;

static size_t write_memory_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer* mem = (MemoryBuffer*)userp;

    if (mem->size + realsize >= mem->max_size) {
        realsize = mem->max_size - mem->size - 1;
    }

    if (realsize > 0) {
        memcpy(&(mem->memory[mem->size]), contents, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = 0;
    }
    return size * nmemb;
}

// Aborta la transferencia en curso cuando se pide cancelar (libcurl la llama
// periódicamente incluso mientras espera la conexión)
static int abort_xferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow) {
    volatile bool* cancel = (volatile bool*)clientp;
    return (cancel && *cancel) ? 1 : 0;
}

static void set_cancellable(CURL* curl, volatile bool* cancel) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)cancel);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
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

static size_t stream_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    if (s_net.stop_requested) return 0;

    // Las cabeceras ya llegaron: el tamaño total queda disponible para adelantar
    if (s_net.header_total && *s_net.header_total > 0) s_net.known_total = *s_net.header_total;

    while (!s_net.stop_requested && audio_get_buffer_fill_percent() > 80) {
        svcSleepThread(15 * 1000 * 1000LL); // 15 ms
    }
    if (s_net.stop_requested) return 0;

    if (realsize > 0) {
        size_t written = 0;
        while (!s_net.stop_requested && written < realsize) {
            size_t fed = audio_feed_mp3_data((const uint8_t*)contents + written, realsize - written);
            written += fed;
            if (written < realsize && !s_net.stop_requested) {
                svcSleepThread(10 * 1000 * 1000LL); // 10 ms si el ring buffer está lleno
            }
        }
    }
    return s_net.stop_requested ? 0 : realsize;
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
    curl_global_cleanup();
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

static bool json_find_string(const char* json, const char* key, char* out_val, size_t max_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* pos = strstr(json, pattern);
    if (!pos) return false;

    pos += strlen(pattern);
    size_t i = 0;
    while (*pos && *pos != '\"' && i < max_len - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
        }
        out_val[i++] = *pos++;
    }
    out_val[i] = '\0';
    return true;
}

static bool json_find_int(const char* json, const char* key, uint32_t* out_val) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* pos = strstr(json, pattern);
    if (!pos) return false;

    pos += strlen(pattern);
    while (*pos == ' ') pos++;
    *out_val = (uint32_t)strtoul(pos, NULL, 10);
    return true;
}

static bool search_tracks(const char* query, SearchResultSet* out_results, volatile bool* cancel) {
    if (!s_net.initialized || !query || !out_results) return false;
    memset(out_results, 0, sizeof(SearchResultSet));

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    char* esc_q = curl_easy_escape(curl, query, 0);
    char url[512];
    snprintf(url, sizeof(url),
             "https://api-v2.soundcloud.com/search/tracks?q=%s&client_id=%s&limit=%d",
             esc_q ? esc_q : query, s_client_id, SEARCH_LIMIT);
    if (esc_q) curl_free(esc_q);

    size_t json_max = SEARCH_JSON_MAX;
    char* json_buf = (char*)malloc(json_max);
    if (!json_buf) {
        curl_easy_cleanup(curl);
        return false;
    }
    json_buf[0] = '\0';

    MemoryBuffer chunk;
    chunk.memory = json_buf;
    chunk.size = 0;
    chunk.max_size = json_max;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    set_cancellable(curl, cancel);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || chunk.size == 0) {
        if (res != CURLE_OK)
            snprintf(s_search.error, sizeof(s_search.error), "Red: %s", curl_easy_strerror(res));
        else
            snprintf(s_search.error, sizeof(s_search.error), "SoundCloud respondio HTTP %ld", http_code);
        free(json_buf);
        return false;
    }
    if (http_code != 200) {
        snprintf(s_search.error, sizeof(s_search.error), "SoundCloud respondio HTTP %ld", http_code);
        free(json_buf);
        return false;
    }

    const char* cursor = strstr(json_buf, "\"collection\":[");
    if (!cursor) cursor = json_buf;

    int count = 0;
    int seen = 0;

    // Solo se listan pistas con stream MP3 progresivo: el resto (HLS/AAC) no se puede reproducir
    while (count < MAX_SEARCH_RESULTS && *cursor) {
        const char* track_start = strstr(cursor, "{\"artwork_url\":");
        const char* alt_start = strstr(cursor, "{\"title\":");
        if (alt_start && (!track_start || alt_start < track_start)) {
            track_start = alt_start;
        }
        if (!track_start) break;

        const char* next1 = strstr(track_start + 20, "{\"artwork_url\":");
        const char* next2 = strstr(track_start + 20, "{\"title\":");
        const char* track_end = NULL;
        if (next1 && next2) {
            track_end = (next1 < next2) ? next1 : next2;
        } else if (next1) {
            track_end = next1;
        } else if (next2) {
            track_end = next2;
        }

        seen++;
        const char* prog_pos = strstr(track_start, "/progressive");
        if (prog_pos && (!track_end || prog_pos < track_end)) {
            const char* url_key = prog_pos;
            while (url_key > track_start && strncmp(url_key, "\"url\":\"", 7) != 0) {
                url_key--;
            }

            if (strncmp(url_key, "\"url\":\"", 7) == 0) {
                SearchResultTrack* t = &out_results->tracks[count];
                memset(t, 0, sizeof(SearchResultTrack));

                json_find_string(url_key, "url", t->stream_endpoint, sizeof(t->stream_endpoint));
                t->has_progressive = true;
                // Las pistas con política SNIP solo publican un fragmento de 30 s
                t->is_preview = strstr(t->stream_endpoint, "/preview/") != NULL;

                json_find_string(track_start, "title", t->title, sizeof(t->title));

                const char* user_start = strstr(track_start, "\"user\":{");
                if (user_start && (!track_end || user_start < track_end)) {
                    json_find_string(user_start, "username", t->artist, sizeof(t->artist));
                }

                json_find_int(track_start, "duration", &t->duration_ms);

                json_find_string(track_start, "artwork_url", t->artwork_url, sizeof(t->artwork_url));
                if (strcmp(t->artwork_url, "null") == 0 || t->artwork_url[0] == '\0') {
                    if (user_start && (!track_end || user_start < track_end)) {
                        json_find_string(user_start, "avatar_url", t->artwork_url, sizeof(t->artwork_url));
                    }
                }

                count++;
            }
        }

        if (!track_end) break;
        cursor = track_end;
    }

    if (count == 0) {
        if (seen > 0)
            snprintf(s_search.error, sizeof(s_search.error), "%d resultados, ninguno en MP3 (solo HLS/AAC)", seen);
        else
            snprintf(s_search.error, sizeof(s_search.error), "Sin resultados");
    }

    out_results->count = count;
    free(json_buf);
    return (count > 0);
}

// Resuelve el endpoint de api-v2 a la URL firmada del CDN. Devuelve false si no se pudo.
static bool resolve_stream_url(const char* endpoint, char* out, size_t out_sz) {
    if (!strstr(endpoint, "api-v2.soundcloud.com")) {
        snprintf(out, out_sz, "%s", endpoint);
        return strncmp(out, "http", 4) == 0;
    }

    char resolve_url[1100];
    snprintf(resolve_url, sizeof(resolve_url), "%s?client_id=%s", endpoint, s_client_id);

    CURL* curl_res = curl_easy_init();
    if (!curl_res) return false;

    char json_res[2048] = {0};
    MemoryBuffer mb = { json_res, 0, sizeof(json_res) };
    curl_easy_setopt(curl_res, CURLOPT_URL, resolve_url);
    curl_easy_setopt(curl_res, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl_res, CURLOPT_WRITEDATA, (void*)&mb);
    curl_easy_setopt(curl_res, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl_res, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_res, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_res, CURLOPT_TIMEOUT, 10L);
    set_cancellable(curl_res, &s_net.stop_requested);

    bool ok = false;
    if (curl_easy_perform(curl_res) == CURLE_OK && mb.size > 0) {
        ok = json_find_string(json_res, "url", out, out_sz) && strncmp(out, "http", 4) == 0;
    }
    curl_easy_cleanup(curl_res);
    return ok;
}

// Lee el tamaño total del MP3 de las cabeceras: Content-Range ".../total" cuando
// se pidió un rango, o Content-Length en una petición completa.
static size_t stream_header_callback(char* buf, size_t size, size_t nitems, void* userp) {
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

static CURLcode stream_from(const char* url) {
    CURL* curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    u64 total = 0;
    char range[32];
    s_net.header_total = &total;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, stream_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)&total);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    if (s_net.start_offset > 0) {
        snprintf(range, sizeof(range), "%llu-", (unsigned long long)s_net.start_offset);
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }
    set_cancellable(curl, &s_net.stop_requested);
    // Cortar si la conexión queda colgada: menos de 1 KB/s durante 20 s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);

    CURLcode res = curl_easy_perform(curl);
    s_net.header_total = NULL;
    curl_easy_cleanup(curl);
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
    if (s_net.stop_requested) {
        s_net.is_streaming = false;
        return;
    }

    CURLcode res = stream_from(s_net.resolved_url);

    // La URL firmada del CDN caduca: si una reanudación falla, resolver de nuevo una vez
    if (res == CURLE_HTTP_RETURNED_ERROR && cached && !s_net.stop_requested &&
        resolve_stream_url(s_net.current_stream_url, s_net.resolved_url, sizeof(s_net.resolved_url))) {
        res = stream_from(s_net.resolved_url);
    }

    if (res != CURLE_OK && !s_net.stop_requested) {
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

bool network_start_stream(const char* stream_endpoint) {
    if (!stream_endpoint || stream_endpoint[0] == '\0') return false;

    network_stop_stream();
    audio_clear_buffers();

    snprintf(s_net.current_stream_url, sizeof(s_net.current_stream_url), "%s", stream_endpoint);
    s_net.resolved_url[0] = '\0';
    s_net.known_total = 0;
    s_net.start_offset = 0;
    return stream_thread_start();
}

bool network_seek_stream(float ratio) {
    u64 total = s_net.known_total;
    if (s_net.current_stream_url[0] == '\0' || total == 0) return false;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 0.99f) ratio = 0.99f;

    network_stop_stream();
    audio_clear_buffers();
    s_net.start_offset = (u64)((double)total * ratio);
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

static bool download_artwork(const char* url, uint8_t** out_rgba, int* out_w, int* out_h, volatile bool* cancel) {
    if (!url || url[0] == '\0' || strcmp(url, "null") == 0) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    size_t max_sz = 128 * 1024;
    char* raw_jpg = (char*)malloc(max_sz);
    if (!raw_jpg) {
        curl_easy_cleanup(curl);
        return false;
    }

    MemoryBuffer mb = { raw_jpg, 0, max_sz };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&mb);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    set_cancellable(curl, cancel);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || mb.size == 0) {
        free(raw_jpg);
        return false;
    }

    int w = 0, h = 0, comp = 0;
    uint8_t* decoded = stbi_load_from_memory((const stbi_uc*)mb.memory, (int)mb.size, &w, &h, &comp, 4);
    free(raw_jpg);

    if (!decoded) return false;

    *out_rgba = decoded;
    *out_w = w;
    *out_h = h;
    return true;
}

void network_free_artwork(uint8_t* rgba) {
    if (rgba) {
        stbi_image_free(rgba);
    }
}

// ---------------------------------------------------------------------------
// Trabajos asíncronos: el hilo principal sigue dibujando mientras esperan
// ---------------------------------------------------------------------------

static void search_worker_func(void* arg) {
    s_search.job.ok = search_tracks(s_search.query, &s_search.results, &s_search.job.cancel);
    s_search.job.running = false;
}

const char* network_get_search_error(void) {
    return s_search.error;
}

const char* network_get_stream_error(void) {
    return s_stream_error;
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
