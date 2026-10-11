// SPDX-License-Identifier: MIT
#include "wl/host/json.h"

#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 16
#define JSON_MAX_BYTES (4u << 20)

typedef struct {
    WlJson *doc;
    const char *p, *end;
} Parser;

static WlResult grow_nodes(WlJson *d) {
    if (d->count < d->cap) {
        return WL_OK;
    }
    size_t ncap = d->cap ? d->cap * 2 : 64;
    WlJsonNode *g = (WlJsonNode *)realloc(d->nodes, ncap * sizeof *g);
    if (g == NULL) {
        return WL_ERR_NOMEM;
    }
    d->nodes = g;
    d->cap = ncap;
    return WL_OK;
}

static WlResult new_node(WlJson *d, WlJsonType type, WlJsonRef *out) {
    WlResult r = grow_nodes(d);
    if (r != WL_OK) {
        return r;
    }
    WlJsonNode *n = &d->nodes[d->count];
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type;
    n->first = n->last = n->next = WL_JSON_NONE;
    *out = (WlJsonRef)d->count++;
    return WL_OK;
}

static WlResult pool_put(WlJson *d, const char *s, size_t n, uint32_t *off) {
    if (d->pool_len + n + 1 > JSON_MAX_BYTES) {
        return WL_ERR_RANGE;
    }
    if (d->pool_len + n + 1 > d->pool_cap) {
        size_t ncap = d->pool_cap ? d->pool_cap * 2 : 256;
        while (ncap < d->pool_len + n + 1) {
            ncap *= 2;
        }
        char *g = (char *)realloc(d->pool, ncap);
        if (g == NULL) {
            return WL_ERR_NOMEM;
        }
        d->pool = g;
        d->pool_cap = ncap;
    }
    *off = (uint32_t)d->pool_len;
    memcpy(d->pool + d->pool_len, s, n);
    d->pool[d->pool_len + n] = '\0';
    d->pool_len += n + 1;
    return WL_OK;
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A') + 10u;
        } else {
            return 0;
        }
        v = v * 16u + d;
    }
    *out = v;
    return 1;
}

/* Parses a string at ps->p (which points at the opening quote) into the pool. */
static WlResult parse_string(Parser *ps, uint32_t *off, uint32_t *len) {
    char *tmp;
    size_t n = 0;
    ps->p++;
    size_t room = (size_t)(ps->end - ps->p) + 1;
    tmp = (char *)malloc(room);
    if (tmp == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = WL_ERR_FORMAT;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') {
            r = pool_put(ps->doc, tmp, n, off);
            *len = (uint32_t)n;
            break;
        }
        if (c < 0x20) {
            break;
        }
        if (c != '\\') {
            tmp[n++] = (char)c;
            continue;
        }
        if (ps->p >= ps->end) {
            break;
        }
        char e = *ps->p++;
        switch (e) {
        case '"': tmp[n++] = '"'; break;
        case '\\': tmp[n++] = '\\'; break;
        case '/': tmp[n++] = '/'; break;
        case 'b': tmp[n++] = '\b'; break;
        case 'f': tmp[n++] = '\f'; break;
        case 'n': tmp[n++] = '\n'; break;
        case 'r': tmp[n++] = '\r'; break;
        case 't': tmp[n++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (ps->end - ps->p < 4 || !hex4(ps->p, &cp) || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                goto done; /* surrogates are not needed by our data files */
            }
            ps->p += 4;
            if (cp == 0) {
                goto done; /* embedded NUL would truncate the C string */
            }
            if (cp < 0x80u) {
                tmp[n++] = (char)cp;
            } else if (cp < 0x800u) {
                tmp[n++] = (char)(0xC0u | (cp >> 6));
                tmp[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                tmp[n++] = (char)(0xE0u | (cp >> 12));
                tmp[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                tmp[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
            break;
        }
        default: goto done;
        }
    }
done:
    free(tmp);
    return r;
}

static WlResult parse_value(Parser *ps, int depth, WlJsonRef *out);

static void append_child(WlJson *d, WlJsonRef parent, WlJsonRef child) {
    WlJsonNode *p = &d->nodes[parent];
    if (p->last == WL_JSON_NONE) {
        p->first = child;
    } else {
        d->nodes[p->last].next = child;
    }
    p->last = child;
    p->count++;
}

static int match_word(Parser *ps, const char *w) {
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, w, n) == 0) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static WlResult parse_container(Parser *ps, int depth, int is_obj, WlJsonRef *out) {
    WlJson *d = ps->doc;
    WlJsonRef self;
    WlResult r = new_node(d, is_obj ? WL_JSON_OBJECT : WL_JSON_ARRAY, &self);
    if (r != WL_OK) {
        return r;
    }
    char close = is_obj ? '}' : ']';
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        *out = self;
        return WL_OK;
    }
    for (;;) {
        uint32_t koff = 0, klen = 0;
        skip_ws(ps);
        if (is_obj) {
            if (ps->p >= ps->end || *ps->p != '"') {
                return WL_ERR_FORMAT;
            }
            r = parse_string(ps, &koff, &klen);
            if (r != WL_OK) {
                return r;
            }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                return WL_ERR_FORMAT;
            }
            ps->p++;
        }
        WlJsonRef child;
        r = parse_value(ps, depth + 1, &child);
        if (r != WL_OK) {
            return r;
        }
        d->nodes[child].key_off = koff;
        d->nodes[child].key_len = klen;
        append_child(d, self, child);
        skip_ws(ps);
        if (ps->p >= ps->end) {
            return WL_ERR_FORMAT;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == close) {
            ps->p++;
            *out = self;
            return WL_OK;
        }
        return WL_ERR_FORMAT;
    }
}

static WlResult parse_value(Parser *ps, int depth, WlJsonRef *out) {
    WlJson *d = ps->doc;
    if (depth > JSON_MAX_DEPTH) {
        return WL_ERR_RANGE;
    }
    skip_ws(ps);
    if (ps->p >= ps->end) {
        return WL_ERR_FORMAT;
    }
    char c = *ps->p;
    WlResult r;
    if (c == '{' || c == '[') {
        return parse_container(ps, depth, c == '{', out);
    }
    if (c == '"') {
        uint32_t off, len;
        r = parse_string(ps, &off, &len);
        if (r != WL_OK) {
            return r;
        }
        r = new_node(d, WL_JSON_STRING, out);
        if (r == WL_OK) {
            d->nodes[*out].str_off = off;
            d->nodes[*out].str_len = len;
        }
        return r;
    }
    if (c == 't' || c == 'f') {
        int val = (c == 't');
        if (!match_word(ps, val ? "true" : "false")) {
            return WL_ERR_FORMAT;
        }
        r = new_node(d, WL_JSON_BOOL, out);
        if (r == WL_OK) {
            d->nodes[*out].num = val;
        }
        return r;
    }
    if (match_word(ps, "null")) {
        return new_node(d, WL_JSON_NULL, out);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        int neg = 0;
        uint64_t v = 0;
        size_t digits = 0;
        if (c == '-') {
            neg = 1;
            ps->p++;
        }
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
            unsigned dg = (unsigned)(*ps->p - '0');
            if (v > (UINT64_MAX - dg) / 10u || v * 10u + dg > (uint64_t)INT64_MAX) {
                return WL_ERR_RANGE;
            }
            v = v * 10u + dg;
            digits++;
            ps->p++;
        }
        if (digits == 0 || (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E'))) {
            return WL_ERR_FORMAT; /* fractions and exponents are unsupported */
        }
        r = new_node(d, WL_JSON_INT, out);
        if (r == WL_OK) {
            d->nodes[*out].num = neg ? -(int64_t)v : (int64_t)v;
        }
        return r;
    }
    return WL_ERR_FORMAT;
}

WlResult wl_json_parse(WlJson *doc, const char *text, size_t len) {
    memset(doc, 0, sizeof *doc);
    if (len > JSON_MAX_BYTES) {
        return WL_ERR_RANGE;
    }
    Parser ps = {doc, text, text + len};
    WlJsonRef root;
    WlResult r = parse_value(&ps, 0, &root);
    if (r == WL_OK) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            r = WL_ERR_FORMAT; /* trailing garbage */
        }
    }
    if (r != WL_OK) {
        wl_json_free(doc);
    }
    return r;
}

void wl_json_free(WlJson *doc) {
    free(doc->nodes);
    free(doc->pool);
    memset(doc, 0, sizeof *doc);
}

WlJsonRef wl_json_root(const WlJson *doc) {
    return doc->count ? 0 : WL_JSON_NONE;
}

static const WlJsonNode *node(const WlJson *d, WlJsonRef r) {
    return (r != WL_JSON_NONE && r < d->count) ? &d->nodes[r] : NULL;
}

WlJsonType wl_json_type(const WlJson *doc, WlJsonRef ref) {
    const WlJsonNode *n = node(doc, ref);
    return n ? (WlJsonType)n->type : WL_JSON_NULL;
}

WlJsonRef wl_json_get(const WlJson *doc, WlJsonRef obj, const char *key) {
    const WlJsonNode *o = node(doc, obj);
    if (o == NULL || o->type != WL_JSON_OBJECT) {
        return WL_JSON_NONE;
    }
    size_t klen = strlen(key);
    for (WlJsonRef c = o->first; c != WL_JSON_NONE; c = doc->nodes[c].next) {
        const WlJsonNode *m = &doc->nodes[c];
        if (m->key_len == klen && memcmp(doc->pool + m->key_off, key, klen) == 0) {
            return c;
        }
    }
    return WL_JSON_NONE;
}

size_t wl_json_len(const WlJson *doc, WlJsonRef ref) {
    const WlJsonNode *n = node(doc, ref);
    return (n && (n->type == WL_JSON_ARRAY || n->type == WL_JSON_OBJECT)) ? n->count : 0;
}

WlJsonRef wl_json_at(const WlJson *doc, WlJsonRef ref, size_t index) {
    const WlJsonNode *n = node(doc, ref);
    if (n == NULL || (n->type != WL_JSON_ARRAY && n->type != WL_JSON_OBJECT) || index >= n->count) {
        return WL_JSON_NONE;
    }
    WlJsonRef c = n->first;
    while (index-- > 0) {
        c = doc->nodes[c].next;
    }
    return c;
}

const char *wl_json_str(const WlJson *doc, WlJsonRef ref) {
    const WlJsonNode *n = node(doc, ref);
    return (n && n->type == WL_JSON_STRING) ? doc->pool + n->str_off : NULL;
}

size_t wl_json_str_len(const WlJson *doc, WlJsonRef ref) {
    const WlJsonNode *n = node(doc, ref);
    return (n && n->type == WL_JSON_STRING) ? n->str_len : 0;
}

WlResult wl_json_int(const WlJson *doc, WlJsonRef ref, int64_t *out) {
    const WlJsonNode *n = node(doc, ref);
    if (n == NULL || n->type != WL_JSON_INT) {
        return WL_ERR_FORMAT;
    }
    *out = n->num;
    return WL_OK;
}

WlResult wl_json_bool(const WlJson *doc, WlJsonRef ref, int *out) {
    const WlJsonNode *n = node(doc, ref);
    if (n == NULL || n->type != WL_JSON_BOOL) {
        return WL_ERR_FORMAT;
    }
    *out = (int)n->num;
    return WL_OK;
}
