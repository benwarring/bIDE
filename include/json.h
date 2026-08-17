#ifndef JSON_H
#define JSON_H

#include <stddef.h>

/**
 * Just enough JSON for LSP, in two halves: a reader for what servers send and a
 * writer for what we send them.
 *
 * The reader is a recursive-descent parser over a complete in-memory message —
 * the framing layer has already established exactly how many bytes a message is
 * before this ever sees it, so there is no streaming case to handle.
 *
 * Reader lookups are total: every accessor takes a possibly-NULL value and a
 * default, and returns the default for a missing key or a wrong type. That is
 * deliberate. Server responses are full of optional fields and the same field
 * arriving in different shapes, so the alternative is a null check on every
 * single access, which is where the real bugs would live.
 */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/* ------------------------------------------------------------- reading --- */

/** Parses one complete value. Returns NULL if the text is not valid JSON. */
JsonValue *json_parse(const char *text, size_t len);

void json_free(JsonValue *v);

JsonType json_type(const JsonValue *v);

/** Element count of an array or object; 0 for anything else. */
int json_len(const JsonValue *v);

/** Array element `i`, or NULL. */
JsonValue *json_at(const JsonValue *v, int i);

/** Object member `key`, or NULL. */
JsonValue *json_get(const JsonValue *v, const char *key);

/** Key of object member `i`, for walking an object whose keys are data. */
const char *json_key_at(const JsonValue *v, int i);

/* Coercions on a value: the default is returned for NULL or a type mismatch. */
const char *json_as_str(const JsonValue *v, const char *def);
double      json_as_num(const JsonValue *v, double def);
int         json_as_int(const JsonValue *v, int def);
int         json_as_bool(const JsonValue *v, int def);

/* The same, looking a key up first. `json_get_str(o, "method", "")` is the
 * single most common operation in the whole subsystem. */
const char *json_get_str(const JsonValue *v, const char *key, const char *def);
double      json_get_num(const JsonValue *v, const char *key, double def);
int         json_get_int(const JsonValue *v, const char *key, int def);
int         json_get_bool(const JsonValue *v, const char *key, int def);

/** Whether the key is present at all, distinct from present-and-null. An LSP
 *  response is a response because it *has* an id, even if that id is null. */
int json_has(const JsonValue *v, const char *key);

/* ------------------------------------------------------------- writing --- */

/**
 * A growable buffer that emits JSON. Commas and colons are inserted
 * automatically from context, because hand-placed separators are the one part
 * of writing JSON by hand that reliably goes wrong.
 */
typedef struct {
    char *b;
    size_t len, cap;
} JsonW;

void jw_init(JsonW *w);
void jw_free(JsonW *w);

void jw_obj(JsonW *w);
void jw_obj_end(JsonW *w);
void jw_arr(JsonW *w);
void jw_arr_end(JsonW *w);

/** Writes a key; the next call writes its value. */
void jw_key(JsonW *w, const char *key);

void jw_str(JsonW *w, const char *s);
void jw_int(JsonW *w, long v);
void jw_bool(JsonW *w, int v);
void jw_null(JsonW *w);

/** Splices in already-serialized JSON, for echoing a value straight back. */
void jw_raw(JsonW *w, const char *json);

/** Key plus value in one call, for the many one-line fields. */
void jw_kstr(JsonW *w, const char *key, const char *s);
void jw_kint(JsonW *w, const char *key, long v);
void jw_kbool(JsonW *w, const char *key, int v);

#endif
