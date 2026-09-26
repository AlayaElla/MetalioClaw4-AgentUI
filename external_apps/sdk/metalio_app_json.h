/* Small allocation-free JSON reader for external App AI action arguments.
 * It validates one complete object and extracts scalar members without adding
 * firmware symbols or a JSON-library dependency to ELF apps. JSON \u escapes
 * are decoded to standard UTF-8, including valid surrogate pairs. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* cursor;
    const char* end;
} metalio_app_json_reader_t;

#define METALIO_APP_JSON_MAX_DEPTH 16U

static void metalio_app_json_skip_ws(metalio_app_json_reader_t* r) {
    while (r->cursor < r->end && (*r->cursor == ' ' || *r->cursor == '\n' ||
            *r->cursor == '\r' || *r->cursor == '\t')) ++r->cursor;
}

static int metalio_app_json_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int metalio_app_json_read_u16(metalio_app_json_reader_t* r,
                                    uint32_t* value) {
    if (r->end - r->cursor < 4 || value == 0) return 0;
    uint32_t parsed = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = metalio_app_json_hex(*r->cursor++);
        if (digit < 0) return 0;
        parsed = (parsed << 4) | (uint32_t)digit;
    }
    *value = parsed;
    return 1;
}

static int metalio_app_json_append_utf8(uint32_t codepoint, char* out,
                                        size_t capacity, size_t* used) {
    if (used == 0 || codepoint == 0 || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return 0;
    const size_t bytes = codepoint <= 0x7fU ? 1 : codepoint <= 0x7ffU ? 2 :
                         codepoint <= 0xffffU ? 3 : 4;
    if (out != 0 && (*used + bytes >= capacity)) return 0;
    if (out != 0) {
        if (bytes == 1) out[(*used)++] = (char)codepoint;
        else if (bytes == 2) { out[(*used)++] = (char)(0xc0U | (codepoint >> 6)); out[(*used)++] = (char)(0x80U | (codepoint & 0x3fU)); }
        else if (bytes == 3) { out[(*used)++] = (char)(0xe0U | (codepoint >> 12)); out[(*used)++] = (char)(0x80U | ((codepoint >> 6) & 0x3fU)); out[(*used)++] = (char)(0x80U | (codepoint & 0x3fU)); }
        else { out[(*used)++] = (char)(0xf0U | (codepoint >> 18)); out[(*used)++] = (char)(0x80U | ((codepoint >> 12) & 0x3fU)); out[(*used)++] = (char)(0x80U | ((codepoint >> 6) & 0x3fU)); out[(*used)++] = (char)(0x80U | (codepoint & 0x3fU)); }
    } else {
        *used += bytes;
    }
    return 1;
}

static int metalio_app_json_string(metalio_app_json_reader_t* r, char* out,
                                   size_t capacity) {
    if (r->cursor >= r->end || *r->cursor++ != '\"') return 0;
    size_t used = 0;
    while (r->cursor < r->end) {
        unsigned char c = (unsigned char)*r->cursor++;
        if (c == '\"') {
            if (out != 0) {
                if (used >= capacity) return 0;
                out[used] = '\0';
            }
            return 1;
        }
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (r->cursor >= r->end) return 0;
            c = (unsigned char)*r->cursor++;
            if (c == 'u') {
                uint32_t codepoint = 0;
                if (!metalio_app_json_read_u16(r, &codepoint)) return 0;
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (r->end - r->cursor < 6 || r->cursor[0] != '\\' ||
                        r->cursor[1] != 'u') return 0;
                    r->cursor += 2;
                    uint32_t low = 0;
                    if (!metalio_app_json_read_u16(r, &low) || low < 0xdc00U ||
                        low > 0xdfffU) return 0;
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10) +
                                (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    return 0;
                }
                if (!metalio_app_json_append_utf8(codepoint, out, capacity, &used)) return 0;
                continue;
            } else if (c == '\"' || c == '\\' || c == '/') {
                /* already decoded */
            } else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else return 0;
        }
        if (c >= 0x80U) {
            // Raw UTF-8 is already encoded in the JSON input; preserve it.
            if (out != 0 && used + 1 >= capacity) return 0;
            if (out != 0) out[used] = (char)c;
            ++used;
        } else if (!metalio_app_json_append_utf8(c, out, capacity, &used)) {
            return 0;
        }
    }
    return 0;
}

static int metalio_app_json_skip_value_depth(metalio_app_json_reader_t* r,
                                             unsigned depth) {
    metalio_app_json_skip_ws(r);
    if (r->cursor >= r->end) return 0;
    if (*r->cursor == '\"') return metalio_app_json_string(r, 0, 0);
    if (*r->cursor == '{' || *r->cursor == '[') {
        if (depth >= METALIO_APP_JSON_MAX_DEPTH) return 0;
        const char open = *r->cursor++;
        const char close = open == '{' ? '}' : ']';
        metalio_app_json_skip_ws(r);
        if (r->cursor < r->end && *r->cursor == close) { ++r->cursor; return 1; }
        for (;;) {
            if (open == '{' && !metalio_app_json_string(r, 0, 0)) return 0;
            metalio_app_json_skip_ws(r);
            if (open == '{' && (r->cursor >= r->end || *r->cursor++ != ':')) return 0;
            if (!metalio_app_json_skip_value_depth(r, depth + 1U)) return 0;
            metalio_app_json_skip_ws(r);
            if (r->cursor < r->end && *r->cursor == close) { ++r->cursor; return 1; }
            if (r->cursor >= r->end || *r->cursor++ != ',') return 0;
            metalio_app_json_skip_ws(r);
        }
    }
    const char* start = r->cursor;
    if (*r->cursor == '-' || (*r->cursor >= '0' && *r->cursor <= '9')) {
        if (*r->cursor == '-') ++r->cursor;
        if (r->cursor >= r->end || *r->cursor < '0' || *r->cursor > '9') return 0;
        if (*r->cursor == '0') ++r->cursor;
        else while (r->cursor < r->end && *r->cursor >= '0' && *r->cursor <= '9') ++r->cursor;
        if (r->cursor < r->end && *r->cursor == '.') {
            ++r->cursor;
            if (r->cursor >= r->end || *r->cursor < '0' || *r->cursor > '9') return 0;
            while (r->cursor < r->end && *r->cursor >= '0' && *r->cursor <= '9') ++r->cursor;
        }
        if (r->cursor < r->end && (*r->cursor == 'e' || *r->cursor == 'E')) {
            ++r->cursor;
            if (r->cursor < r->end && (*r->cursor == '+' || *r->cursor == '-')) ++r->cursor;
            if (r->cursor >= r->end || *r->cursor < '0' || *r->cursor > '9') return 0;
            while (r->cursor < r->end && *r->cursor >= '0' && *r->cursor <= '9') ++r->cursor;
        }
        return r->cursor != start;
    }
    const char* literal = *r->cursor == 't' ? "true" : *r->cursor == 'f' ? "false" :
                          *r->cursor == 'n' ? "null" : 0;
    if (literal == 0) return 0;
    while (*literal != '\0') { if (r->cursor >= r->end || *r->cursor++ != *literal++) return 0; }
    return 1;
}

static int metalio_app_json_skip_value(metalio_app_json_reader_t* r) {
    return metalio_app_json_skip_value_depth(r, 0);
}

static int metalio_app_json_member(const char* json, const char* wanted,
                                   metalio_app_json_reader_t* value) {
    if (json == 0 || wanted == 0 || value == 0) return 0;
    metalio_app_json_reader_t r = {json, json};
    while (*r.end != '\0') ++r.end;
    metalio_app_json_skip_ws(&r);
    if (r.cursor >= r.end || *r.cursor++ != '{') return 0;
    int found = 0;
    metalio_app_json_skip_ws(&r);
    while (r.cursor < r.end && *r.cursor != '}') {
        char key[64];
        if (!metalio_app_json_string(&r, key, sizeof(key))) return 0;
        metalio_app_json_skip_ws(&r);
        if (r.cursor >= r.end || *r.cursor++ != ':') return 0;
        metalio_app_json_skip_ws(&r);
        metalio_app_json_reader_t member = r;
        if (!metalio_app_json_skip_value(&r)) return 0;
        const char* a = key; const char* b = wanted;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') {
            if (found) return 0;
            member.end = r.cursor;
            *value = member;
            found = 1;
        }
        metalio_app_json_skip_ws(&r);
        if (r.cursor < r.end && *r.cursor == ',') { ++r.cursor; metalio_app_json_skip_ws(&r); continue; }
        break;
    }
    if (r.cursor >= r.end || *r.cursor++ != '}') return 0;
    metalio_app_json_skip_ws(&r);
    return r.cursor == r.end && found;
}

static int metalio_app_json_get_string(const char* json, const char* key,
                                       char* out, size_t capacity) {
    metalio_app_json_reader_t value;
    if (!metalio_app_json_member(json, key, &value)) return 0;
    return metalio_app_json_string(&value, out, capacity) && value.cursor == value.end;
}

static int metalio_app_json_get_uint(const char* json, const char* key,
                                     uint32_t* out) {
    metalio_app_json_reader_t value;
    if (out == 0 || !metalio_app_json_member(json, key, &value)) return 0;
    uint64_t parsed = 0; const char* p = value.cursor;
    if (p >= value.end || *p < '0' || *p > '9') return 0;
    do { parsed = parsed * 10U + (uint32_t)(*p++ - '0'); if (parsed > UINT32_MAX) return 0; }
    while (p < value.end && *p >= '0' && *p <= '9');
    if (p != value.end) return 0;
    *out = (uint32_t)parsed; return 1;
}
