#ifndef DDDD_SERIAL_H
#define DDDD_SERIAL_H

/* Header-only nested-list builder for Pure Data C/C++ externals.
 *
 * Generates a Pd list containing individual numeric and symbol atoms:
 *    440 0.5 ( 880 0.25 )
 *
 * Your existing Lua dddd:table_from_atoms() wraps these tokens in the
 * outer parentheses, reconstructing {440, 0.5, {880, 0.25}}.
 *
 * Does not require lua_State or populate _G.dddd_outlets. A pdlua
 * receiver can convert this list to a proper dddd registry message.
 *
 * IMPORTANT: Numbers are Pd float atoms. On a 32-bit-float Pd build,
 * double precision is rounded to t_float. For exact double-to-Lua
 * transport, use dddd_serial_number_exact(), at the cost of interning
 * an individual numeric symbol for each number.
 */

#include "m_pd.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────
typedef struct _dddd_serial {
    t_atom *atoms;
    size_t count;
    size_t capacity;
    int depth;
    int finished;
    int failed;
} t_dddd_serial;

// ─────────────────────────────────────
static inline void dddd_serial_init(t_dddd_serial *w) {
    memset(w, 0, sizeof(*w));
}

// ─────────────────────────────────────
static inline void dddd_serial_free(t_dddd_serial *w) {
    free(w->atoms);
    memset(w, 0, sizeof(*w));
}

// ─────────────────────────────────────
static inline int dddd_serial_reserve(t_dddd_serial *w, size_t needed) {
    if (w->failed)
        return 0;
    if (needed <= w->capacity)
        return 1;
    if (needed > (size_t)INT_MAX || needed > SIZE_MAX / sizeof(t_atom)) {
        w->failed = 1;
        return 0;
    }

    size_t capacity = w->capacity ? w->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2 || capacity > (size_t)INT_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    t_atom *p = (t_atom *)realloc(w->atoms, capacity * sizeof(t_atom));
    if (!p) {
        w->failed = 1;
        return 0;
    }

    w->atoms = p;
    w->capacity = capacity;
    return 1;
}

// ─────────────────────────────────────
static inline int dddd_serial_push_symbol(t_dddd_serial *w, const char *s) {
    if (w->count == SIZE_MAX || !dddd_serial_reserve(w, w->count + 1)) {
        w->failed = 1;
        return 0;
    }

    SETSYMBOL(&w->atoms[w->count], gensym(s));
    w->count++;
    return 1;
}

// ─────────────────────────────────────
static inline int dddd_serial_begin(t_dddd_serial *w) {
    if (w->failed || w->finished || (w->depth == 0 && w->count != 0)) {
        w->failed = 1;
        return 0;
    }

    // Nested parentheses are separate Pd symbol atoms. The outer
    // parentheses are intentionally omitted: dddd adds them in Lua.
    if (w->depth > 0 && !dddd_serial_push_symbol(w, "("))
        return 0;

    w->depth++;
    return 1;
}

// ─────────────────────────────────────
static inline int dddd_serial_end(t_dddd_serial *w) {
    if (w->failed || w->depth <= 0) {
        w->failed = 1;
        return 0;
    }

    if (w->depth > 1 && !dddd_serial_push_symbol(w, ")"))
        return 0;

    w->depth--;
    if (w->depth == 0)
        w->finished = 1;
    return 1;
}

// ─────────────────────────────────────
static inline int dddd_serial_number(t_dddd_serial *w, double value) {
    if (w->failed || w->depth <= 0 || w->finished ||
        !isfinite(value)) {
        w->failed = 1;
        return 0;
    }

    t_float converted = (t_float)value;
    if (!isfinite((double)converted) ||
        w->count == SIZE_MAX || !dddd_serial_reserve(w, w->count + 1)) {
        w->failed = 1;
        return 0;
    }

    SETFLOAT(&w->atoms[w->count], converted);
    w->count++;
    return 1;
}

// ─────────────────────────────────────
/* Optional: encode a double as its own numeric symbol to preserve
 * precision on 32-bit-float Pd builds. Lua tonumber() converts it back.
 * Be mindful that gensym() interns each distinct text persistently. */
static inline int dddd_serial_number_exact(t_dddd_serial *w, double value) {
    if (w->failed || w->depth <= 0 || w->finished || !isfinite(value)) {
        w->failed = 1;
        return 0;
    }

    char num[64];
    int n = snprintf(num, sizeof(num), "%.17g", value);
    if (n < 0 || (size_t)n >= sizeof(num)) {
        w->failed = 1;
        return 0;
    }
    // Lua's tonumber() expects a dot regardless of locale.
    for (int i = 0; i < n; ++i)
        if (num[i] == ',')
            num[i] = '.';

    return dddd_serial_push_symbol(w, num);
}

// ─────────────────────────────────────
static inline int dddd_serial_symbol(t_dddd_serial *w, const char *s) {
    if (w->failed || w->depth <= 0 || w->finished || !s || !*s) {
        w->failed = 1;
        return 0;
    }

    // Existing dddd parser has no escaping/quoting.
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (isspace(*p) || *p == '(' || *p == ')' || *p == '[' || *p == ']') {
            w->failed = 1;
            return 0;
        }
    }

    return dddd_serial_push_symbol(w, s);
}

// ─────────────────────────────────────
static inline int dddd_serial_output(t_dddd_serial *w, t_outlet *out) {
    if (w->failed || !w->finished || w->depth != 0 || !out) {
        pd_error(NULL, "dddd: invalid or incomplete nested list");
        return 0;
    }
    if (w->count == 0) {
        pd_error(NULL, "dddd: empty root list needs explicit receiver support");
        return 0;
    }

    outlet_list(out, &s_list, (int)w->count, w->atoms);
    return 1;
}

#endif // DDDD_SERIAL_H
