#include "hls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Copia de [start, end) sin espacios ni saltos de línea finales
static char* dup_range(const char* start, const char* end) {
    while (end > start && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t n = (size_t)(end - start);
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, start, n);
    s[n] = '\0';
    return s;
}

static char* resolve_uri(const char* base, const char* start, const char* end) {
    char* rel = dup_range(start, end);
    if (!rel || strstr(rel, "://") || !base) return rel;

    // Relativa: se une al directorio (o a la raíz del host si empieza con '/')
    const char* cut;
    if (rel[0] == '/') {
        const char* scheme = strstr(base, "://");
        cut = scheme ? strchr(scheme + 3, '/') : NULL;
        if (!cut) cut = base + strlen(base);
    } else {
        const char* q = strchr(base, '?');
        const char* limit = q ? q : base + strlen(base);
        cut = limit;
        while (cut > base && cut[-1] != '/') cut--;
    }
    size_t blen = (size_t)(cut - base);
    char* full = (char*)malloc(blen + strlen(rel) + 1);
    if (full) {
        memcpy(full, base, blen);
        strcpy(full + blen, rel);
    }
    free(rel);
    return full;
}

// Valor del atributo URI="..." de una etiqueta
static char* attr_uri(const char* line, const char* line_end, const char* base) {
    const char* u = strstr(line, "URI=\"");
    if (!u || u >= line_end) return NULL;
    u += 5;
    const char* q = memchr(u, '"', (size_t)(line_end - u));
    return q ? resolve_uri(base, u, q) : NULL;
}

bool hls_parse(const char* text, const char* base_url, HlsPlaylist* out) {
    memset(out, 0, sizeof(*out));
    if (!text || strncmp(text, "#EXTM3U", 7) != 0) return false;

    int cap = 0;
    double pending_duration = -1.0;
    bool expect_variant = false;
    double total = 0.0;

    const char* line = text;
    while (*line) {
        const char* line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);

        if (strncmp(line, "#EXT-X-KEY:", 11) == 0) {
            if (!strstr(line, "METHOD=NONE") || strstr(line, "METHOD=NONE") > line_end) {
                hls_free(out);
                return false;
            }
        } else if (strncmp(line, "#EXT-X-MAP:", 11) == 0) {
            free(out->init_uri);
            out->init_uri = attr_uri(line, line_end, base_url);
        } else if (strncmp(line, "#EXTINF:", 8) == 0) {
            pending_duration = strtod(line + 8, NULL);
        } else if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            expect_variant = true;
        } else if (line[0] != '#' && line_end > line && line[0] != '\r') {
            if (expect_variant) {
                // Lista maestra: basta con la primera variante
                out->is_master = true;
                out->variant_uri = resolve_uri(base_url, line, line_end);
                return out->variant_uri != NULL;
            }
            if (pending_duration >= 0.0) {
                if (out->count == cap) {
                    int ncap = cap ? cap * 2 : 64;
                    HlsSegment* grown = (HlsSegment*)realloc(out->segments, (size_t)ncap * sizeof(HlsSegment));
                    if (!grown) {
                        hls_free(out);
                        return false;
                    }
                    out->segments = grown;
                    cap = ncap;
                }
                HlsSegment* seg = &out->segments[out->count];
                seg->uri = resolve_uri(base_url, line, line_end);
                if (!seg->uri) {
                    hls_free(out);
                    return false;
                }
                seg->start_ms = (uint32_t)(total * 1000.0 + 0.5);
                seg->duration_ms = (uint32_t)(pending_duration * 1000.0 + 0.5);
                total += pending_duration;
                out->count++;
                pending_duration = -1.0;
            }
        }

        line = *line_end ? line_end + 1 : line_end;
    }

    out->total_ms = (uint32_t)(total * 1000.0 + 0.5);
    if (out->count == 0) {
        hls_free(out);
        return false;
    }
    return true;
}

void hls_free(HlsPlaylist* pl) {
    if (!pl) return;
    for (int i = 0; i < pl->count; i++) free(pl->segments[i].uri);
    free(pl->segments);
    free(pl->init_uri);
    free(pl->variant_uri);
    memset(pl, 0, sizeof(*pl));
}

int hls_segment_at(const HlsPlaylist* pl, uint32_t ms) {
    if (!pl || pl->count == 0) return 0;
    for (int i = 0; i < pl->count; i++) {
        if (ms < pl->segments[i].start_ms + pl->segments[i].duration_ms) return i;
    }
    return pl->count - 1;
}
