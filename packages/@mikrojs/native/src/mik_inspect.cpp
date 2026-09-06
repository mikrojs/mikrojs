#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <quickjs.h>

#include "mikrojs/mik_color.h"
#include "mikrojs/private.h"
#include "mikrojs/utils.h"

/* ── Options ─────────────────────────────────────────────────────── */

struct InspectOpts {
    int depth;
    int truncate;
    bool colors;
    bool show_hidden;
    std::vector<void*> seen;
    /* Nesting of [cause] rendering, capped like mik__report_uncaught */
    int cause_level = 0;
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static const char TRUNCATOR[] = "\xe2\x80\xa6";  // UTF-8 "…"

static std::string stylize(const std::string& value, MikThemeToken token, const InspectOpts& opts) {
    if (opts.colors) {
        return mik_colorize(token, value);
    }
    return value;
}

/* User getters, proxy traps and inspect hooks can throw while inspect reads
 * values; drain the exception so inspect never leaves one pending. */
static void drain_exception(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
}

/* Placeholder rendered where a property read threw */
static std::string getter_threw(JSContext* ctx, const InspectOpts& opts) {
    drain_exception(ctx);
    return stylize("[getter threw]", MIK_TOKEN_ERROR, opts);
}

static std::string truncate_str(const std::string& str, int max_len) {
    if (max_len <= 0) return TRUNCATOR;
    int slen = static_cast<int>(str.size());
    if (slen <= max_len) return str;
    if (max_len <= 1) return TRUNCATOR;
    return str.substr(0, max_len - 1) + TRUNCATOR;
}

/* Check if a key needs quoting */
static bool is_simple_key(const char* key) {
    if (!key || !*key) return false;
    char c = key[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) return false;
    for (const char* p = key + 1; *p; p++) {
        c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return false;
    }
    return true;
}

static std::string quote_key(const char* key) {
    if (is_simple_key(key)) return key;
    /* Wrap in single quotes with minimal escaping */
    std::string out = "'";
    for (const char* p = key; *p; p++) {
        if (*p == '\'')
            out += "\\'";
        else if (*p == '\\')
            out += "\\\\";
        else
            out += *p;
    }
    out += "'";
    return out;
}

/* ── Forward declaration ─────────────────────────────────────────── */

static std::string inspect_value(JSContext* ctx, JSValue value, InspectOpts& opts, int depth);
static std::string indent_continuation(const std::string& s);

/* ── Custom inspect callback state (single-threaded JS) ──────────── */

static InspectOpts* s_inspect_opts = nullptr;

static JSValue mik__custom_inspect_cb(JSContext* ctx, JSValue /*this_val*/, int argc,
                                      JSValue* argv) {
    if (argc < 1 || !s_inspect_opts) return JS_NewString(ctx, "undefined");
    int32_t depth = 2;
    if (argc >= 2) JS_ToInt32(ctx, &depth, argv[1]);
    std::string result = inspect_value(ctx, argv[0], *s_inspect_opts, depth);
    return JS_NewString(ctx, result.c_str());
}

/* Call Symbol.for('mikrojs.inspect') on value if present.
 * Returns true and sets `out` if custom inspect was used. */
static bool try_custom_inspect(JSContext* ctx, JSValue value, InspectOpts& opts, int depth,
                               std::string& out) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue symbol_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue for_fn = JS_GetPropertyStr(ctx, symbol_ctor, "for");

    JSValue key_str = JS_NewString(ctx, "mikrojs.inspect");
    JSValue inspect_sym = JS_Call(ctx, for_fn, symbol_ctor, 1, &key_str);
    JS_FreeValue(ctx, key_str);
    JS_FreeValue(ctx, for_fn);

    bool used = false;

    if (JS_IsException(inspect_sym)) {
        drain_exception(ctx);
    } else {
        JSAtom sym_atom = JS_ValueToAtom(ctx, inspect_sym);
        JSValue custom_fn = JS_GetProperty(ctx, value, sym_atom);
        JS_FreeAtom(ctx, sym_atom);
        if (JS_IsException(custom_fn)) drain_exception(ctx);

        if (JS_IsFunction(ctx, custom_fn)) {
            /* Set up callback state and create the inspect helper */
            InspectOpts* prev_opts = s_inspect_opts;
            s_inspect_opts = &opts;

            JSValue inspect_fn =
                JS_NewCFunction(ctx, mik__custom_inspect_cb, "inspect", 1);
            JSValue args[2] = {JS_NewInt32(ctx, depth), inspect_fn};
            JSValue result = JS_Call(ctx, custom_fn, value, 2, args);
            JS_FreeValue(ctx, inspect_fn);
            JS_FreeValue(ctx, args[0]);

            s_inspect_opts = prev_opts;

            if (JS_IsException(result)) drain_exception(ctx);

            if (JS_IsString(result)) {
                const char* str = JS_ToCString(ctx, result);
                if (str) {
                    out = str;
                    JS_FreeCString(ctx, str);
                    used = true;
                }
            }
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, custom_fn);
    }
    JS_FreeValue(ctx, inspect_sym);
    JS_FreeValue(ctx, symbol_ctor);
    JS_FreeValue(ctx, global);

    return used;
}

/* ── String escaping ─────────────────────────────────────────────── */

static std::string escape_string(const char* str, size_t len) {
    std::string out;
    out.reserve(len + 8);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        switch (c) {
            case '\b':
                out += "\\b";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\'':
                out += "\\'";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    /* ASCII control characters */
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    /* Pass through printable ASCII and all multi-byte UTF-8
                     * bytes (0x80+) so strings with unicode render correctly */
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

/* ── Type detection via Object.prototype.toString ────────────────── */

static std::string get_object_type(JSContext* ctx, JSValue value) {
    /* Call Object.prototype.toString.call(value) */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue object_ctor = JS_GetPropertyStr(ctx, global, "Object");
    JSValue prototype = JS_GetPropertyStr(ctx, object_ctor, "prototype");
    JSValue to_string = JS_GetPropertyStr(ctx, prototype, "toString");

    JSValue result = JS_Call(ctx, to_string, value, 0, nullptr);

    std::string type;
    if (JS_IsException(result)) {
        /* A throwing Symbol.toStringTag getter or proxy trap */
        drain_exception(ctx);
    } else {
        const char* str = JS_ToCString(ctx, result);
        if (str) {
            /* Extract "Foo" from "[object Foo]" */
            std::string_view sv(str);
            if (sv.size() > 9 && sv.substr(0, 8) == "[object " && sv.back() == ']') {
                type = sv.substr(8, sv.size() - 9);
            } else {
                type = sv;
            }
            JS_FreeCString(ctx, str);
        }
    }

    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, to_string);
    JS_FreeValue(ctx, prototype);
    JS_FreeValue(ctx, object_ctor);
    JS_FreeValue(ctx, global);

    return type;
}

/* ── Circular reference check ────────────────────────────────────── */

static bool is_seen(const InspectOpts& opts, JSValue value) {
    void* ptr = JS_VALUE_GET_PTR(value);
    for (auto* seen_ptr : opts.seen) {
        if (seen_ptr == ptr) return true;
    }
    return false;
}

/* ── Inspect list of items ───────────────────────────────────────── */

/* Inspect a JS array's elements as a comma-separated list */
static std::string inspect_array_items(JSContext* ctx, JSValue arr, uint32_t len,
                                       InspectOpts& opts, int depth) {
    std::string out;
    for (uint32_t i = 0; i < len; i++) {
        if (i > 0) out += ", ";
        JSValue item = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(item)) {
            out += getter_threw(ctx, opts);
        } else {
            out += indent_continuation(inspect_value(ctx, item, opts, depth));
        }
        JS_FreeValue(ctx, item);
    }
    return out;
}

/* ── Inspect object properties ───────────────────────────────────── */

static std::string inspect_properties(JSContext* ctx, JSValue obj, InspectOpts& opts, int depth,
                                      const char* const* skip_keys = nullptr,
                                      size_t skip_count = 0) {
    JSPropertyEnum* ptab = nullptr;
    uint32_t plen = 0;

    int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY;
    if (opts.show_hidden) {
        flags = JS_GPN_STRING_MASK;
    }

    if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, obj, flags) != 0) {
        drain_exception(ctx);
        return "";
    }

    std::string out;
    bool first = true;
    for (uint32_t i = 0; i < plen; i++) {
        const char* key = JS_AtomToCString(ctx, ptab[i].atom);
        if (!key) {
            JS_FreeAtom(ctx, ptab[i].atom);
            continue;
        }

        /* Skip specified keys */
        bool skip = false;
        if (skip_keys) {
            for (size_t j = 0; j < skip_count; j++) {
                if (strcmp(key, skip_keys[j]) == 0) {
                    skip = true;
                    break;
                }
            }
        }

        if (!skip) {
            if (!first) out += ", ";
            first = false;

            JSValue val = JS_GetPropertyStr(ctx, obj, key);
            out += quote_key(key);
            out += ": ";
            if (JS_IsException(val)) {
                out += getter_threw(ctx, opts);
            } else {
                out += indent_continuation(inspect_value(ctx, val, opts, depth));
            }
            JS_FreeValue(ctx, val);
        }

        JS_FreeCString(ctx, key);
        JS_FreeAtom(ctx, ptab[i].atom);
    }
    js_free(ctx, ptab);
    return out;
}

/* ── Type-specific inspectors ────────────────────────────────────── */

static std::string inspect_number(JSContext* ctx, JSValue value, InspectOpts& opts) {
    double d;
    JS_ToFloat64(ctx, &d, value);

    if (std::isnan(d)) return stylize("NaN", MIK_TOKEN_NUMBER, opts);
    if (d == INFINITY) return stylize("Infinity", MIK_TOKEN_NUMBER, opts);
    if (d == -INFINITY) return stylize("-Infinity", MIK_TOKEN_NUMBER, opts);
    if (d == 0.0 && std::signbit(d)) {
        return stylize("-0", MIK_TOKEN_NUMBER, opts);
    }

    /* Use JS_ToCString which formats numbers correctly (integer or float) */
    const char* str = JS_ToCString(ctx, value);
    std::string result(str ? str : "0");
    if (str) JS_FreeCString(ctx, str);
    return stylize(result, MIK_TOKEN_NUMBER, opts);
}

static std::string inspect_string(JSContext* ctx, JSValue value, InspectOpts& opts) {
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, value);
    if (!str) return stylize("''", MIK_TOKEN_STRING, opts);

    std::string escaped = escape_string(str, len);
    JS_FreeCString(ctx, str);

    if (opts.truncate > 0) {
        escaped = truncate_str(escaped, opts.truncate - 2);
    }
    return stylize("'" + escaped + "'", MIK_TOKEN_STRING, opts);
}

static std::string inspect_function(JSContext* ctx, JSValue value, InspectOpts& opts) {
    JSValue name_val = JS_GetPropertyStr(ctx, value, "name");
    if (JS_IsException(name_val)) drain_exception(ctx);
    const char* name = JS_IsException(name_val) ? nullptr : JS_ToCString(ctx, name_val);
    std::string result;

    /* Check for Symbol.toStringTag */
    std::string func_type = "Function";
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue symbol_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue tag_sym = JS_GetPropertyStr(ctx, symbol_ctor, "toStringTag");
    if (!JS_IsUndefined(tag_sym)) {
        JSAtom tag_atom = JS_ValueToAtom(ctx, tag_sym);
        JSValue tag_val = JS_GetProperty(ctx, value, tag_atom);
        if (JS_IsException(tag_val)) drain_exception(ctx);
        if (JS_IsString(tag_val)) {
            const char* tag = JS_ToCString(ctx, tag_val);
            if (tag) {
                func_type = tag;
                JS_FreeCString(ctx, tag);
            }
        }
        JS_FreeValue(ctx, tag_val);
        JS_FreeAtom(ctx, tag_atom);
    }
    JS_FreeValue(ctx, tag_sym);
    JS_FreeValue(ctx, symbol_ctor);
    JS_FreeValue(ctx, global);

    if (name && name[0]) {
        result = "[" + func_type + " " + name + "]";
    } else {
        result = "[" + func_type + "]";
    }

    if (name) JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, name_val);

    return stylize(result, MIK_TOKEN_FUNCTION, opts);
}

static std::string inspect_symbol(JSContext* ctx, JSValue value, InspectOpts& opts) {
    /* ToString throws on symbols; read the boxed description getter instead */
    JSValue desc_val = JS_GetPropertyStr(ctx, value, "description");
    std::string result = "Symbol(";
    if (JS_IsString(desc_val)) {
        const char* desc = JS_ToCString(ctx, desc_val);
        if (desc) {
            result += desc;
            JS_FreeCString(ctx, desc);
        }
    } else if (JS_IsException(desc_val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    result += ")";
    JS_FreeValue(ctx, desc_val);
    return stylize(result, MIK_TOKEN_SYMBOL, opts);
}

static std::string inspect_bigint(JSContext* ctx, JSValue value, InspectOpts& opts) {
    const char* str = JS_ToCString(ctx, value);
    std::string result;
    if (str) {
        result = std::string(str) + "n";
        JS_FreeCString(ctx, str);
    } else {
        result = "0n";
    }
    return stylize(result, MIK_TOKEN_BIGINT, opts);
}

static std::string inspect_date(JSContext* ctx, JSValue value, InspectOpts& opts) {
    JSValue to_json = JS_GetPropertyStr(ctx, value, "toJSON");
    if (JS_IsException(to_json)) drain_exception(ctx);
    if (JS_IsFunction(ctx, to_json)) {
        JSValue json_val = JS_Call(ctx, to_json, value, 0, nullptr);
        JS_FreeValue(ctx, to_json);
        if (JS_IsException(json_val)) {
            drain_exception(ctx);
            JS_FreeValue(ctx, json_val);
            return "Invalid Date";
        }
        if (JS_IsNull(json_val) || JS_IsUndefined(json_val)) {
            JS_FreeValue(ctx, json_val);
            return "Invalid Date";
        }
        const char* str = JS_ToCString(ctx, json_val);
        JS_FreeValue(ctx, json_val);
        if (str) {
            std::string result(str);
            JS_FreeCString(ctx, str);
            return stylize(result, MIK_TOKEN_DATE, opts);
        }
    } else {
        JS_FreeValue(ctx, to_json);
    }
    return "Invalid Date";
}

static std::string inspect_regexp(JSContext* ctx, JSValue value, InspectOpts& opts) {
    const char* str = JS_ToCString(ctx, value);
    if (str) {
        std::string result(str);
        JS_FreeCString(ctx, str);
        return stylize(result, MIK_TOKEN_REGEXP, opts);
    }
    drain_exception(ctx);
    return stylize("/(?:)/", MIK_TOKEN_REGEXP, opts);
}

/* Indent every line after the first by two spaces so a multi-line value
 * (an error's stack frames, its own cause) nests visibly under its parent. */
static std::string indent_continuation(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        out += c;
        if (c == '\n') out += "  ";
    }
    return out;
}

/* "\n  [cause]: <value>" for an own cause, or "". Shared by Errors and
 * plain objects, since Result errors carry cause as an ordinary field.
 * The chain is what a log line exists for, so it renders past the depth
 * limit; the caller keeps the parent on the seen list to bound cycles. */
static std::string inspect_cause(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    JSValue cause = JS_GetPropertyStr(ctx, value, "cause");
    if (JS_IsException(cause)) {
        drain_exception(ctx);
        return "";
    }
    /* Same cap as mik__report_uncaught; bounds recursion on the JS task stack */
    static const int MAX_CAUSE_LEVEL = 4;
    std::string out;
    if (!JS_IsUndefined(cause)) {
        out = "\n  [cause]: ";
        if (opts.cause_level >= MAX_CAUSE_LEVEL) {
            out += TRUNCATOR;
        } else {
            opts.cause_level++;
            out += indent_continuation(inspect_value(ctx, cause, opts, depth < 1 ? 1 : depth));
            opts.cause_level--;
        }
    }
    JS_FreeValue(ctx, cause);
    return out;
}

/* Errors render as
 *   Name: message { extra: props }
 *       at frame (file:line:col)
 *     [cause]: <inspected cause>
 * The header keeps name, message and every own enumerable field (code,
 * errno, ...) on one line; the stack and the cause chain follow. */
static std::string inspect_error(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    /* Get name and message */
    JSValue name_val = JS_GetPropertyStr(ctx, value, "name");
    if (JS_IsException(name_val)) drain_exception(ctx);
    JSValue msg_val = JS_GetPropertyStr(ctx, value, "message");
    if (JS_IsException(msg_val)) drain_exception(ctx);

    const char* name = JS_IsException(name_val) ? nullptr : JS_ToCString(ctx, name_val);
    const char* msg = JS_IsException(msg_val) ? nullptr : JS_ToCString(ctx, msg_val);

    std::string result;
    if (name) result = name;
    else result = "Error";

    if (msg && msg[0]) {
        result += ": ";
        result += msg;
    }

    if (name) JS_FreeCString(ctx, name);
    if (msg) JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, name_val);
    JS_FreeValue(ctx, msg_val);

    if (is_seen(opts, value)) return result + " [Circular]";
    opts.seen.push_back(JS_VALUE_GET_PTR(value));

    /* Inspect extra properties (skip standard error keys; cause is rendered
     * below so it reads the same whether QuickJS or a panic defined it) */
    static const char* const error_keys[] = {"stack",     "line",       "column",     "name",
                                             "message",   "fileName",   "lineNumber", "columnNumber",
                                             "number",    "description", "cause"};
    std::string props =
        inspect_properties(ctx, value, opts, depth, error_keys, countof(error_keys));
    if (!props.empty()) {
        result += " { " + props + " }";
    }

    /* QuickJS stacks are frame lines only ("    at ..."), no header line */
    JSValue stack_val = JS_GetPropertyStr(ctx, value, "stack");
    if (JS_IsException(stack_val)) drain_exception(ctx);
    if (JS_IsString(stack_val)) {
        const char* stack = JS_ToCString(ctx, stack_val);
        if (stack) {
            std::string frames(stack);
            JS_FreeCString(ctx, stack);
            while (!frames.empty() && frames.back() == '\n') frames.pop_back();
            if (!frames.empty()) {
                result += "\n";
                result += frames;
            }
        }
    }
    JS_FreeValue(ctx, stack_val);

    result += inspect_cause(ctx, value, opts, depth);

    opts.seen.pop_back();
    return result;
}

static std::string inspect_array(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    JSValue len_val = JS_GetPropertyStr(ctx, value, "length");
    uint32_t len = 0;
    if (JS_IsException(len_val)) {
        drain_exception(ctx);
    } else {
        JS_ToUint32(ctx, &len, len_val);
    }
    JS_FreeValue(ctx, len_val);

    if (len == 0) {
        /* Check for non-index properties */
        JSPropertyEnum* ptab = nullptr;
        uint32_t plen = 0;
        int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY;
        if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, value, flags) == 0) {
            /* All indexed properties would have numeric keys */
            bool has_non_index = plen > len;
            for (uint32_t i = 0; i < plen; i++) {
                JS_FreeAtom(ctx, ptab[i].atom);
            }
            js_free(ctx, ptab);
            if (!has_non_index) return "[]";
        } else {
            drain_exception(ctx);
            return "[]";
        }
    }

    if (is_seen(opts, value)) return "[Circular]";

    opts.seen.push_back(JS_VALUE_GET_PTR(value));

    std::string items = inspect_array_items(ctx, value, len, opts, depth);

    opts.seen.pop_back();

    return "[ " + items + " ]";
}

static std::string inspect_map(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    JSValue size_val = JS_GetPropertyStr(ctx, value, "size");
    int32_t size = 0;
    if (JS_IsException(size_val)) {
        drain_exception(ctx);
    } else {
        JS_ToInt32(ctx, &size, size_val);
    }
    JS_FreeValue(ctx, size_val);

    if (size <= 0) return "Map{}";

    if (is_seen(opts, value)) return "[Circular]";
    opts.seen.push_back(JS_VALUE_GET_PTR(value));

    /* Get entries via entries() iterator */
    JSValue entries_fn = JS_GetPropertyStr(ctx, value, "entries");
    JSValue iterator = JS_Call(ctx, entries_fn, value, 0, nullptr);
    JS_FreeValue(ctx, entries_fn);

    if (JS_IsException(iterator)) {
        drain_exception(ctx);
        JS_FreeValue(ctx, iterator);
        opts.seen.pop_back();
        return "Map{}";
    }

    JSValue next_fn = JS_GetPropertyStr(ctx, iterator, "next");

    std::string items;
    bool first = true;
    for (int i = 0; i < size; i++) {
        JSValue step = JS_Call(ctx, next_fn, iterator, 0, nullptr);
        if (JS_IsException(step)) {
            drain_exception(ctx);
            JS_FreeValue(ctx, step);
            break;
        }
        JSValue done = JS_GetPropertyStr(ctx, step, "done");
        if (JS_ToBool(ctx, done)) {
            JS_FreeValue(ctx, done);
            JS_FreeValue(ctx, step);
            break;
        }
        JS_FreeValue(ctx, done);

        JSValue entry = JS_GetPropertyStr(ctx, step, "value");
        JSValue key = JS_GetPropertyUint32(ctx, entry, 0);
        JSValue val = JS_GetPropertyUint32(ctx, entry, 1);

        if (!first) items += ", ";
        first = false;
        items += indent_continuation(inspect_value(ctx, key, opts, depth));
        items += " => ";
        items += indent_continuation(inspect_value(ctx, val, opts, depth));

        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, entry);
        JS_FreeValue(ctx, step);
    }

    JS_FreeValue(ctx, next_fn);
    JS_FreeValue(ctx, iterator);

    opts.seen.pop_back();

    return "Map{ " + items + " }";
}

static std::string inspect_set(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    JSValue size_val = JS_GetPropertyStr(ctx, value, "size");
    int32_t size = 0;
    if (JS_IsException(size_val)) {
        drain_exception(ctx);
    } else {
        JS_ToInt32(ctx, &size, size_val);
    }
    JS_FreeValue(ctx, size_val);

    if (size == 0) return "Set{}";

    if (is_seen(opts, value)) return "[Circular]";
    opts.seen.push_back(JS_VALUE_GET_PTR(value));

    /* Get values via forEach */
    JSValue for_each_fn = JS_GetPropertyStr(ctx, value, "forEach");

    /* Build array of values to inspect */
    std::string items;
    JSValue values_fn = JS_GetPropertyStr(ctx, value, "values");
    JSValue iterator = JS_Call(ctx, values_fn, value, 0, nullptr);
    JS_FreeValue(ctx, values_fn);
    JS_FreeValue(ctx, for_each_fn);

    if (JS_IsException(iterator)) {
        drain_exception(ctx);
        JS_FreeValue(ctx, iterator);
        opts.seen.pop_back();
        return "Set{}";
    }

    JSValue next_fn = JS_GetPropertyStr(ctx, iterator, "next");
    bool first = true;
    for (int i = 0; i < size; i++) {
        JSValue step = JS_Call(ctx, next_fn, iterator, 0, nullptr);
        if (JS_IsException(step)) {
            drain_exception(ctx);
            JS_FreeValue(ctx, step);
            break;
        }
        JSValue done = JS_GetPropertyStr(ctx, step, "done");
        if (JS_ToBool(ctx, done)) {
            JS_FreeValue(ctx, done);
            JS_FreeValue(ctx, step);
            break;
        }
        JS_FreeValue(ctx, done);

        JSValue val = JS_GetPropertyStr(ctx, step, "value");
        if (!first) items += ", ";
        first = false;
        items += indent_continuation(inspect_value(ctx, val, opts, depth));
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, step);
    }

    JS_FreeValue(ctx, next_fn);
    JS_FreeValue(ctx, iterator);

    opts.seen.pop_back();

    return "Set{ " + items + " }";
}

static std::string inspect_typed_array(JSContext* ctx, JSValue value, const std::string& type_name,
                                       InspectOpts& opts) {
    size_t byte_length;
    size_t byte_offset;
    size_t bytes_per_element;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &byte_length, &bytes_per_element);
    if (JS_IsException(buffer)) drain_exception(ctx);
    JS_FreeValue(ctx, buffer);

    JSValue len_val = JS_GetPropertyStr(ctx, value, "length");
    uint32_t len = 0;
    if (JS_IsException(len_val)) {
        drain_exception(ctx);
    } else {
        JS_ToUint32(ctx, &len, len_val);
    }
    JS_FreeValue(ctx, len_val);

    if (len == 0) return type_name + "[]";

    std::string items;
    for (uint32_t i = 0; i < len; i++) {
        if (i > 0) items += ", ";
        JSValue elem = JS_GetPropertyUint32(ctx, value, i);
        if (JS_IsException(elem)) {
            JS_FreeValue(ctx, elem);
            items += getter_threw(ctx, opts);
            continue;
        }
        const char* str = JS_ToCString(ctx, elem);
        JS_FreeValue(ctx, elem);
        items += stylize(str ? str : "0", MIK_TOKEN_NUMBER, opts);
        if (str) JS_FreeCString(ctx, str);
    }

    return type_name + "[ " + items + " ]";
}

static std::string inspect_object(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    if (is_seen(opts, value)) return "[Circular]";

    /* Check for empty object */
    JSPropertyEnum* ptab = nullptr;
    uint32_t plen = 0;
    int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY;
    if (opts.show_hidden) {
        flags = JS_GPN_STRING_MASK;
    }

    if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, value, flags) != 0) {
        drain_exception(ctx);
        return "{}";
    }
    for (uint32_t i = 0; i < plen; i++) {
        JS_FreeAtom(ctx, ptab[i].atom);
    }
    js_free(ctx, ptab);

    if (plen == 0) return "{}";

    /* A cause field chains below the braces like an Error's, so a Result
     * error wrapped around another keeps its whole chain readable */
    static const char* const cause_key[] = {"cause"};
    opts.seen.push_back(JS_VALUE_GET_PTR(value));
    std::string props = inspect_properties(ctx, value, opts, depth, cause_key, 1);
    std::string result = props.empty() ? "{}" : "{ " + props + " }";
    result += inspect_cause(ctx, value, opts, depth);
    opts.seen.pop_back();

    return result;
}

static std::string inspect_class(JSContext* ctx, JSValue value, const std::string& type_name,
                                 InspectOpts& opts, int depth) {
    /* Get constructor name or toStringTag */
    std::string name = type_name;

    if (name.empty() || name == "Object") {
        JSValue ctor = JS_GetPropertyStr(ctx, value, "constructor");
        if (JS_IsException(ctor)) drain_exception(ctx);
        if (JS_IsFunction(ctx, ctor)) {
            JSValue ctor_name = JS_GetPropertyStr(ctx, ctor, "name");
            if (JS_IsException(ctor_name)) drain_exception(ctx);
            const char* n =
                JS_IsException(ctor_name) ? nullptr : JS_ToCString(ctx, ctor_name);
            if (n && n[0] && strcmp(n, "Object") != 0) {
                name = n;
            }
            if (n) JS_FreeCString(ctx, n);
            JS_FreeValue(ctx, ctor_name);
        }
        JS_FreeValue(ctx, ctor);
    }

    std::string obj_str = inspect_object(ctx, value, opts, depth);
    if (!name.empty() && name != "Object") {
        return name + obj_str;
    }
    return obj_str;
}

/* ── Main dispatch ───────────────────────────────────────────────── */

static std::string inspect_value(JSContext* ctx, JSValue value, InspectOpts& opts, int depth) {
    /* Primitives */
    if (JS_IsUndefined(value)) {
        return stylize("undefined", MIK_TOKEN_UNDEFINED, opts);
    }
    if (JS_IsNull(value)) {
        return stylize("null", MIK_TOKEN_NULL, opts);
    }
    if (JS_IsBool(value)) {
        return stylize(JS_ToBool(ctx, value) ? "true" : "false", MIK_TOKEN_BOOLEAN, opts);
    }
    if (JS_IsNumber(value)) {
        return inspect_number(ctx, value, opts);
    }
    if (JS_IsBigInt(value)) {
        return inspect_bigint(ctx, value, opts);
    }
    if (JS_IsString(value)) {
        return inspect_string(ctx, value, opts);
    }
    if (JS_IsSymbol(value)) {
        return inspect_symbol(ctx, value, opts);
    }

    /* Functions */
    if (JS_IsFunction(ctx, value)) {
        return inspect_function(ctx, value, opts);
    }

    /* Depth check for objects */
    if (depth <= 0) {
        if (JS_IsArray(value)) {
            JSValue len_val = JS_GetPropertyStr(ctx, value, "length");
            uint32_t len = 0;
            if (JS_IsException(len_val)) {
                drain_exception(ctx);
            } else {
                JS_ToUint32(ctx, &len, len_val);
            }
            JS_FreeValue(ctx, len_val);
            char buf[32];
            snprintf(buf, sizeof(buf), "[Array(%u)]", len);
            return buf;
        }
        return "[Object]";
    }

    /* Check for custom inspect: Symbol.for('mikrojs.inspect') */
    std::string custom;
    if (try_custom_inspect(ctx, value, opts, depth, custom)) {
        return custom;
    }

    /* Objects — detect type */
    std::string type = get_object_type(ctx, value);

    if (JS_IsArray(value) || type == "Array") {
        return inspect_array(ctx, value, opts, depth - 1);
    }
    if (JS_IsError(value) || type == "Error") {
        return inspect_error(ctx, value, opts, depth - 1);
    }
    if (type == "Date") {
        return inspect_date(ctx, value, opts);
    }
    if (type == "RegExp") {
        return inspect_regexp(ctx, value, opts);
    }
    if (type == "Map") {
        return inspect_map(ctx, value, opts, depth - 1);
    }
    if (type == "Set") {
        return inspect_set(ctx, value, opts, depth - 1);
    }
    if (type == "Promise") {
        return stylize("Promise{\xe2\x80\xa6}", MIK_TOKEN_IDENTIFIER, opts);
    }
    if (type == "WeakMap") {
        return stylize("WeakMap{\xe2\x80\xa6}", MIK_TOKEN_IDENTIFIER, opts);
    }
    if (type == "WeakSet") {
        return stylize("WeakSet{\xe2\x80\xa6}", MIK_TOKEN_IDENTIFIER, opts);
    }

    /* Typed arrays */
    if (type == "Int8Array" || type == "Uint8Array" || type == "Uint8ClampedArray" ||
        type == "Int16Array" || type == "Uint16Array" || type == "Int32Array" ||
        type == "Uint32Array" || type == "Float32Array" || type == "Float64Array") {
        return inspect_typed_array(ctx, value, type, opts);
    }

    /* Plain object vs class instance */
    JSValue proto = JS_GetPrototype(ctx, value);
    if (JS_IsException(proto)) drain_exception(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj_ctor = JS_GetPropertyStr(ctx, global, "Object");
    JSValue obj_proto = JS_GetPropertyStr(ctx, obj_ctor, "prototype");

    bool is_plain = JS_IsNull(proto) ||
                    (JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(obj_proto));

    JS_FreeValue(ctx, obj_proto);
    JS_FreeValue(ctx, obj_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);

    if (is_plain) {
        return inspect_object(ctx, value, opts, depth - 1);
    }

    return inspect_class(ctx, value, type, opts, depth - 1);
}

/* ── Public API ──────────────────────────────────────────────────── */

std::string mik_inspect(JSContext* ctx, JSValue value, int depth, bool colors, bool show_hidden) {
    InspectOpts opts;
    opts.depth = depth;
    opts.truncate = 0;  /* 0 = unlimited */
    opts.colors = colors;
    opts.show_hidden = show_hidden;
    return inspect_value(ctx, value, opts, depth);
}

/* ── JS-callable inspect function (for mikro/inspect module) ───── */

static JSValue mik__inspect_js(JSContext* ctx, JSValue this_val, int argc, JSValue* argv) {
    if (argc < 1) return JS_NewString(ctx, "undefined");

    int depth = 2;
    bool colors = false;
    bool show_hidden = false;

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue d = JS_GetPropertyStr(ctx, argv[1], "depth");
        if (JS_IsException(d)) drain_exception(ctx);
        if (JS_IsNumber(d)) {
            int32_t d32;
            JS_ToInt32(ctx, &d32, d);
            depth = d32;
        }
        JS_FreeValue(ctx, d);

        JSValue c = JS_GetPropertyStr(ctx, argv[1], "colors");
        if (JS_IsException(c)) drain_exception(ctx);
        else if (!JS_IsUndefined(c)) colors = JS_ToBool(ctx, c);
        JS_FreeValue(ctx, c);

        JSValue h = JS_GetPropertyStr(ctx, argv[1], "showHidden");
        if (JS_IsException(h)) drain_exception(ctx);
        else if (!JS_IsUndefined(h)) show_hidden = JS_ToBool(ctx, h);
        JS_FreeValue(ctx, h);
    }

    std::string result = mik_inspect(ctx, argv[0], depth, colors, show_hidden);
    return JS_NewString(ctx, result.c_str());
}

/* Module init for 'mikro/inspect' native C module */
int mik__inspect_module_init(JSContext* ctx, JSModuleDef* m) {
    return JS_SetModuleExport(ctx, m, "inspect",
                              JS_NewCFunction(ctx, mik__inspect_js, "inspect", 1));
}

void mik__inspect_register(JSContext* ctx) {
    JSModuleDef* m = JS_NewCModule(ctx, "mikro/inspect", mik__inspect_module_init);
    if (m) {
        JS_AddModuleExport(ctx, m, "inspect");
    }
}
