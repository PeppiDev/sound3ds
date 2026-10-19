#include "account.h"
#include "debuglog.h"
#include "i18n.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TOKEN_URL        "https://secure.soundcloud.com/oauth/token"
#define REFRESH_AGE_S    (20 * 3600)   // el token dura 24 h: se renueva con margen
#define RETRY_S          60            // reintento si la renovación falló por la red
#define ACCOUNT_STACK_SZ (32 * 1024)

static struct {
    LightLock lock;              // protege los campos de abajo
    LightLock refresh_lock;      // una sola renovación a la vez
    bool loaded;
    char username[64];
    long long user_id;
    char access[4096];
    char refresh[512];
    char client[128];
    long long obtained_at;       // reloj de la consola (time()), no el del PC
    u32 gen;
    u64 next_try_ms;             // no reintentar antes (osGetTime)
    char message[96];
    bool expired;
    Thread thread;
    volatile bool thread_done;
} s_acc;

typedef enum { REFRESH_OK, REFRESH_REJECTED, REFRESH_NET } RefreshResult;

// Limpia memoria con tokens para que no queden en el heap libre
static void wipe(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

static void clear_locked(void) {
    wipe(s_acc.access, sizeof(s_acc.access));
    wipe(s_acc.refresh, sizeof(s_acc.refresh));
    s_acc.loaded = false;
    s_acc.username[0] = s_acc.client[0] = 0;
    s_acc.user_id = 0;
    s_acc.obtained_at = 0;
    s_acc.gen++;
}

static const char* jstr(json_t* o, const char* key) {
    const char* v = json_string_value(json_object_get(o, key));
    return v ? v : "";
}

// Guarda la cuenta: primero a un temporal y luego se renombra
static bool save_locked(void) {
    json_t* root = json_pack("{s:s, s:I, s:s, s:s, s:s, s:I}",
                             "username", s_acc.username, "user_id", (json_int_t)s_acc.user_id,
                             "access_token", s_acc.access, "refresh_token", s_acc.refresh,
                             "auth_client_id", s_acc.client, "obtained_at", (json_int_t)s_acc.obtained_at);
    if (!root) return false;
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    bool ok = json_dump_file(root, ACCOUNT_PATH ".tmp", JSON_COMPACT) == 0;
    json_decref(root);
    if (ok) {
        remove(ACCOUNT_PATH);
        ok = rename(ACCOUNT_PATH ".tmp", ACCOUNT_PATH) == 0;
    }
    if (!ok) remove(ACCOUNT_PATH ".tmp");
    return ok;
}

static void set_message_locked(const char* msg) {
    snprintf(s_acc.message, sizeof(s_acc.message), "%s", msg);
}

// ---------------------------------------------------------------------------
// Renovación
// ---------------------------------------------------------------------------

typedef struct {
    char* data;
    size_t size;
} Body;

static size_t body_write(void* p, size_t sz, size_t n, void* user) {
    Body* b = (Body*)user;
    size_t len = sz * n;
    if (b->size + len > 16 * 1024) return 0;
    char* g = realloc(b->data, b->size + len + 1);
    if (!g) return 0;
    b->data = g;
    memcpy(b->data + b->size, p, len);
    b->size += len;
    b->data[b->size] = 0;
    return len;
}

// POST a secure.soundcloud.com con el token de renovación. No necesita secreto:
// es el mismo pedido que hace la web. El token de renovación cambia en cada uso.
static RefreshResult do_refresh(const char* refresh, const char* client,
                                char* access_out, size_t access_sz, char* refresh_out, size_t refresh_sz) {
    CURL* c = curl_easy_init();
    if (!c) return REFRESH_NET;
    char* r_esc = curl_easy_escape(c, refresh, 0);
    char* c_esc = curl_easy_escape(c, client, 0);
    char form[800];
    snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s",
             r_esc ? r_esc : "", c_esc ? c_esc : "");
    if (r_esc) { wipe(r_esc, strlen(r_esc)); curl_free(r_esc); }
    if (c_esc) curl_free(c_esc);

    Body b = {0};
    struct curl_slist* hdr = curl_slist_append(NULL, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_URL, TOKEN_URL);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    CURLcode res = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    wipe(form, sizeof(form));

    RefreshResult out = REFRESH_NET;
    if (res == CURLE_OK && http == 200 && b.data) {
        json_t* root = json_loads(b.data, 0, NULL);
        const char* a = jstr(root, "access_token");
        const char* r = jstr(root, "refresh_token");
        if (*a && strlen(a) < access_sz) {
            snprintf(access_out, access_sz, "%s", a);
            // Si no llega uno nuevo, el anterior sigue sirviendo
            snprintf(refresh_out, refresh_sz, "%s", (*r && strlen(r) < refresh_sz) ? r : refresh);
            out = REFRESH_OK;
        }
        json_decref(root);
    } else if (res == CURLE_OK && (http == 400 || http == 401)) {
        out = REFRESH_REJECTED;   // token de renovación vencido o revocado
    }
    dlog("cuenta: renovar -> curl %d, HTTP %ld, %s", (int)res, http,
         out == REFRESH_OK ? "ok" : (out == REFRESH_REJECTED ? "rechazada" : "red"));
    if (b.data) { wipe(b.data, b.size); free(b.data); }
    return out;
}

// Renueva si la generación sigue siendo gen (si no, otro hilo ya lo hizo).
// true si después hay un token más nuevo que gen.
static bool refresh_now(u32 gen) {
    LightLock_Lock(&s_acc.refresh_lock);
    LightLock_Lock(&s_acc.lock);
    if (!s_acc.loaded || s_acc.gen != gen) {
        bool newer = s_acc.loaded && s_acc.gen != gen;
        LightLock_Unlock(&s_acc.lock);
        LightLock_Unlock(&s_acc.refresh_lock);
        return newer;
    }
    static char refresh[512], client[128], new_access[4096], new_refresh[512];
    snprintf(refresh, sizeof(refresh), "%s", s_acc.refresh);
    snprintf(client, sizeof(client), "%s", s_acc.client);
    LightLock_Unlock(&s_acc.lock);

    RefreshResult r = REFRESH_REJECTED;
    if (refresh[0] && client[0])
        r = do_refresh(refresh, client, new_access, sizeof(new_access), new_refresh, sizeof(new_refresh));
    // Sin token de renovación (pegado a mano en Sound3DS Share) no hay cómo renovar

    bool ok = false;
    LightLock_Lock(&s_acc.lock);
    if (s_acc.loaded && s_acc.gen == gen) {   // nadie desconectó mientras tanto
        if (r == REFRESH_OK) {
            snprintf(s_acc.access, sizeof(s_acc.access), "%s", new_access);
            snprintf(s_acc.refresh, sizeof(s_acc.refresh), "%s", new_refresh);
            s_acc.obtained_at = (long long)time(NULL);
            s_acc.gen++;
            if (!save_locked()) dlog("cuenta: no se pudo guardar la sesion renovada");
            ok = true;
        } else if (r == REFRESH_REJECTED) {
            clear_locked();
            remove(ACCOUNT_PATH);
            set_message_locked("Vuelve a conectar SoundCloud desde Sound3DS Share");
            s_acc.expired = true;
        } else {
            s_acc.next_try_ms = osGetTime() + RETRY_S * 1000ULL;
        }
    }
    LightLock_Unlock(&s_acc.lock);
    wipe(refresh, sizeof(refresh));
    wipe(new_access, sizeof(new_access));
    wipe(new_refresh, sizeof(new_refresh));
    LightLock_Unlock(&s_acc.refresh_lock);
    return ok;
}

static void refresh_thread(void* arg) {
    refresh_now((u32)(uintptr_t)arg);
    s_acc.thread_done = true;
}

static void join_thread(void) {
    if (s_acc.thread) {
        threadJoin(s_acc.thread, U64_MAX);
        threadFree(s_acc.thread);
        s_acc.thread = NULL;
    }
}

static void start_background_refresh(u32 gen) {
    join_thread();
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_acc.thread_done = false;
    s_acc.thread = threadCreate(refresh_thread, (void*)(uintptr_t)gen, ACCOUNT_STACK_SZ,
                                prio + 1 > 0x3F ? 0x3F : prio + 1, -2, false);
    if (!s_acc.thread) s_acc.thread_done = true;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void account_init(void) {
    LightLock_Init(&s_acc.lock);
    LightLock_Init(&s_acc.refresh_lock);
    s_acc.thread_done = true;

    json_t* root = json_load_file(ACCOUNT_PATH, 0, NULL);
    if (!root) return;
    const char* name = jstr(root, "username");
    const char* access = jstr(root, "access_token");
    if (*name && *access && strlen(access) < sizeof(s_acc.access)) {
        snprintf(s_acc.username, sizeof(s_acc.username), "%s", name);
        snprintf(s_acc.access, sizeof(s_acc.access), "%s", access);
        snprintf(s_acc.refresh, sizeof(s_acc.refresh), "%s", jstr(root, "refresh_token"));
        snprintf(s_acc.client, sizeof(s_acc.client), "%s", jstr(root, "auth_client_id"));
        s_acc.user_id = (long long)json_integer_value(json_object_get(root, "user_id"));
        s_acc.obtained_at = (long long)json_number_value(json_object_get(root, "obtained_at"));
        s_acc.loaded = true;
        s_acc.gen = 1;
        dlog("cuenta: cargada (%s), edad %lld s", s_acc.username, (long long)time(NULL) - s_acc.obtained_at);
    } else {
        dlog("cuenta: account.json invalido");
    }
    // El objeto JSON tenía los tokens: limpiarlos antes de liberarlo
    const char* keys[2] = { "access_token", "refresh_token" };
    for (int k = 0; k < 2; k++) {
        json_t* v = json_object_get(root, keys[k]);
        if (json_is_string(v)) wipe((void*)json_string_value(v), json_string_length(v));
    }
    json_decref(root);
}

static const char* jstr_max(json_t* o, const char* key, size_t max) {
    const char* v = json_string_value(json_object_get(o, key));
    return v && *v && strlen(v) < max ? v : NULL;
}

bool account_import(const char* json_text, char* user_out, size_t user_sz, char* err, size_t err_sz) {
    json_t* in = json_loads(json_text, 0, NULL);
    const char* name = jstr_max(in, "username", sizeof(s_acc.username));
    const char* access = jstr_max(in, "access_token", sizeof(s_acc.access));
    const char* refresh = jstr_max(in, "refresh_token", sizeof(s_acc.refresh));
    const char* client = jstr_max(in, "auth_client_id", sizeof(s_acc.client));
    json_t* uid = json_object_get(in, "user_id");
    bool ok = name && access && json_is_integer(uid);
    if (!ok) {
        snprintf(err, err_sz, "%s", "Respuesta inválida de Sound3DS Share");
    } else {
        LightLock_Lock(&s_acc.refresh_lock);
        LightLock_Lock(&s_acc.lock);
        clear_locked();
        snprintf(s_acc.username, sizeof(s_acc.username), "%s", name);
        snprintf(s_acc.access, sizeof(s_acc.access), "%s", access);
        // Con el token pegado a mano puede no venir renovación: al vencer pedirá reconectar
        snprintf(s_acc.refresh, sizeof(s_acc.refresh), "%s", refresh && client ? refresh : "");
        snprintf(s_acc.client, sizeof(s_acc.client), "%s", refresh && client ? client : "");
        s_acc.user_id = (long long)json_integer_value(uid);
        // Hora de la consola, no la del PC: la edad se mide con este mismo reloj
        s_acc.obtained_at = (long long)time(NULL);
        s_acc.loaded = true;
        s_acc.expired = false;
        s_acc.next_try_ms = 0;
        ok = save_locked();
        if (!ok) {
            clear_locked();
            snprintf(err, err_sz, "%s", "No se pudo guardar en la tarjeta SD");
        } else {
            snprintf(user_out, user_sz, "%s", s_acc.username);
        }
        LightLock_Unlock(&s_acc.lock);
        LightLock_Unlock(&s_acc.refresh_lock);
        dlog("cuenta: importada desde el PC (%s)", ok ? name : "error");
    }
    // El objeto JSON tenía los tokens: limpiarlos antes de liberarlo
    const char* keys[2] = { "access_token", "refresh_token" };
    for (int k = 0; k < 2; k++) {
        json_t* v = json_object_get(in, keys[k]);
        if (json_is_string(v)) wipe((void*)json_string_value(v), json_string_length(v));
    }
    json_decref(in);
    return ok;
}

void account_exit(void) {
    join_thread();
    LightLock_Lock(&s_acc.lock);
    wipe(s_acc.access, sizeof(s_acc.access));
    wipe(s_acc.refresh, sizeof(s_acc.refresh));
    LightLock_Unlock(&s_acc.lock);
}

void account_poll(void) {
    if (!s_acc.thread_done) return;
    if (s_acc.thread) join_thread();
    LightLock_Lock(&s_acc.lock);
    bool due = false;
    if (s_acc.loaded && s_acc.refresh[0] && osGetTime() >= s_acc.next_try_ms) {
        long long age = (long long)time(NULL) - s_acc.obtained_at;
        // Edad negativa: el reloj de la consola se atrasó; mejor renovar
        due = age >= REFRESH_AGE_S || age < 0;
    }
    u32 gen = s_acc.gen;
    if (due) s_acc.next_try_ms = osGetTime() + RETRY_S * 1000ULL;   // no relanzar cada cuadro
    LightLock_Unlock(&s_acc.lock);
    if (due) start_background_refresh(gen);
}

bool account_connected(void) {
    LightLock_Lock(&s_acc.lock);
    bool c = s_acc.loaded;
    LightLock_Unlock(&s_acc.lock);
    return c;
}

void account_username(char* out, size_t out_sz) {
    LightLock_Lock(&s_acc.lock);
    snprintf(out, out_sz, "%s", s_acc.loaded ? s_acc.username : "");
    LightLock_Unlock(&s_acc.lock);
}

long long account_user_id(void) {
    LightLock_Lock(&s_acc.lock);
    long long id = s_acc.loaded ? s_acc.user_id : 0;
    LightLock_Unlock(&s_acc.lock);
    return id;
}

bool account_token(char* out, size_t out_sz, u32* gen) {
    LightLock_Lock(&s_acc.lock);
    bool ok = s_acc.loaded && strlen(s_acc.access) < out_sz;
    if (ok) {
        snprintf(out, out_sz, "%s", s_acc.access);
        if (gen) *gen = s_acc.gen;
    }
    LightLock_Unlock(&s_acc.lock);
    return ok;
}

bool account_on_unauthorized(u32 gen) {
    dlog("cuenta: 401, renovando");
    return refresh_now(gen);
}

void account_disconnect(void) {
    // Espera a una renovación en curso para que no vuelva a escribir el archivo
    LightLock_Lock(&s_acc.refresh_lock);
    LightLock_Lock(&s_acc.lock);
    clear_locked();
    remove(ACCOUNT_PATH);
    remove(ACCOUNT_PATH ".tmp");
    LightLock_Unlock(&s_acc.lock);
    LightLock_Unlock(&s_acc.refresh_lock);
    dlog("cuenta: desconectada");
}

bool account_expired(void) {
    LightLock_Lock(&s_acc.lock);
    bool e = s_acc.expired;
    LightLock_Unlock(&s_acc.lock);
    return e;
}

bool account_take_message(char* out, size_t out_sz) {
    LightLock_Lock(&s_acc.lock);
    bool has = s_acc.message[0] != 0;
    if (has) {
        snprintf(out, out_sz, "%s", s_acc.message);
        s_acc.message[0] = 0;
    }
    LightLock_Unlock(&s_acc.lock);
    return has;
}
