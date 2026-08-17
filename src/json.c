#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "wrappers.h"

/* Servers do not send deeply nested JSON, so a limit well above anything real
 * costs nothing and stops a malformed message from recursing us off the stack. */
#define MAX_DEPTH 64

struct JsonValue {
    JsonType type;
    union {
        int boolean;
        double number;
        char *string;                                   // owned, already unescaped
        struct {
            JsonValue **items;
            int n;
        } array;
        struct {
            char **keys;
            JsonValue **vals;
            int n;
        } object;
    } as;
};

/* ------------------------------------------------------------- reading --- */

typedef struct {
    const char *p;
    const char *end;
    int depth;
} Parser;

static JsonValue *parse_value(Parser *ps);

static JsonValue *value_new(JsonType t) {
    JsonValue *v = Malloc(sizeof(JsonValue));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static int peek(Parser *ps) {
    return ps->p < ps->end ? (unsigned char)*ps->p : -1;
}

/** Appends one code point as UTF-8. The buffer always has room: every escape
 *  consumes at least six input bytes and produces at most four. */
static void append_utf8(char **out, unsigned long cp) {
    char *o = *out;
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    *out = o;
}

static int hex4(const char *p, unsigned long *out) {
    unsigned long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/**
 * Parses a quoted string into a fresh allocation. The decoded form is never
 * longer than the escaped form, so one allocation of the source length is
 * always enough and no growth check is needed in the loop.
 */
static char *parse_string_raw(Parser *ps) {
    if (peek(ps) != '"') return NULL;
    ps->p++;

    const char *start = ps->p;
    char *out = Malloc((size_t)(ps->end - start) + 1);
    char *o = out;

    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p != '\\') {
            *o++ = *ps->p++;
            continue;
        }

        ps->p++;                                        // the backslash
        if (ps->p >= ps->end) break;

        char esc = *ps->p++;
        switch (esc) {
            case '"':  *o++ = '"';  break;
            case '\\': *o++ = '\\'; break;
            case '/':  *o++ = '/';  break;
            case 'b':  *o++ = '\b'; break;
            case 'f':  *o++ = '\f'; break;
            case 'n':  *o++ = '\n'; break;
            case 'r':  *o++ = '\r'; break;
            case 't':  *o++ = '\t'; break;
            case 'u': {
                unsigned long cp;
                if (ps->end - ps->p < 4 || !hex4(ps->p, &cp)) {
                    free(out);
                    return NULL;
                }
                ps->p += 4;

                // a high surrogate is only half a code point; the low half
                // follows as its own \u escape and the pair combines
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned long lo;
                    if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                append_utf8(&o, cp);
                break;
            }
            default:
                free(out);
                return NULL;
        }
    }

    if (ps->p >= ps->end) {                             // unterminated
        free(out);
        return NULL;
    }
    ps->p++;                                            // closing quote

    *o = '\0';
    return out;
}

static JsonValue *parse_array(Parser *ps) {
    ps->p++;                                            // '['
    JsonValue *v = value_new(JSON_ARRAY);

    skip_ws(ps);
    if (peek(ps) == ']') {
        ps->p++;
        return v;
    }

    int cap = 0;
    while (1) {
        JsonValue *item = parse_value(ps);
        if (item == NULL) {
            json_free(v);
            return NULL;
        }

        if (v->as.array.n == cap) {
            cap = cap ? cap * 2 : 8;
            v->as.array.items = Realloc(v->as.array.items, sizeof(JsonValue *) * (size_t)cap);
        }
        v->as.array.items[v->as.array.n++] = item;

        skip_ws(ps);
        int c = peek(ps);
        if (c == ',') {
            ps->p++;
            continue;
        }
        if (c == ']') {
            ps->p++;
            return v;
        }
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_object(Parser *ps) {
    ps->p++;                                            // '{'
    JsonValue *v = value_new(JSON_OBJECT);

    skip_ws(ps);
    if (peek(ps) == '}') {
        ps->p++;
        return v;
    }

    int cap = 0;
    while (1) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (key == NULL) {
            json_free(v);
            return NULL;
        }

        skip_ws(ps);
        if (peek(ps) != ':') {
            free(key);
            json_free(v);
            return NULL;
        }
        ps->p++;

        JsonValue *val = parse_value(ps);
        if (val == NULL) {
            free(key);
            json_free(v);
            return NULL;
        }

        if (v->as.object.n == cap) {
            cap = cap ? cap * 2 : 8;
            v->as.object.keys = Realloc(v->as.object.keys, sizeof(char *) * (size_t)cap);
            v->as.object.vals = Realloc(v->as.object.vals, sizeof(JsonValue *) * (size_t)cap);
        }
        v->as.object.keys[v->as.object.n] = key;
        v->as.object.vals[v->as.object.n] = val;
        v->as.object.n++;

        skip_ws(ps);
        int c = peek(ps);
        if (c == ',') {
            ps->p++;
            continue;
        }
        if (c == '}') {
            ps->p++;
            return v;
        }
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_value(Parser *ps) {
    if (++ps->depth > MAX_DEPTH) {
        ps->depth--;
        return NULL;
    }

    skip_ws(ps);
    JsonValue *v = NULL;
    int c = peek(ps);

    if (c == '{') {
        v = parse_object(ps);
    } else if (c == '[') {
        v = parse_array(ps);
    } else if (c == '"') {
        char *s = parse_string_raw(ps);
        if (s) {
            v = value_new(JSON_STRING);
            v->as.string = s;
        }
    } else if ((size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        v = value_new(JSON_BOOL);
        v->as.boolean = 1;
    } else if ((size_t)(ps->end - ps->p) >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        v = value_new(JSON_BOOL);
        v->as.boolean = 0;
    } else if ((size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        v = value_new(JSON_NULL);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        // strtod needs a terminator, and the message buffer is not guaranteed to
        // have one at this point; the number is copied out to a small stack buffer
        char num[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof(num) - 1 &&
               (strchr("+-.eE", *ps->p) != NULL || (*ps->p >= '0' && *ps->p <= '9'))) {
            num[n++] = *ps->p++;
        }
        num[n] = '\0';

        char *stop;
        double d = strtod(num, &stop);
        if (stop != num) {
            v = value_new(JSON_NUMBER);
            v->as.number = d;
        }
    }

    ps->depth--;
    return v;
}

JsonValue *json_parse(const char *text, size_t len) {
    Parser ps = {text, text + len, 0};

    JsonValue *v = parse_value(&ps);
    if (v == NULL) return NULL;

    skip_ws(&ps);
    if (ps.p != ps.end) {                               // trailing garbage is a malformed message
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(JsonValue *v) {
    if (v == NULL) return;

    switch (v->type) {
        case JSON_STRING:
            free(v->as.string);
            break;
        case JSON_ARRAY:
            for (int i = 0; i < v->as.array.n; i++) json_free(v->as.array.items[i]);
            free(v->as.array.items);
            break;
        case JSON_OBJECT:
            for (int i = 0; i < v->as.object.n; i++) {
                free(v->as.object.keys[i]);
                json_free(v->as.object.vals[i]);
            }
            free(v->as.object.keys);
            free(v->as.object.vals);
            break;
        default:
            break;
    }
    free(v);
}

/* ------------------------------------------------------------ accessors --- */

JsonType json_type(const JsonValue *v) {
    return v ? v->type : JSON_NULL;
}

int json_len(const JsonValue *v) {
    if (v == NULL) return 0;
    if (v->type == JSON_ARRAY) return v->as.array.n;
    if (v->type == JSON_OBJECT) return v->as.object.n;
    return 0;
}

JsonValue *json_at(const JsonValue *v, int i) {
    if (v == NULL || v->type != JSON_ARRAY) return NULL;
    if (i < 0 || i >= v->as.array.n) return NULL;
    return v->as.array.items[i];
}

JsonValue *json_get(const JsonValue *v, const char *key) {
    if (v == NULL || v->type != JSON_OBJECT || key == NULL) return NULL;

    for (int i = 0; i < v->as.object.n; i++) {
        if (strcmp(v->as.object.keys[i], key) == 0) return v->as.object.vals[i];
    }
    return NULL;
}

const char *json_key_at(const JsonValue *v, int i) {
    if (v == NULL || v->type != JSON_OBJECT) return NULL;
    if (i < 0 || i >= v->as.object.n) return NULL;
    return v->as.object.keys[i];
}

int json_has(const JsonValue *v, const char *key) {
    return json_get(v, key) != NULL;
}

const char *json_as_str(const JsonValue *v, const char *def) {
    return (v && v->type == JSON_STRING) ? v->as.string : def;
}

double json_as_num(const JsonValue *v, double def) {
    return (v && v->type == JSON_NUMBER) ? v->as.number : def;
}

int json_as_int(const JsonValue *v, int def) {
    return (v && v->type == JSON_NUMBER) ? (int)v->as.number : def;
}

int json_as_bool(const JsonValue *v, int def) {
    if (v == NULL) return def;
    if (v->type == JSON_BOOL) return v->as.boolean;
    if (v->type == JSON_NUMBER) return v->as.number != 0;
    return def;
}

const char *json_get_str(const JsonValue *v, const char *key, const char *def) {
    return json_as_str(json_get(v, key), def);
}

double json_get_num(const JsonValue *v, const char *key, double def) {
    return json_as_num(json_get(v, key), def);
}

int json_get_int(const JsonValue *v, const char *key, int def) {
    return json_as_int(json_get(v, key), def);
}

int json_get_bool(const JsonValue *v, const char *key, int def) {
    return json_as_bool(json_get(v, key), def);
}

/* ------------------------------------------------------------- writing --- */

void jw_init(JsonW *w) {
    w->b = NULL;
    w->len = 0;
    w->cap = 0;
}

void jw_free(JsonW *w) {
    free(w->b);
    jw_init(w);
}

static void jw_reserve(JsonW *w, size_t extra) {
    if (w->len + extra + 1 <= w->cap) return;
    w->cap = (w->len + extra + 1) * 2;
    w->b = Realloc(w->b, w->cap);
}

static void jw_put(JsonW *w, const char *s, size_t n) {
    jw_reserve(w, n);
    memcpy(&w->b[w->len], s, n);
    w->len += n;
    w->b[w->len] = '\0';
}

/**
 * Emits a separating comma when one is due. Rather than track nesting state,
 * this reads the last byte written: a value may follow '{', '[' or ':' with no
 * comma, and needs one after anything else. That covers every position a value
 * can appear in, with no bookkeeping to get out of step with the output.
 */
static void jw_sep(JsonW *w) {
    if (w->len == 0) return;

    char last = w->b[w->len - 1];
    if (last == '{' || last == '[' || last == ':') return;
    jw_put(w, ",", 1);
}

void jw_obj(JsonW *w)     { jw_sep(w); jw_put(w, "{", 1); }
void jw_obj_end(JsonW *w) { jw_put(w, "}", 1); }
void jw_arr(JsonW *w)     { jw_sep(w); jw_put(w, "[", 1); }
void jw_arr_end(JsonW *w) { jw_put(w, "]", 1); }

/** Escapes per RFC 8259: the two mandatory characters, the control range, and
 *  the shorthands for the control codes that have them. */
static void jw_escaped(JsonW *w, const char *s) {
    jw_put(w, "\"", 1);

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  jw_put(w, "\\\"", 2); break;
            case '\\': jw_put(w, "\\\\", 2); break;
            case '\b': jw_put(w, "\\b", 2);  break;
            case '\f': jw_put(w, "\\f", 2);  break;
            case '\n': jw_put(w, "\\n", 2);  break;
            case '\r': jw_put(w, "\\r", 2);  break;
            case '\t': jw_put(w, "\\t", 2);  break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    int n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    jw_put(w, esc, (size_t)n);
                } else {
                    // bytes >= 0x80 pass through: the source is already UTF-8 and
                    // re-encoding it would only be a chance to corrupt it
                    jw_put(w, (const char *)p, 1);
                }
                break;
        }
    }
    jw_put(w, "\"", 1);
}

void jw_key(JsonW *w, const char *key) {
    jw_sep(w);
    jw_escaped(w, key);
    jw_put(w, ":", 1);
}

void jw_str(JsonW *w, const char *s) {
    jw_sep(w);
    jw_escaped(w, s ? s : "");
}

void jw_int(JsonW *w, long v) {
    jw_sep(w);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld", v);
    jw_put(w, buf, (size_t)n);
}

void jw_bool(JsonW *w, int v) {
    jw_sep(w);
    jw_put(w, v ? "true" : "false", v ? 4u : 5u);
}

void jw_null(JsonW *w) {
    jw_sep(w);
    jw_put(w, "null", 4);
}

void jw_raw(JsonW *w, const char *json) {
    jw_sep(w);
    jw_put(w, json, strlen(json));
}

void jw_kstr(JsonW *w, const char *key, const char *s) { jw_key(w, key); jw_str(w, s); }
void jw_kint(JsonW *w, const char *key, long v)        { jw_key(w, key); jw_int(w, v); }
void jw_kbool(JsonW *w, const char *key, int v)        { jw_key(w, key); jw_bool(w, v); }
