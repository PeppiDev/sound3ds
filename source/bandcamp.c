#include "bandcamp.h"
#include "net_http.h"
#include "debuglog.h"
#include "i18n.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SEARCH_URL   "https://bandcamp.com/api/bcsearch_public_api/1/autocomplete_elastic"
#define TRALBUM_URL  "https://bandcamp.com/api/mobile/24/tralbum_details"
#define DISCOVER_URL "https://bandcamp.com/api/discover/1/discover_web"
#define ART_URL      "https://f4.bcbits.com/img/a%lld_4.jpg"   // 300x300
#define JSON_MAX     (1024 * 1024)
#define COLLECTION_URL "https://bandcamp.com/api/fancollection/1/collection_items"
#define SUMMARY_URL    "https://bandcamp.com/api/fan/2/collection_summary"

// Cuenta conectada (la cookie identity no se escribe nunca en el registro)
static struct {
    bool ok;
    char identity[512];
    long long fan_id;
    char username[64];
} s_acc;
static LightLock s_lock;

static bool parse_account(json_t* root) {
    const char* identity = json_string_value(json_object_get(root, "identity"));
    const char* user = json_string_value(json_object_get(root, "username"));
    json_t* fid = json_object_get(root, "fan_id");
    if (!identity || !identity[0] || strlen(identity) >= sizeof(s_acc.identity) || !json_is_integer(fid)) return false;
    LightLock_Lock(&s_lock);
    snprintf(s_acc.identity, sizeof(s_acc.identity), "%s", identity);
    s_acc.fan_id = (long long)json_integer_value(fid);
    snprintf(s_acc.username, sizeof(s_acc.username), "%s", user ? user : "");
    s_acc.ok = true;
    LightLock_Unlock(&s_lock);
    return true;
}

void bandcamp_init(void) {
    LightLock_Init(&s_lock);
    json_t* root = json_load_file(BANDCAMP_PATH, 0, NULL);
    if (!root) return;
    if (parse_account(root)) dlog("bandcamp: cuenta conectada");
    json_decref(root);
}

bool bandcamp_connected(void) {
    return s_acc.ok;
}

void bandcamp_label(char* out, size_t out_sz) {
    LightLock_Lock(&s_lock);
    snprintf(out, out_sz, "%s", s_acc.ok ? s_acc.username : "");
    LightLock_Unlock(&s_lock);
}

void bandcamp_cookie(char* out, size_t out_sz) {
    LightLock_Lock(&s_lock);
    if (s_acc.ok) snprintf(out, out_sz, "identity=%s", s_acc.identity);
    else if (out_sz) out[0] = '\0';
    LightLock_Unlock(&s_lock);
}

static bool save_account(void) {
    json_t* root = json_object();
    LightLock_Lock(&s_lock);
    json_object_set_new(root, "identity", json_string(s_acc.identity));
    json_object_set_new(root, "fan_id", json_integer(s_acc.fan_id));
    json_object_set_new(root, "username", json_string(s_acc.username));
    LightLock_Unlock(&s_lock);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    bool ok = json_dump_file(root, BANDCAMP_PATH ".tmp", JSON_COMPACT) == 0;
    json_decref(root);
    if (ok) {
        remove(BANDCAMP_PATH);
        ok = rename(BANDCAMP_PATH ".tmp", BANDCAMP_PATH) == 0;
    }
    return ok;
}

void bandcamp_disconnect(void) {
    LightLock_Lock(&s_lock);
    memset(&s_acc, 0, sizeof(s_acc));
    LightLock_Unlock(&s_lock);
    remove(BANDCAMP_PATH);
    dlog("bandcamp: cuenta desconectada");
}

bool bandcamp_import(const char* json_text, char* label_out, size_t label_sz, char* err, size_t err_sz) {
    json_t* root = json_loads(json_text, 0, NULL);
    bool ok = root && parse_account(root);
    if (root) json_decref(root);
    if (!ok) {
        snprintf(err, err_sz, "%s", L("Datos de la cuenta incompletos"));
        return false;
    }
    // Probar la sesión antes de guardarla
    volatile bool no_cancel = false;
    long code = 0;
    json_t* sum = net_get_json(SUMMARY_URL, 64 * 1024, &no_cancel, &code, err, err_sz);
    bool valid = sum && json_is_integer(json_object_get(sum, "fan_id"));
    if (sum) json_decref(sum);
    if (!valid) {
        LightLock_Lock(&s_lock);
        memset(&s_acc, 0, sizeof(s_acc));
        LightLock_Unlock(&s_lock);
        json_t* old = json_load_file(BANDCAMP_PATH, 0, NULL);   // volver a la cuenta anterior, si había
        if (old) {
            parse_account(old);
            json_decref(old);
        }
        snprintf(err, err_sz, "%s", L("Bandcamp no aceptó la sesión"));
        return false;
    }
    if (!save_account()) {
        snprintf(err, err_sz, "%s", L("No se pudo guardar en la SD"));
        return false;
    }
    bandcamp_label(label_out, label_sz);
    dlog("bandcamp: cuenta conectada");
    return true;
}

static const char* jstr(json_t* o, const char* key) {
    json_t* v = json_object_get(o, key);
    return json_is_string(v) ? json_string_value(v) : NULL;
}

static long long jid(json_t* o, const char* key) {
    json_t* v = json_object_get(o, key);
    return json_is_integer(v) ? (long long)json_integer_value(v) : 0;
}

static void art_url(long long art_id, char* out, size_t out_sz) {
    if (art_id > 0) snprintf(out, out_sz, ART_URL, art_id);
    else out[0] = '\0';
}

static void track_endpoint(long long band_id, long long track_id, char* out, size_t out_sz) {
    snprintf(out, out_sz, "bc:t:%lld:%lld", band_id, track_id);
}

// Búsqueda de la web ("t" canciones, "a" álbumes): la consulta va escapada en JSON
static json_t* search(const char* query, const char* filter, volatile bool* cancel, char* err, size_t err_sz) {
    json_t* body = json_pack("{s:s, s:s, s:b, s:n}", "search_text", query, "search_filter", filter, "full_page", 0,
                             "fan_id");
    char* text = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (!text) return NULL;
    json_t* root = net_post_json(SEARCH_URL, text, JSON_MAX, cancel, NULL, err, err_sz);
    free(text);
    return root;
}

bool bandcamp_search_tracks(const char* query, SearchResultSet* out, volatile bool* cancel, char* err, size_t err_sz) {
    memset(out, 0, sizeof(*out));
    json_t* root = search(query, "t", cancel, err, err_sz);
    if (!root) return false;
    size_t i;
    json_t* r;
    json_array_foreach(json_object_get(json_object_get(root, "auto"), "results"), i, r) {
        if (out->count >= MAX_SEARCH_RESULTS) break;
        const char* type = jstr(r, "type");
        if (!type || strcmp(type, "t") != 0) continue;
        SearchResultTrack* t = &out->tracks[out->count];
        memset(t, 0, sizeof(*t));
        long long id = jid(r, "id"), band = jid(r, "band_id");
        if (!id || !band) continue;
        snprintf(t->id, sizeof(t->id), "%lld", id);
        const char* name = jstr(r, "name");
        const char* artist = jstr(r, "band_name");
        snprintf(t->title, sizeof(t->title), "%s", name ? name : "?");
        snprintf(t->artist, sizeof(t->artist), "%s", artist ? artist : "");
        track_endpoint(band, id, t->stream_endpoint, sizeof(t->stream_endpoint));
        art_url(jid(r, "art_id"), t->artwork_url, sizeof(t->artwork_url));
        t->kind = STREAM_MP3;   // la duración se sabe al reproducir
        out->count++;
    }
    json_decref(root);
    if (out->count == 0) snprintf(err, err_sz, "Sin resultados");
    return out->count > 0;
}

bool bandcamp_search_albums(const char* query, PlaylistResultSet* out, volatile bool* cancel, char* err, size_t err_sz) {
    memset(out, 0, sizeof(*out));
    json_t* root = search(query, "a", cancel, err, err_sz);
    if (!root) return false;
    size_t i;
    json_t* r;
    json_array_foreach(json_object_get(json_object_get(root, "auto"), "results"), i, r) {
        if (out->count >= MAX_SEARCH_RESULTS) break;
        const char* type = jstr(r, "type");
        long long id = jid(r, "id"), band = jid(r, "band_id");
        if (!type || strcmp(type, "a") != 0 || !id || !band) continue;
        PlaylistResult* a = &out->items[out->count++];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "bc:a:%lld:%lld", band, id);
        const char* name = jstr(r, "name");
        const char* artist = jstr(r, "band_name");
        snprintf(a->title, sizeof(a->title), "%s", name ? name : "?");
        snprintf(a->creator, sizeof(a->creator), "%s", artist ? artist : "");
        art_url(jid(r, "art_id"), a->artwork_url, sizeof(a->artwork_url));
    }
    json_decref(root);
    if (out->count == 0) snprintf(err, err_sz, "Sin resultados");
    return out->count > 0;
}

// Ficha de un álbum ('a') o de una canción ('t')
static json_t* tralbum(char type, long long band, long long id, volatile bool* cancel, char* err, size_t err_sz) {
    char url[192];
    snprintf(url, sizeof(url), TRALBUM_URL "?band_id=%lld&tralbum_type=%c&tralbum_id=%lld", band, type, id);
    long code = 0;
    json_t* root = net_get_json(url, JSON_MAX, cancel, &code, err, err_sz);
    if (!root && code == 404) snprintf(err, err_sz, "%s", L("Ya no está en Bandcamp"));
    return root;
}

static const char* mp3_url(json_t* track) {
    return jstr(json_object_get(track, "streaming_url"), "mp3-128");
}

bool bandcamp_load_album(const char* id, PlaylistDetail* d, volatile bool* cancel, char* err, size_t err_sz) {
    memset(d, 0, sizeof(*d));
    long long band = 0, album = 0;
    char kind = 0;
    if (sscanf(id, "bc:%c:%lld:%lld", &kind, &band, &album) != 3 || (kind != 'a' && kind != 's')) {
        snprintf(err, err_sz, "%s", L("No se pudo abrir la playlist"));
        return false;
    }
    json_t* root = tralbum(kind == 's' ? 't' : 'a', band, album, cancel, err, err_sz);
    if (!root) return false;
    snprintf(d->id, sizeof(d->id), "%s", id);
    const char* title = jstr(root, "title");
    const char* artist = jstr(root, "tralbum_artist");
    if (!artist) artist = jstr(json_object_get(root, "band"), "name");
    snprintf(d->title, sizeof(d->title), "%s", title ? title : "?");
    snprintf(d->creator, sizeof(d->creator), "%s", artist ? artist : "");
    char cover[96];
    art_url(jid(root, "art_id"), cover, sizeof(cover));
    snprintf(d->artwork_url, sizeof(d->artwork_url), "%s", cover);
    json_t* tracks = json_object_get(root, "tracks");
    d->total = (int)json_array_size(tracks);
    size_t i;
    json_t* tr;
    json_array_foreach(tracks, i, tr) {
        if (d->count >= MAX_PLAYLIST_TRACKS) break;
        long long tid = jid(tr, "track_id");
        if (!tid || !mp3_url(tr)) {   // sin escucha gratis (solo comprándolo)
            d->unavailable++;
            continue;
        }
        SearchResultTrack* t = &d->tracks[d->count];
        memset(t, 0, sizeof(*t));
        snprintf(t->id, sizeof(t->id), "%lld", tid);
        const char* tt = jstr(tr, "title");
        const char* ta = jstr(tr, "band_name");
        snprintf(t->title, sizeof(t->title), "%s", tt ? tt : "?");
        snprintf(t->artist, sizeof(t->artist), "%s", ta ? ta : d->creator);
        t->duration_ms = (uint32_t)(json_number_value(json_object_get(tr, "duration")) * 1000.0);
        track_endpoint(jid(tr, "band_id") ? jid(tr, "band_id") : band, tid, t->stream_endpoint, sizeof(t->stream_endpoint));
        snprintf(t->artwork_url, sizeof(t->artwork_url), "%s", cover);
        t->kind = STREAM_MP3;
        long long num = jid(tr, "track_num");
        d->position[d->count] = num > 0 ? (int)num : (int)i + 1;
        d->total_ms += t->duration_ms;
        d->count++;
    }
    json_decref(root);
    if (d->count == 0) snprintf(err, err_sz, "%s", L("No tiene canciones reproducibles"));
    return d->count > 0;
}

// Colección de la cuenta: álbumes y canciones compradas (o descargadas a precio libre)
static bool collection(const char* next, AccPage* out, volatile bool* cancel, char* err, size_t err_sz) {
    LightLock_Lock(&s_lock);
    long long fan_id = s_acc.fan_id;
    LightLock_Unlock(&s_lock);
    json_t* body = json_pack("{s:I, s:s, s:i}", "fan_id", (json_int_t)fan_id, "older_than_token",
                             next && next[0] ? next : "9999999999::a::", "count", ACC_PAGE_SIZE);
    char* text = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (!text) return false;
    json_t* root = net_post_json(COLLECTION_URL, text, 2 * JSON_MAX, cancel, NULL, err, err_sz);
    free(text);
    if (!root) return false;
    size_t i;
    json_t* r;
    json_array_foreach(json_object_get(root, "items"), i, r) {
        out->seen++;
        if (out->count >= ACC_PAGE_SIZE) break;
        const char* type = jstr(r, "tralbum_type");
        long long id = jid(r, "tralbum_id"), band = jid(r, "band_id");
        if (!type || !id || !band || (type[0] != 'a' && type[0] != 't')) {
            out->skipped++;
            continue;
        }
        PlaylistResult* a = &out->playlists[out->count++];
        snprintf(a->id, sizeof(a->id), "bc:%c:%lld:%lld", type[0] == 'a' ? 'a' : 's', band, id);
        const char* title = jstr(r, "item_title");
        const char* artist = jstr(r, "band_name");
        snprintf(a->title, sizeof(a->title), "%s", title ? title : "?");
        snprintf(a->creator, sizeof(a->creator), "%s", artist ? artist : "");
        a->track_count = type[0] == 't' ? 1 : (int)jid(r, "num_streamable_tracks");
        art_url(jid(r, "item_art_id"), a->artwork_url, sizeof(a->artwork_url));
    }
    const char* last = jstr(root, "last_token");
    if (last && json_is_true(json_object_get(root, "more_available")))
        snprintf(out->next_href, sizeof(out->next_href), "%s", last);
    json_decref(root);
    return true;
}

bool bandcamp_discover(AccListKind kind, const char* next, AccPage* out, volatile bool* cancel, char* err,
                       size_t err_sz) {
    memset(out, 0, sizeof(*out));
    if (kind == ACC_LIST_PLAYLISTS && bandcamp_connected()) return collection(next, out, cancel, err, err_sz);
    const char* slice = kind == ACC_LIST_FEED ? "new" : "top";
    json_t* body = json_pack("{s:i, s:[], s:i, s:s, s:s, s:i, s:[s]}", "category_id", 0, "tag_norm_names",
                             "geoname_id", 0, "slice", slice, "cursor", next && next[0] ? next : "*", "size",
                             ACC_PAGE_SIZE, "include_result_types", "a");
    char* text = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (!text) return false;
    json_t* root = net_post_json(DISCOVER_URL, text, 2 * JSON_MAX, cancel, NULL, err, err_sz);
    free(text);
    if (!root) return false;
    size_t i;
    json_t* r;
    json_array_foreach(json_object_get(root, "results"), i, r) {
        out->seen++;
        if (out->count >= ACC_PAGE_SIZE) break;
        long long id = jid(r, "item_id"), band = jid(r, "band_id");
        json_t* img = json_object_get(r, "primary_image");
        long long art = json_is_true(json_object_get(img, "is_art")) ? jid(img, "image_id") : 0;
        const char* title = jstr(r, "title");
        const char* artist = jstr(r, "album_artist");
        if (!artist) artist = jstr(r, "band_name");
        if (kind == ACC_LIST_PLAYLISTS) {
            if (!id || !band) {
                out->skipped++;
                continue;
            }
            PlaylistResult* a = &out->playlists[out->count++];
            snprintf(a->id, sizeof(a->id), "bc:a:%lld:%lld", band, id);
            snprintf(a->title, sizeof(a->title), "%s", title ? title : "?");
            snprintf(a->creator, sizeof(a->creator), "%s", artist ? artist : "");
            a->track_count = (int)jid(r, "track_count");
            art_url(art, a->artwork_url, sizeof(a->artwork_url));
        } else {
            // Populares y Novedades: la canción destacada de cada álbum
            json_t* ft = json_object_get(r, "featured_track");
            long long tid = jid(ft, "id");
            if (!tid) {
                out->skipped++;
                continue;
            }
            SearchResultTrack* t = &out->tracks[out->count++];
            snprintf(t->id, sizeof(t->id), "%lld", tid);
            const char* tt = jstr(ft, "title");
            const char* ta = jstr(ft, "band_name");
            snprintf(t->title, sizeof(t->title), "%s", tt ? tt : "?");
            snprintf(t->artist, sizeof(t->artist), "%s", ta ? ta : (artist ? artist : ""));
            t->duration_ms = (uint32_t)(json_number_value(json_object_get(ft, "duration")) * 1000.0);
            track_endpoint(jid(ft, "band_id") ? jid(ft, "band_id") : band, tid, t->stream_endpoint,
                           sizeof(t->stream_endpoint));
            art_url(art, t->artwork_url, sizeof(t->artwork_url));
            t->kind = STREAM_MP3;
        }
    }
    const char* cursor = jstr(root, "cursor");
    if (cursor && out->seen > 0) snprintf(out->next_href, sizeof(out->next_href), "%s", cursor);
    json_decref(root);
    return true;
}

bool bandcamp_resolve(const char* endpoint, char* url_out, size_t url_sz, uint32_t* duration_ms, volatile bool* cancel,
                      char* err, size_t err_sz) {
    long long band = 0, track = 0;
    if (sscanf(endpoint, "bc:t:%lld:%lld", &band, &track) != 2) return false;
    json_t* root = tralbum('t', band, track, cancel, err, err_sz);
    if (!root) return false;
    bool ok = false;
    size_t i;
    json_t* tr;
    json_array_foreach(json_object_get(root, "tracks"), i, tr) {
        if (jid(tr, "track_id") != track) continue;
        const char* url = mp3_url(tr);
        if (url) {
            snprintf(url_out, url_sz, "%s", url);
            if (duration_ms) *duration_ms = (uint32_t)(json_number_value(json_object_get(tr, "duration")) * 1000.0);
            ok = true;
        }
        break;
    }
    json_decref(root);
    if (!ok) snprintf(err, err_sz, "%s", L("Esta canción solo se escucha comprándola"));
    return ok;
}
