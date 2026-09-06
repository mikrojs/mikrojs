#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include <quickjs.h>

#include "mikrojs/platform.h"
#include "mikrojs/private.h"
#include "mikrojs/utils.h"

/* ── Printf-style format (port of format.ts) ────────────────────── */

static std::string mik__format(JSContext* ctx, int argc, JSValue* argv, bool colors) {
    if (argc == 0) return "";

    /* If first arg is not a string, just inspect all args */
    if (!JS_IsString(argv[0])) {
        std::string out;
        for (int i = 0; i < argc; i++) {
            if (i > 0) out += ", ";
            out += mik_inspect(ctx, argv[i], 2, colors);
        }
        return out;
    }

    const char* fmt = JS_ToCString(ctx, argv[0]);
    if (!fmt) return "";

    std::string result;
    int arg_idx = 1;  /* next arg to consume */

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            result += *p;
            continue;
        }

        /* Look at next char */
        char next = *(p + 1);
        if (next == '\0') {
            result += '%';
            break;
        }

        if (next == '%') {
            result += '%';
            p++;
            continue;
        }

        if (arg_idx >= argc) {
            /* No more args — keep the format specifier literal */
            result += '%';
            result += next;
            p++;
            continue;
        }

        switch (next) {
            case 's': {
                const char* s = JS_ToCString(ctx, argv[arg_idx]);
                if (s) {
                    result += s;
                    JS_FreeCString(ctx, s);
                }
                arg_idx++;
                p++;
                break;
            }
            case 'd': {
                const char* s = JS_ToCString(ctx, argv[arg_idx]);
                if (s) {
                    result += s;
                    JS_FreeCString(ctx, s);
                }
                arg_idx++;
                p++;
                break;
            }
            case 'f': {
                double d;
                JS_ToFloat64(ctx, &d, argv[arg_idx]);
                char buf[64];
                snprintf(buf, sizeof(buf), "%f", d);
                result += buf;
                arg_idx++;
                p++;
                break;
            }
            case 'o':
            case 'O': {
                result += mik_inspect(ctx, argv[arg_idx], 2, colors);
                arg_idx++;
                p++;
                break;
            }
            case 'c': {
                /* CSS colors not supported — consume arg, output nothing */
                arg_idx++;
                p++;
                break;
            }
            default:
                result += '%';
                result += next;
                p++;
                break;
        }
    }

    JS_FreeCString(ctx, fmt);

    /* Append remaining args */
    for (int i = arg_idx; i < argc; i++) {
        result += ' ';
        result += mik_inspect(ctx, argv[i], 2, colors);
    }

    return result;
}

/* ── Console methods ─────────────────────────────────────────────── */

static JSValue mik__console_log(JSContext* ctx, JSValue this_val, int argc, JSValue* argv) {
    std::string output = mik__format(ctx, argc, argv, true);

    if (mik__repl_is_protocol_mode()) {
        mik__repl_proto_send_output(MIK_MSG_LOG, output.c_str(), output.size());
        return JS_UNDEFINED;
    }

    output += "\r\n";
    MIK_GetPlatform()->stdout_write(output.c_str(), output.size());
    fsync(fileno(stdout));
    return JS_UNDEFINED;
}

static JSValue mik__console_info(JSContext* ctx, JSValue this_val, int argc, JSValue* argv) {
    std::string output = mik__format(ctx, argc, argv, true);

    if (mik__repl_is_protocol_mode()) {
        mik__repl_proto_send_output(MIK_MSG_INFO, output.c_str(), output.size());
        return JS_UNDEFINED;
    }

    output += "\r\n";
    MIK_GetPlatform()->stdout_write(output.c_str(), output.size());
    fsync(fileno(stdout));
    return JS_UNDEFINED;
}

static JSValue mik__console_debug(JSContext* ctx, JSValue this_val, int argc, JSValue* argv) {
    std::string output = mik__format(ctx, argc, argv, true);

    if (mik__repl_is_protocol_mode()) {
        mik__repl_proto_send_output(MIK_MSG_DEBUG, output.c_str(), output.size());
        return JS_UNDEFINED;
    }

    output += "\r\n";
    MIK_GetPlatform()->stdout_write(output.c_str(), output.size());
    fsync(fileno(stdout));
    return JS_UNDEFINED;
}

static JSValue mik__console_error_warn(JSContext* ctx, JSValue this_val, int argc, JSValue* argv,
                                       int magic) {
    uint8_t msg_type = magic == 0 ? MIK_MSG_ERROR : MIK_MSG_WARN;

    /* Error arguments render through inspect: name, message, extra fields,
     * stack and cause chain. Same output as console.log for the same value. */
    std::string output = mik__format(ctx, argc, argv, true);

    if (mik__repl_is_protocol_mode()) {
        mik__repl_proto_send_output(msg_type, output.c_str(), output.size());
        return JS_UNDEFINED;
    }

    output += "\r\n";
    MIK_GetPlatform()->stderr_write(output.c_str(), output.size());
    return JS_UNDEFINED;
}

/* ── Uncaught error reporting ─────────────────────────────────────── */

/* Bounded stack-buffer writer for the uncaught path: never touches the
 * C++ heap (see mik__report_uncaught). Silently truncates when full. */
struct MIKBoundedBuf {
    char* buf;
    size_t cap;
    size_t pos = 0;
    void append(const char* s, size_t len) {
        if (pos >= cap - 1) return;
        size_t avail = cap - 1 - pos;
        size_t n = len < avail ? len : avail;
        memcpy(buf + pos, s, n);
        pos += n;
    }
    void cstr(const char* s) {
        if (s) append(s, strlen(s));
    }
};

/* One primitive as a log token (strings quoted). Objects print as
 * [object]: nested inspection would need the C++ heap. */
static void mik__append_primitive(JSContext* ctx, JSValue v, MIKBoundedBuf& out) {
    if (JS_IsObject(v)) {
        out.cstr("[object]");
        return;
    }
    /* Allocation-free spellings first: a null rejection is how QuickJS
     * reports OOM, so this must work with no heap at all */
    if (JS_IsNull(v)) {
        out.cstr("null");
        return;
    }
    if (JS_IsUndefined(v)) {
        out.cstr("undefined");
        return;
    }
    if (JS_IsBool(v)) {
        out.cstr(JS_ToBool(ctx, v) ? "true" : "false");
        return;
    }
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        char num[16];
        int n = snprintf(num, sizeof(num), "%d", JS_VALUE_GET_INT(v));
        if (n > 0) out.append(num, (size_t)n);
        return;
    }
    bool quote = JS_IsString(v);
    /* JS-heap allocation only; returns NULL on OOM instead of throwing */
    const char* s = JS_ToCString(ctx, v);
    if (!s) {
        /* Symbols reject ToString */
        JS_FreeValue(ctx, JS_GetException(ctx));
        out.cstr("[primitive]");
        return;
    }
    if (quote) out.cstr("'");
    out.cstr(s);
    if (quote) out.cstr("'");
    JS_FreeCString(ctx, s);
}

/* "Name: message { extra: fields }" then the stack frames, `indent`
 * prefixed to every frame line. Also fits a plain object (a Result error
 * as a panic cause has name/message and no stack). */
static void mik__append_error_head(JSContext* ctx, JSValue exc, const char* indent, MIKBoundedBuf& out) {
    JSValue name_val = JS_GetPropertyStr(ctx, exc, "name");
    JSValue msg_val = JS_GetPropertyStr(ctx, exc, "message");
    if (JS_IsException(name_val) || JS_IsException(msg_val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    /* Non-string name/message count as absent (a native Result error has
     * code + message and no name) */
    const char* name = JS_IsString(name_val) ? JS_ToCString(ctx, name_val) : nullptr;
    const char* emsg = JS_IsString(msg_val) ? JS_ToCString(ctx, msg_val) : nullptr;

    if (name && name[0]) {
        out.cstr(name);
        if (emsg && emsg[0]) {
            out.cstr(": ");
            out.cstr(emsg);
        }
    } else if (emsg && emsg[0]) {
        out.cstr(emsg);
    } else {
        out.cstr("[object]");
    }

    if (name) JS_FreeCString(ctx, name);
    if (emsg) JS_FreeCString(ctx, emsg);
    JS_FreeValue(ctx, name_val);
    JS_FreeValue(ctx, msg_val);

    /* Own enumerable fields beyond the standard ones: code, errno, path... */
    JSPropertyEnum* ptab = nullptr;
    uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, exc, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) ==
        0) {
        bool first = true;
        for (uint32_t i = 0; i < plen; i++) {
            const char* key = JS_AtomToCString(ctx, ptab[i].atom);
            if (key && strcmp(key, "name") != 0 && strcmp(key, "message") != 0 &&
                strcmp(key, "stack") != 0 && strcmp(key, "cause") != 0) {
                JSValue v = JS_GetProperty(ctx, exc, ptab[i].atom);
                if (JS_IsException(v)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                } else {
                    out.cstr(first ? " { " : ", ");
                    first = false;
                    out.cstr(key);
                    out.cstr(": ");
                    mik__append_primitive(ctx, v, out);
                }
                JS_FreeValue(ctx, v);
            }
            if (key) JS_FreeCString(ctx, key);
            JS_FreeAtom(ctx, ptab[i].atom);
        }
        if (!first) out.cstr(" }");
        js_free(ctx, ptab);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }

    JSValue stack_val = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack_val)) {
        const char* stack = JS_ToCString(ctx, stack_val);
        if (stack) {
            const char* p = stack;
            while (*p) {
                const char* nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (len) {
                    out.cstr("\n");
                    out.cstr(indent);
                    out.append(p, len);
                }
                if (!nl) break;
                p = nl + 1;
            }
            JS_FreeCString(ctx, stack);
        }
    } else if (JS_IsException(stack_val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, stack_val);
}

/* Dedup across report paths: one error can reach this function more than
 * once for a single failure. The sync throw path (mik_dump_error) can report
 * an error whose promise still sits in the deferred flush queue, e.g. an
 * entry module whose rejected eval promise is read by value while the host
 * dumps the pending exception. Mark the error object the first
 * time it is reported and skip it afterwards. Keying on object identity is
 * exact and time-independent; a recycled address belongs to a fresh object
 * with no marker, so distinct errors are never suppressed. Primitive
 * rejections (e.g. a null OOM rejection) can't hold a marker and are always
 * reported. Returns true if it reported, false if it was a duplicate. */
bool mik__report_uncaught(JSContext* ctx, JSValue exc, bool in_promise) {
    if (JS_IsObject(exc)) {
        JSAtom marker = JS_NewAtom(ctx, "\xff" "mik_reported");
        if (marker != JS_ATOM_NULL) {
            if (JS_HasProperty(ctx, exc, marker) > 0) {
                JS_FreeAtom(ctx, marker);
                return false;
            }
            /* Non-enumerable, non-writable, non-configurable so it stays
             * invisible to user code. Best-effort: JS_* calls return error
             * codes (no C++ throw), so under OOM we just report without
             * dedup rather than abort. */
            JS_DefinePropertyValue(ctx, exc, marker, JS_TRUE, 0);
            JS_FreeAtom(ctx, marker);
        }
    }

    /* A blocking-watchdog timeout reaches here either through mik_dump_error
     * (which already reported it; this is then a no-op) or as an unhandled
     * rejection when the interrupted turn was a module evaluation. */
    mik__watchdog_report_blocking(MIK_GetRuntime(ctx));

    /* Fixed-size stack buffer so this function never allocates from the
     * C++ heap. Previously we used std::string + mik_inspect here; both
     * can throw std::bad_alloc under memory pressure, and with
     * CONFIG_COMPILER_CXX_EXCEPTIONS=n on ESP32 that turns into
     * abort() — masking the real OOM the reporter was trying to surface.
     * Messages longer than the buffer get truncated, which beats
     * aborting. */
    char buf[1024];
    MIKBoundedBuf out{buf, sizeof(buf)};

    out.cstr(in_promise ? "Uncaught (in promise) " : "Uncaught ");

    if (JS_IsObject(exc)) {
        mik__append_error_head(ctx, exc, "", out);

        /* Cause chain: a panic's cause is the Result error that triggered
         * it, so this is usually the line that says what actually failed.
         * Bounded depth; `seen` cuts cycles. */
        static const char* const indents[] = {"", "  ", "    ", "      ", "        "};
        void* seen[countof(indents)] = {JS_VALUE_GET_PTR(exc)};
        JSValue cur = JS_DupValue(ctx, exc);
        for (size_t level = 1; level < countof(indents); level++) {
            JSValue cause = JS_GetPropertyStr(ctx, cur, "cause");
            JS_FreeValue(ctx, cur);
            cur = cause;
            if (JS_IsException(cause)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                break;
            }
            if (JS_IsUndefined(cause)) break;
            out.cstr("\n");
            out.cstr(indents[level]);
            out.cstr("[cause]: ");
            if (!JS_IsObject(cause)) {
                mik__append_primitive(ctx, cause, out);
                break;
            }
            bool cyclic = false;
            for (size_t j = 0; j < level; j++) {
                if (seen[j] == JS_VALUE_GET_PTR(cause)) cyclic = true;
            }
            if (cyclic) {
                out.cstr("[Circular]");
                break;
            }
            seen[level] = JS_VALUE_GET_PTR(cause);
            mik__append_error_head(ctx, cause, indents[level], out);
        }
        JS_FreeValue(ctx, cur);
    } else {
        /* Describe primitive rejections by type without going through
         * mik_inspect (which allocates a std::string). */
        if (JS_IsString(exc)) {
            const char* s = JS_ToCString(ctx, exc);
            if (s) {
                out.cstr(s);
                JS_FreeCString(ctx, s);
            }
        } else {
            mik__append_primitive(ctx, exc, out);
        }

        /* Primitive rejections lose the throw-site stack (the async
         * chain has already unwound). Attach current system-heap free
         * bytes so logs hint at whether this was an allocation failure
         * — QuickJS surfaces OOM as a null rejection when it can't
         * allocate an Error object for the real throw. */
        const MIKPlatform* plat = MIK_GetPlatform();
        if (plat && plat->get_free_system_mem) {
            char note[96];
            int n = snprintf(note, sizeof(note),
                             "\n  (primitive rejection, systemFree=%zuB at report time)",
                             plat->get_free_system_mem());
            if (n > 0) out.append(note, (size_t)n);
        }
    }

    if (mik__repl_is_protocol_mode()) {
        mik__repl_proto_send_output(MIK_MSG_ERROR, buf, out.pos);
        return true;
    }

    out.cstr("\r\n");
    MIK_GetPlatform()->stderr_write(buf, out.pos);
    return true;
}

/* ── Test emit ────────────────────────────────────────────────────── */

static JSValue mik__test_emit(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    if (mik__repl_is_protocol_mode()) {
        mik__repl_proto_send_output(MIK_MSG_TEST, str, len);
    } else {
        /* Non-protocol mode (e.g. Node addon for the simulator): if the host
         * registered a handler via MIK_SetTestEmitHandler, route the payload
         * there. Without a handler, this is a no-op — same as before. */
        MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
        if (mik_rt && mik_rt->test_emit_fn) {
            mik_rt->test_emit_fn(str, len, mik_rt->test_emit_opaque);
        }
    }
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* Signals the current ServeLoop to return so a supervisor driving multiple
 * test files through one transport session can tear down the runtime and
 * move to the next file. Called from the test runtime after emitting the
 * final run_done event. No-op when not running under a supervisor. */
static JSValue mik__test_file_done(JSContext* /*ctx*/, JSValue /*this_val*/, int /*argc*/,
                                   JSValue* /*argv*/) {
    MIK_ProtocolExit();
    return JS_UNDEFINED;
}

/* ── Init ────────────────────────────────────────────────────────── */

void mik__console_init(JSContext* ctx, JSValue global_obj) {
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, mik__console_log, "log", 0));
    JS_SetPropertyStr(ctx, console, "info", JS_NewCFunction(ctx, mik__console_info, "info", 0));
    JS_SetPropertyStr(ctx, console, "debug", JS_NewCFunction(ctx, mik__console_debug, "debug", 0));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunctionMagic(ctx, mik__console_error_warn, "warn", 0,
                                           JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunctionMagic(ctx, mik__console_error_warn, "error", 0,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global_obj, "console", console);
}

/* Public: opt-in test helper installation. Skipped for ordinary runtimes so
 * they don't pay the cost of two JSCFunction allocations + global property
 * bindings they'll never use. The mikrojs/test built-in has a console.log
 * fallback for when these globals aren't present. */
void MIK_EnableTestHelpers(MIKRuntime* mik_rt) {
    mik_rt->test_mode = true;
    JSContext* ctx = mik_rt->ctx;
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "__testEmit",
                      JS_NewCFunction(ctx, mik__test_emit, "__testEmit", 1));
    JS_SetPropertyStr(ctx, global_obj, "__testFileDone",
                      JS_NewCFunction(ctx, mik__test_file_done, "__testFileDone", 0));
    JS_FreeValue(ctx, global_obj);
}
