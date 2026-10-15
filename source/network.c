#include "network.h"
#include "audio.h"
#include <string.h>
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
    u8* thread_stack;
    char current_stream_url[1024];
} s_net;

#define NET_THREAD_STACK_SZ (32 * 1024)

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

static size_t stream_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    if (s_net.stop_requested) return 0;

    while (!s_net.stop_requested && audio_get_buffer_fill_percent() > 80) {
        svcSleepThread(15 * 1000 * 1000LL); // 15 ms
    }

    if (realsize > 0) {
        audio_feed_mp3_data((const uint8_t*)contents, realsize);
    }
    return realsize;
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

bool network_search_tracks(const char* query, SearchResultSet* out_results) {
    if (!s_net.initialized || !query || !out_results) return false;
    memset(out_results, 0, sizeof(SearchResultSet));

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    char* esc_q = curl_easy_escape(curl, query, 0);
    char url[512];
    snprintf(url, sizeof(url),
             "https://api-v2.soundcloud.com/search/tracks?q=%s&client_id=%s&limit=35",
             esc_q ? esc_q : query, s_client_id);
    if (esc_q) curl_free(esc_q);

    size_t json_max = 128 * 1024;
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || chunk.size == 0) {
        free(json_buf);
        return false;
    }

    const char* cursor = strstr(json_buf, "\"collection\":[");
    if (!cursor) cursor = json_buf;

    int count = 0;

    // Pase 1: Extraer pistas que contengan stream progressive MP3 nativo
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

    // Pase 2: Si no hubo pistas progressive en la búsqueda, listar las primeras sin streaming roto
    if (count == 0) {
        cursor = strstr(json_buf, "\"collection\":[");
        if (!cursor) cursor = json_buf;

        while (count < MAX_SEARCH_RESULTS && *cursor) {
            const char* track_start = strstr(cursor, "{\"artwork_url\":");
            const char* alt_start = strstr(cursor, "{\"title\":");
            if (alt_start && (!track_start || alt_start < track_start)) {
                track_start = alt_start;
            }
            if (!track_start) break;

            const char* next1 = strstr(track_start + 20, "{\"artwork_url\":");
            const char* next2 = strstr(track_start + 20, "{\"title\":");
            const char* track_end = next1 ? (next2 && next2 < next1 ? next2 : next1) : next2;

            SearchResultTrack* t = &out_results->tracks[count];
            memset(t, 0, sizeof(SearchResultTrack));
            t->has_progressive = false;
            t->stream_endpoint[0] = '\0';

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
            if (!track_end) break;
            cursor = track_end;
        }
    }

    out_results->count = count;
    free(json_buf);
    return (count > 0);
}

static void stream_worker_func(void* arg) {
    char stream_url[1024];
    strncpy(stream_url, s_net.current_stream_url, sizeof(stream_url) - 1);
    stream_url[sizeof(stream_url) - 1] = '\0';

    // 1. Si la URL es de api-v2.soundcloud.com, resolver el enlace final de CloudFront (.mp3)
    if (strstr(stream_url, "api-v2.soundcloud.com")) {
        char resolve_url[1100];
        snprintf(resolve_url, sizeof(resolve_url), "%s?client_id=%s", stream_url, s_client_id);

        CURL* curl_res = curl_easy_init();
        if (curl_res) {
            char json_res[2048] = {0};
            MemoryBuffer mb = { json_res, 0, sizeof(json_res) };

            curl_easy_setopt(curl_res, CURLOPT_URL, resolve_url);
            curl_easy_setopt(curl_res, CURLOPT_WRITEFUNCTION, write_memory_callback);
            curl_easy_setopt(curl_res, CURLOPT_WRITEDATA, (void*)&mb);
            curl_easy_setopt(curl_res, CURLOPT_USERAGENT, "Mozilla/5.0");
            curl_easy_setopt(curl_res, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl_res, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl_res, CURLOPT_TIMEOUT, 6L);

            if (curl_easy_perform(curl_res) == CURLE_OK && mb.size > 0) {
                char final_url[1024] = {0};
                if (json_find_string(json_res, "url", final_url, sizeof(final_url))) {
                    strncpy(stream_url, final_url, sizeof(stream_url) - 1);
                    stream_url[sizeof(stream_url) - 1] = '\0';
                }
            }
            curl_easy_cleanup(curl_res);
        }
    }

    // Si no se pudo resolver a una URL directa de audio CDN, abortar para no bloquear
    if (strstr(stream_url, "api-v2.soundcloud.com") || strncmp(stream_url, "http", 4) != 0) {
        s_net.is_streaming = false;
        return;
    }

    // 2. Conectar al stream directo de CloudFront / CDN (MP3 puro)
    CURL* curl = curl_easy_init();
    if (!curl) {
        s_net.is_streaming = false;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, stream_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);

    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    s_net.is_streaming = false;
}

bool network_start_stream(const char* stream_endpoint) {
    if (!stream_endpoint || stream_endpoint[0] == '\0') return false;

    network_stop_stream();

    audio_clear_buffers();
    strncpy(s_net.current_stream_url, stream_endpoint, sizeof(s_net.current_stream_url) - 1);
    s_net.current_stream_url[sizeof(s_net.current_stream_url) - 1] = '\0';

    s_net.stop_requested = false;
    s_net.is_streaming = true;

    s_net.thread_stack = (u8*)memalign(32, NET_THREAD_STACK_SZ);
    if (!s_net.thread_stack) return false;

    s_net.stream_thread = threadCreate(
        stream_worker_func,
        NULL,
        NET_THREAD_STACK_SZ,
        0x32,
        -1,
        false
    );

    if (!s_net.stream_thread) {
        s_net.is_streaming = false;
        free(s_net.thread_stack);
        s_net.thread_stack = NULL;
        return false;
    }

    return true;
}

void network_stop_stream(void) {
    if (!s_net.is_streaming) return;

    s_net.stop_requested = true;

    if (s_net.stream_thread) {
        threadJoin(s_net.stream_thread, U64_MAX);
        threadFree(s_net.stream_thread);
        s_net.stream_thread = NULL;
    }

    if (s_net.thread_stack) {
        free(s_net.thread_stack);
        s_net.thread_stack = NULL;
    }

    s_net.is_streaming = false;
}

bool network_is_streaming(void) {
    return s_net.is_streaming;
}

bool network_download_artwork(const char* url, uint8_t** out_rgba, int* out_w, int* out_h) {
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);

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
