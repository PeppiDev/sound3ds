#include "subsonic.h"
#include "net_http.h"
#include "debuglog.h"
#include "i18n.h"

#include <3ds.h>
#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define API_VERSION   "1.16.1"
#define CLIENT_NAME   "Sound3DS"
#define JSON_MAX      (2 * 1024 * 1024)
#define PAGE          50
#define STREAM_KBPS   192   // MP3 que pide la consola (el servidor convierte FLAC, AAC, etc.)
#define COVER_SIZE    300

static struct {
    bool ok;
    char url[256];      // sin la barra final
    char user[64];
    char token[40];     // md5(contraseña + sal) en hexadecimal
    char salt[40];
    char name[64];      // nombre del servidor (Navidrome, ...)
} s_cfg;
static LightLock s_lock;

// ---------------------------------------------------------------------------
// Configuración
// ---------------------------------------------------------------------------

static bool parse_config(json_t* root, char* err, size_t err_sz) {
    const char* url = json_string_value(json_object_get(root, "url"));
    const char* user = json_string_value(json_object_get(root, "u"));
    const char* token = json_string_value(json_object_get(root, "t"));
    const char* salt = json_string_value(json_object_get(root, "s"));
    const char* name = json_string_value(json_object_get(root, "name"));
    if (!url || !user || !token || !salt || strncmp(url, "http", 4) != 0 || strlen(url) >= sizeof(s_cfg.url) ||
        strlen(user) >= sizeof(s_cfg.user) || strlen(token) >= sizeof(s_cfg.token) || strlen(salt) >= sizeof(s_cfg.salt)) {
        if (err) snprintf(err, err_sz, "%s", L("Datos del servidor incompletos"));
        return false;
    }
    LightLock_Lock(&s_lock);
    snprintf(s_cfg.url, sizeof(s_cfg.url), "%s", url);
    size_t n = strlen(s_cfg.url);
    while (n > 0 && s_cfg.url[n - 1] == '/') s_cfg.url[--n] = '\0';
    snprintf(s_cfg.user, sizeof(s_cfg.user), "%s", user);
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
    snprintf(s_cfg.salt, sizeof(s_cfg.salt), "%s", salt);
    snprintf(s_cfg.name, sizeof(s_cfg.name), "%s", name ? name : "");
    s_cfg.ok = true;
    LightLock_Unlock(&s_lock);
    return true;
}

void subsonic_init(void) {
    LightLock_Init(&s_lock);
    json_t* root = json_load_file(SUBSONIC_PATH, 0, NULL);
    if (!root) return;
    if (parse_config(root, NULL, 0)) dlog("servidor: configurado (%s)", s_cfg.name[0] ? s_cfg.name : "Subsonic");
    json_decref(root);
}

bool subsonic_configured(void) {
    return s_cfg.ok;
}

void subsonic_label(char* out, size_t out_sz) {
    LightLock_Lock(&s_lock);
    if (!s_cfg.ok) {
        out[0] = '\0';
    } else {
        // "usuario @ host" (sin el esquema)
        const char* host = strstr(s_cfg.url, "://");
        host = host ? host + 3 : s_cfg.url;
        snprintf(out, out_sz, "%s @ %s", s_cfg.user, host);
    }
    LightLock_Unlock(&s_lock);
}

static bool save_config(void) {
    json_t* root = json_object();
    LightLock_Lock(&s_lock);
    json_object_set_new(root, "url", json_string(s_cfg.url));
    json_object_set_new(root, "u", json_string(s_cfg.user));
    json_object_set_new(root, "t", json_string(s_cfg.token));
    json_object_set_new(root, "s", json_string(s_cfg.salt));
    json_object_set_new(root, "name", json_string(s_cfg.name));
    LightLock_Unlock(&s_lock);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    // Escribir aparte y reemplazar: el archivo nunca queda a medias
    bool ok = json_dump_file(root, SUBSONIC_PATH ".tmp", JSON_COMPACT) == 0;
    json_decref(root);
    if (ok) {
        remove(SUBSONIC_PATH);
        ok = rename(SUBSONIC_PATH ".tmp", SUBSONIC_PATH) == 0;
    }
    return ok;
}

void subsonic_disconnect(void) {
    LightLock_Lock(&s_lock);
    memset(&s_cfg, 0, sizeof(s_cfg));
    LightLock_Unlock(&s_lock);
    remove(SUBSONIC_PATH);
    dlog("servidor: desconectado");
}

// ---------------------------------------------------------------------------
// Peticiones
// ---------------------------------------------------------------------------

// "u=..&t=..&s=..&v=..&c=..&f=json"
static void auth_query(char* out, size_t out_sz) {
    LightLock_Lock(&s_lock);
    char* u = curl_easy_escape(NULL, s_cfg.user, 0);
    snprintf(out, out_sz, "u=%s&t=%s&s=%s&v=" API_VERSION "&c=" CLIENT_NAME "&f=json", u ? u : s_cfg.user,
             s_cfg.token, s_cfg.salt);
    if (u) curl_free(u);
    LightLock_Unlock(&s_lock);
}

static void endpoint_url(const char* method, const char* params, char* out, size_t out_sz) {
    char auth[256];
    auth_query(auth, sizeof(auth));
    LightLock_Lock(&s_lock);
    snprintf(out, out_sz, "%s/rest/%s?%s%s%s", s_cfg.url, method, auth, params && params[0] ? "&" : "",
             params ? params : "");
    LightLock_Unlock(&s_lock);
}

// GET a la API; devuelve el objeto "subsonic-response" (con una referencia: liberar
// con json_decref) o NULL con el error. *root_out recibe la raíz para liberarla.
static json_t* api_call(const char* method, const char* params, volatile bool* cancel, char* err, size_t err_sz,
                        json_t** root_out) {
    *root_out = NULL;
    if (!s_cfg.ok) {
        if (err) snprintf(err, err_sz, "%s", L("Conecta tu servidor desde Sound3DS Share"));
        return NULL;
    }
    char url[1024];
    endpoint_url(method, params, url, sizeof(url));
    long code = 0;
    json_t* root = net_get_json(url, JSON_MAX, cancel, &code, err, err_sz);
    memset(url, 0, sizeof(url));
    if (!root) {
        if (err && code == 0) snprintf(err, err_sz, "%s", L("No se pudo conectar con el servidor"));
        return NULL;
    }
    json_t* resp = json_object_get(root, "subsonic-response");
    const char* status = json_string_value(json_object_get(resp, "status"));
    if (!status || strcmp(status, "ok") != 0) {
        json_t* e = json_object_get(resp, "error");
        int ecode = (int)json_integer_value(json_object_get(e, "code"));
        const char* msg = json_string_value(json_object_get(e, "message"));
        if (err) {
            if (ecode == 40 || ecode == 41) snprintf(err, err_sz, "%s", L("El servidor rechazó el usuario o la contraseña"));
            else snprintf(err, err_sz, "%s", msg ? msg : L("El servidor respondió con un error"));
        }
        dlog("servidor: %s -> error %d", method, ecode);
        json_decref(root);
        return NULL;
    }
    *root_out = root;
    return resp;
}

static const char* jstr(json_t* o, const char* key) {
    json_t* v = json_object_get(o, key);
    return json_is_string(v) ? json_string_value(v) : NULL;
}

static int jint(json_t* o, const char* key) {
    json_t* v = json_object_get(o, key);
    return json_is_integer(v) ? (int)json_integer_value(v) : 0;
}

static void cover_url(const char* cover_id, char* out, size_t out_sz) {
    out[0] = '\0';
    if (!cover_id || !cover_id[0]) return;
    char* esc = curl_easy_escape(NULL, cover_id, 0);
    char params[160];
    snprintf(params, sizeof(params), "id=%s&size=%d", esc ? esc : cover_id, COVER_SIZE);
    if (esc) curl_free(esc);
    endpoint_url("getCoverArt", params, out, out_sz);
}

// Canción de la API -> pista reproducible (el servidor la convierte a MP3)
static bool parse_song(json_t* s, SearchResultTrack* t) {
    memset(t, 0, sizeof(*t));
    const char* id = jstr(s, "id");
    if (!id || json_is_true(json_object_get(s, "isVideo"))) return false;
    snprintf(t->id, sizeof(t->id), "%s", id);
    const char* title = jstr(s, "title");
    snprintf(t->title, sizeof(t->title), "%s", title ? title : "?");
    const char* artist = jstr(s, "artist");
    snprintf(t->artist, sizeof(t->artist), "%s", artist ? artist : "");
    t->duration_ms = (uint32_t)jint(s, "duration") * 1000u;
    char* esc = curl_easy_escape(NULL, id, 0);
    char params[160];
    snprintf(params, sizeof(params), "id=%s&format=mp3&maxBitRate=%d", esc ? esc : id, STREAM_KBPS);
    if (esc) curl_free(esc);
    endpoint_url("stream", params, t->stream_endpoint, sizeof(t->stream_endpoint));
    cover_url(jstr(s, "coverArt"), t->artwork_url, sizeof(t->artwork_url));
    t->kind = STREAM_MP3;
    t->starred = json_object_get(s, "starred") != NULL;
    return true;
}

// Álbum o playlist de la API -> elemento de lista que se abre como un álbum
static void parse_album(json_t* a, PlaylistResult* r, bool playlist) {
    memset(r, 0, sizeof(*r));
    const char* id = jstr(a, "id");
    snprintf(r->id, sizeof(r->id), "%s:%s", playlist ? "pl" : "al", id ? id : "");
    const char* name = jstr(a, "name");
    if (!name) name = jstr(a, "title");
    snprintf(r->title, sizeof(r->title), "%s", name ? name : "?");
    const char* who = playlist ? jstr(a, "owner") : jstr(a, "artist");
    snprintf(r->creator, sizeof(r->creator), "%s", who ? who : "");
    r->track_count = jint(a, "songCount");
    cover_url(jstr(a, "coverArt"), r->artwork_url, sizeof(r->artwork_url));
}

static void escape_param(const char* name, const char* value, char* out, size_t out_sz) {
    char* esc = curl_easy_escape(NULL, value, 0);
    snprintf(out, out_sz, "%s=%s", name, esc ? esc : value);
    if (esc) curl_free(esc);
}

// ---------------------------------------------------------------------------
// Conectar
// ---------------------------------------------------------------------------

bool subsonic_import(const char* json_text, char* label_out, size_t label_sz, char* err, size_t err_sz) {
    json_t* root = json_loads(json_text, 0, NULL);
    if (!root) {
        snprintf(err, err_sz, "%s", L("Datos del servidor incompletos"));
        return false;
    }
    bool ok = parse_config(root, err, err_sz);
    json_decref(root);
    if (!ok) return false;

    // Probar la conexión antes de guardarla
    json_t* r = NULL;
    volatile bool no_cancel = false;
    json_t* resp = api_call("ping", NULL, &no_cancel, err, err_sz, &r);
    if (!resp) {
        LightLock_Lock(&s_lock);
        s_cfg.ok = false;
        LightLock_Unlock(&s_lock);
        subsonic_init();   // volver a la conexión anterior, si había
        return false;
    }
    const char* type = jstr(resp, "type");
    if (type && !s_cfg.name[0]) {
        LightLock_Lock(&s_lock);
        snprintf(s_cfg.name, sizeof(s_cfg.name), "%s", type);
        LightLock_Unlock(&s_lock);
    }
    json_decref(r);
    if (!save_config()) {
        snprintf(err, err_sz, "%s", L("No se pudo guardar en la SD"));
        return false;
    }
    subsonic_label(label_out, label_sz);
    dlog("servidor: conectado (%s)", s_cfg.name);
    return true;
}

// ---------------------------------------------------------------------------
// Búsquedas y listas
// ---------------------------------------------------------------------------

bool subsonic_search_songs(const char* query, SearchResultSet* out, volatile bool* cancel, char* err, size_t err_sz) {
    memset(out, 0, sizeof(*out));
    char params[256], q[200];
    escape_param("query", query, q, sizeof(q));
    snprintf(params, sizeof(params), "%s&songCount=%d&albumCount=0&artistCount=0", q, MAX_SEARCH_RESULTS);
    json_t* root;
    json_t* resp = api_call("search3", params, cancel, err, err_sz, &root);
    if (!resp) return false;
    size_t i;
    json_t* s;
    json_array_foreach(json_object_get(json_object_get(resp, "searchResult3"), "song"), i, s) {
        if (out->count >= MAX_SEARCH_RESULTS) break;
        if (parse_song(s, &out->tracks[out->count])) out->count++;
    }
    json_decref(root);
    if (out->count == 0) snprintf(err, err_sz, "Sin resultados");
    return out->count > 0;
}

bool subsonic_search_albums(const char* query, PlaylistResultSet* out, volatile bool* cancel, char* err, size_t err_sz) {
    memset(out, 0, sizeof(*out));
    char params[256], q[200];
    escape_param("query", query, q, sizeof(q));
    snprintf(params, sizeof(params), "%s&songCount=0&albumCount=%d&artistCount=0", q, MAX_SEARCH_RESULTS);
    json_t* root;
    json_t* resp = api_call("search3", params, cancel, err, err_sz, &root);
    if (!resp) return false;
    size_t i;
    json_t* a;
    json_array_foreach(json_object_get(json_object_get(resp, "searchResult3"), "album"), i, a) {
        if (out->count >= MAX_SEARCH_RESULTS) break;
        parse_album(a, &out->items[out->count++], false);
    }
    json_decref(root);
    if (out->count == 0) snprintf(err, err_sz, "Sin resultados");
    return out->count > 0;
}

bool subsonic_load_list(const char* id, PlaylistDetail* d, volatile bool* cancel, char* err, size_t err_sz) {
    memset(d, 0, sizeof(*d));
    bool playlist = strncmp(id, "pl:", 3) == 0;
    char params[128];
    escape_param("id", id + 3, params, sizeof(params));
    json_t* root;
    json_t* resp = api_call(playlist ? "getPlaylist" : "getAlbum", params, cancel, err, err_sz, &root);
    if (!resp) return false;
    json_t* obj = json_object_get(resp, playlist ? "playlist" : "album");
    PlaylistResult head;
    parse_album(obj, &head, playlist);
    snprintf(d->id, sizeof(d->id), "%s", id);
    snprintf(d->title, sizeof(d->title), "%s", head.title);
    snprintf(d->creator, sizeof(d->creator), "%s", head.creator);
    snprintf(d->artwork_url, sizeof(d->artwork_url), "%s", head.artwork_url);
    size_t i;
    json_t* s;
    json_t* songs = json_object_get(obj, playlist ? "entry" : "song");
    d->total = (int)json_array_size(songs);
    json_array_foreach(songs, i, s) {
        if (d->count >= MAX_PLAYLIST_TRACKS) break;
        if (!parse_song(s, &d->tracks[d->count])) {
            d->unavailable++;
            continue;
        }
        d->position[d->count] = (int)i + 1;
        d->total_ms += d->tracks[d->count].duration_ms;
        d->count++;
    }
    d->unavailable += d->total - (int)(d->count + d->unavailable);
    json_decref(root);
    if (!d->artwork_url[0] && d->count > 0) snprintf(d->artwork_url, sizeof(d->artwork_url), "%s", d->tracks[0].artwork_url);
    if (d->count == 0) snprintf(err, err_sz, "%s", L("No tiene canciones reproducibles"));
    return d->count > 0;
}

static void add_songs(json_t* arr, AccPage* out) {
    size_t i;
    json_t* s;
    json_array_foreach(arr, i, s) {
        out->seen++;
        if (out->count >= ACC_PAGE_SIZE) break;
        if (parse_song(s, &out->tracks[out->count])) out->count++;
        else out->skipped++;
    }
}

bool subsonic_account_list(AccListKind kind, const char* next, AccPage* out, volatile bool* cancel, char* err,
                           size_t err_sz) {
    memset(out, 0, sizeof(*out));
    json_t* root = NULL;
    json_t* resp;
    if (kind == ACC_LIST_LIKES) {
        resp = api_call("getStarred2", NULL, cancel, err, err_sz, &root);
        if (!resp) return false;
        add_songs(json_object_get(json_object_get(resp, "starred2"), "song"), out);
    } else if (kind == ACC_LIST_FEED) {
        char params[32];
        snprintf(params, sizeof(params), "size=%d", ACC_PAGE_SIZE);
        resp = api_call("getRandomSongs", params, cancel, err, err_sz, &root);
        if (!resp) return false;
        add_songs(json_object_get(json_object_get(resp, "randomSongs"), "song"), out);
    } else {
        // Playlists del servidor (solo en la primera página) y después los álbumes de a 50
        int offset = next && next[0] ? atoi(next) : 0;
        if (offset == 0) {
            json_t* proot;
            json_t* presp = api_call("getPlaylists", NULL, cancel, err, err_sz, &proot);
            if (presp) {
                size_t i;
                json_t* p;
                json_array_foreach(json_object_get(json_object_get(presp, "playlists"), "playlist"), i, p) {
                    if (out->count >= ACC_PAGE_SIZE / 2) break;
                    parse_album(p, &out->playlists[out->count++], true);
                    out->seen++;
                }
                json_decref(proot);
            }
        }
        int want = ACC_PAGE_SIZE - out->count;
        char params[96];
        snprintf(params, sizeof(params), "type=alphabeticalByName&size=%d&offset=%d", want, offset);
        resp = api_call("getAlbumList2", params, cancel, err, err_sz, &root);
        if (!resp) return out->count > 0;
        int got = 0;
        size_t i;
        json_t* a;
        json_array_foreach(json_object_get(json_object_get(resp, "albumList2"), "album"), i, a) {
            if (out->count >= ACC_PAGE_SIZE) break;
            parse_album(a, &out->playlists[out->count++], false);
            out->seen++;
            got++;
        }
        if (got == want) snprintf(out->next_href, sizeof(out->next_href), "%d", offset + got);
    }
    json_decref(root);
    return true;
}

bool subsonic_star(const char* song_id, bool star, volatile bool* cancel, char* err, size_t err_sz) {
    char params[128];
    escape_param("id", song_id, params, sizeof(params));
    json_t* root;
    json_t* resp = api_call(star ? "star" : "unstar", params, cancel, err, err_sz, &root);
    if (!resp) return false;
    json_decref(root);
    return true;
}

bool subsonic_stream_at(const char* stream_url, unsigned seconds, char* out, size_t out_sz) {
    if (!strstr(stream_url, "/rest/stream?")) return false;
    snprintf(out, out_sz, "%s", stream_url);
    char* old = strstr(out, "&timeOffset=");
    if (old) *old = '\0';
    size_t n = strlen(out);
    if (seconds > 0) snprintf(out + n, out_sz - n, "&timeOffset=%u", seconds);
    return true;
}
