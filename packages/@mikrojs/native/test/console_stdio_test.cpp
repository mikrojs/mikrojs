#include <cstring>
#include <string>

#include <mikrojs/mikrojs.h>
#include <mikrojs/platform.h>
#include <mikrojs/private.h>
#include <quickjs.h>

#include <doctest.h>

/* Host-side tests for console (mik_console.cpp + mik_inspect.cpp +
 * mik_color.cpp) and the stdio native module (mik_stdio.cpp), through a
 * platform whose stdout/stderr/stdin are captured/scripted in-process. */

namespace {

static std::string g_stdout;
static std::string g_stderr;
static std::string g_stdin_data;

static int cap_stdout(const void* buf, size_t len) {
    g_stdout.append(static_cast<const char*>(buf), len);
    return (int)len;
}

static int cap_stderr(const void* buf, size_t len) {
    g_stderr.append(static_cast<const char*>(buf), len);
    return (int)len;
}

static int fake_stdin_read(void* buf, size_t len) {
    if (g_stdin_data.empty()) return 0;
    size_t n = g_stdin_data.size() < len ? g_stdin_data.size() : len;
    memcpy(buf, g_stdin_data.data(), n);
    g_stdin_data.erase(0, n);
    return (int)n;
}

struct CaptureFixture {
    const MIKPlatform* orig = nullptr;
    MIKPlatform fake;
    MIKRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    CaptureFixture() {
        g_stdout.clear();
        g_stderr.clear();
        g_stdin_data.clear();
        orig = MIK_GetPlatform();
        fake = *orig;
        fake.stdout_write = cap_stdout;
        fake.stderr_write = cap_stderr;
        fake.stdin_read = fake_stdin_read;
        MIK_SetPlatform(&fake);
        rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        ctx = MIK_GetJSContext(rt);
    }

    ~CaptureFixture() {
        MIK_FreeRuntime(rt);
        MIK_SetPlatform(orig);
    }
};

static void run(JSContext* ctx, const char* src) {
    std::string code = src;
    JSValue rv = JS_Eval(ctx, code.c_str(), code.size(), "mikro/test-console-driver",
                         JS_EVAL_TYPE_MODULE);
    REQUIRE(!JS_IsException(rv));
    JSPromiseStateEnum state = JS_PromiseState(ctx, rv);
    if (state == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, rv);
        const char* s = JS_ToCString(ctx, reason);
        if (s) {
            fprintf(stderr, "[console run] rejected: %s\n", s);
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, reason);
    }
    JS_FreeValue(ctx, rv);
    REQUIRE(state == JS_PROMISE_FULFILLED);
}

/* Console output is colorized (ANSI escapes); strip them so assertions
 * compare visible text. mik_color coverage comes from producing them. */
static std::string strip_ansi(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '\033' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && in[i] != 'm') i++;
            continue;
        }
        out += in[i];
    }
    return out;
}

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE_FIXTURE(CaptureFixture, "console levels route to stdout and stderr" *
                                      doctest::test_suite("console")) {
    run(ctx,
        "console.log('to-log')\n"
        "console.info('to-info')\n"
        "console.debug('to-debug')\n"
        "console.warn('to-warn')\n"
        "console.error('to-error')\n");
    std::string out = strip_ansi(g_stdout);
    std::string err = strip_ansi(g_stderr);
    CHECK(contains(out, "to-log\r\n"));
    CHECK(contains(out, "to-info"));
    CHECK(contains(out, "to-debug"));
    CHECK_FALSE(contains(out, "to-warn"));
    CHECK(contains(err, "to-warn"));
    CHECK(contains(err, "to-error\r\n"));
}

TEST_CASE_FIXTURE(CaptureFixture, "console format specifiers" * doctest::test_suite("console")) {
    run(ctx,
        "console.log('%s and %d', 'str', 42)\n"
        "console.log('%f', 1.5)\n"
        "console.log('%o', {k: 1})\n"
        "console.log('css:%cend', 'color: red')\n"
        "console.log('100%% sure')\n"
        "console.log('unknown %q stays')\n"
        "console.log('missing %s %s', 'one')\n"
        "console.log('trail %')\n"
        "console.log('fmt', 'extra', {deep: true})\n"
        "console.log(1, false, null)\n");
    std::string out = strip_ansi(g_stdout);
    CHECK(contains(out, "str and 42"));
    CHECK(contains(out, "1.500000"));
    CHECK(contains(out, "k: 1"));
    CHECK(contains(out, "css:end"));
    CHECK(contains(out, "100% sure"));
    CHECK(contains(out, "unknown %q stays"));
    CHECK(contains(out, "missing one %s"));
    CHECK(contains(out, "trail %"));
    CHECK(contains(out, "fmt 'extra'")); /* appended strings are inspected, so quoted */
    CHECK(contains(out, "deep: true"));
    CHECK(contains(out, "1, false, null")); /* non-string first arg: inspect-join */
}

TEST_CASE_FIXTURE(CaptureFixture, "console.error prints the whole error: fields, stack, cause" *
                                      doctest::test_suite("console")) {
    run(ctx,
        "const inner = Object.assign(new TypeError('inner'), {errno: 5})\n"
        "const outer = Object.assign(new Error('kaboom', {cause: inner}), {code: 7})\n"
        "console.error('context:', outer)\n"
        "console.warn(new TypeError('warned'))\n");
    std::string err = strip_ansi(g_stderr);
    CHECK(contains(err, "context: Error: kaboom { code: 7 }\n    at "));
    CHECK(contains(err, "\n  [cause]: TypeError: inner { errno: 5 }\n      at "));
    CHECK(contains(err, "TypeError: warned\n    at "));

    /* A Result error wrapped for context keeps its own cause chain */
    g_stderr.clear();
    run(ctx,
        "const failed = {name: 'ConnectFailed', message: 'auth failed', cause: {name: 'Dhcp'}}\n"
        "console.error('wrapped:', new Error('WiFi connect failed', {cause: failed}))\n");
    err = strip_ansi(g_stderr);
    CHECK(contains(err, "wrapped: Error: WiFi connect failed\n    at "));
    CHECK(contains(err, "\n  [cause]: { name: 'ConnectFailed', message: 'auth failed' }\n"
                        "    [cause]: { name: 'Dhcp' }"));
}

TEST_CASE_FIXTURE(CaptureFixture, "console.log renders an Error the same way as console.error" *
                                      doctest::test_suite("console")) {
    run(ctx,
        "const e = Object.assign(new Error('same'), {code: 1})\n"
        "console.log(e)\n"
        "console.error(e)\n");
    std::string out = strip_ansi(g_stdout);
    std::string err = strip_ansi(g_stderr);
    CHECK(contains(out, "Error: same { code: 1 }\n    at "));
    CHECK(out == err);
}

TEST_CASE_FIXTURE(CaptureFixture, "uncaught report includes fields and the cause chain" *
                                      doctest::test_suite("console")) {
    /* orPanic composes a PanicError whose cause is the Result error: the
     * cause line is what says what actually failed. */
    const char* src =
        "import {err} from 'mikro/result'\n"
        "err({name: 'ConnectFailed', message: 'timeout', errno: 110}).orPanic('wifi required')\n";
    JSValue rv = JS_Eval(ctx, src, strlen(src), "mikro/test-uncaught", JS_EVAL_TYPE_MODULE);
    REQUIRE(!JS_IsException(rv));
    REQUIRE(JS_PromiseState(ctx, rv) == JS_PROMISE_REJECTED);
    JSValue reason = JS_PromiseResult(ctx, rv);
    JS_FreeValue(ctx, rv);
    CHECK(mik__report_uncaught(ctx, reason, true));
    JS_FreeValue(ctx, reason);
    std::string err = strip_ansi(g_stderr);
    CHECK(contains(err, "Uncaught (in promise) PanicError: wifi required"));
    CHECK(contains(err, "\n  [cause]: ConnectFailed: timeout { errno: 110 }"));

    /* Nested Error causes keep their frames, indented under their parent;
     * a cyclic cause terminates. */
    g_stderr.clear();
    const char* src2 =
        "const root = Object.assign(new RangeError('root'), {code: 3})\n"
        "const mid = new Error('mid', {cause: root})\n"
        "root.cause = mid\n"
        "throw new Error('top', {cause: mid})\n";
    JSValue rv2 = JS_Eval(ctx, src2, strlen(src2), "mikro/test-uncaught2", JS_EVAL_TYPE_GLOBAL);
    REQUIRE(JS_IsException(rv2));
    JSValue exc = JS_GetException(ctx);
    CHECK(mik__report_uncaught(ctx, exc, false));
    JS_FreeValue(ctx, exc);
    err = strip_ansi(g_stderr);
    CHECK(contains(err, "Uncaught Error: top\n    at "));
    CHECK(contains(err, "\n  [cause]: Error: mid\n      at "));
    CHECK(contains(err, "\n    [cause]: RangeError: root { code: 3 }\n        at "));
    CHECK(contains(err, "\n      [cause]: [Circular]"));
}

TEST_CASE_FIXTURE(CaptureFixture, "inspect renders the value menagerie" *
                                      doctest::test_suite("console")) {
    run(ctx,
        "console.log({\n"
        "  n: 42, neg: -1.5, big: 123n,\n"
        "  s: \"it's got\\nnewlines\",\n"
        "  t: true, u: undefined, nil: null,\n"
        "  arr: [1, 2, 3],\n"
        "  m: new Map([['key', 1]]),\n"
        "  set: new Set([7, 8]),\n"
        "  d: new Date(86400000),\n"
        "  re: /ab+c/gi,\n"
        "  fn: function named() {},\n"
        "  arrow: () => {},\n"
        "  sym: Symbol('tag'),\n"
        "  u8: new Uint8Array([1, 2, 3]),\n"
        "  buf: new ArrayBuffer(4),\n"
        "})\n");
    std::string out = strip_ansi(g_stdout);
    CHECK(contains(out, "42"));
    CHECK(contains(out, "-1.5"));
    CHECK(contains(out, "123n"));
    CHECK(contains(out, "newlines"));
    CHECK(contains(out, "true"));
    CHECK(contains(out, "undefined"));
    CHECK(contains(out, "null"));
    CHECK(contains(out, "[ 1, 2, 3 ]"));
    CHECK(contains(out, "Map"));
    CHECK(contains(out, "Set"));
    CHECK(contains(out, "1970"));
    CHECK(contains(out, "/ab+c/gi"));
    CHECK(contains(out, "named"));
    CHECK(contains(out, "Symbol(tag)"));
    CHECK(contains(out, "Uint8Array"));
    CHECK(contains(out, "ArrayBuffer"));
}

TEST_CASE_FIXTURE(CaptureFixture, "inspect handles cycles, depth, and class instances" *
                                      doctest::test_suite("console")) {
    run(ctx,
        "const c = {name: 'loop'}\n"
        "c.self = c\n"
        "console.log(c)\n"
        "console.log({a: {b: {c: {d: 'buried'}}}})\n"
        "class Point { constructor() { this.x = 1 } }\n"
        "console.log(new Point())\n"
        "console.log([[1]])\n"); /* nested arrays within the depth limit */
    std::string out = strip_ansi(g_stdout);
    CHECK(contains(out, "[ [ 1 ] ]"));
    CHECK(contains(out, "loop"));
    CHECK(contains(out, "Circular"));
    CHECK_FALSE(contains(out, "buried")); /* depth 2 elides the innermost */
    CHECK(contains(out, "Point"));
    CHECK(contains(out, "x: 1"));
}

TEST_CASE_FIXTURE(CaptureFixture, "stdout.write is raw and supports bytes" *
                                      doctest::test_suite("stdio")) {
    run(ctx,
        "import {stdout} from 'mikro/stdio'\n"
        "stdout.write('raw-text')\n"
        "stdout.write(new Uint8Array([65, 66, 67]))\n"
        "stdout.flush()\n");
    /* write() adds no newline and no colors */
    CHECK(g_stdout == "raw-textABC");
}

TEST_CASE_FIXTURE(CaptureFixture, "stdin.read returns queued bytes then undefined" *
                                      doctest::test_suite("stdio")) {
    g_stdin_data = "hi";
    run(ctx,
        "import {stdin} from 'mikro/stdio'\n"
        "const first = stdin.read()\n"
        "globalThis.__first = String.fromCharCode(...first)\n"
        "globalThis.__empty = stdin.read() === undefined\n");
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue first = JS_GetPropertyStr(ctx, g, "__first");
    JSValue empty = JS_GetPropertyStr(ctx, g, "__empty");
    const char* s = JS_ToCString(ctx, first);
    CHECK(std::string(s ? s : "") == "hi");
    JS_FreeCString(ctx, s);
    CHECK(JS_ToBool(ctx, empty) == 1);
    JS_FreeValue(ctx, first);
    JS_FreeValue(ctx, empty);
    JS_FreeValue(ctx, g);
}

TEST_CASE_FIXTURE(CaptureFixture, "stdin listeners receive loop-dispatched data" *
                                      doctest::test_suite("stdio")) {
    run(ctx,
        "import {stdin} from 'mikro/stdio'\n"
        "globalThis.__got = ''\n"
        "const listener = (chunk) => { globalThis.__got += String.fromCharCode(...chunk) }\n"
        "stdin.addListener('data', listener)\n"
        "globalThis.__stop = () => stdin.removeListener('data', listener)\n");

    g_stdin_data = "ping";
    MIK_Loop(rt);
    run(ctx, "globalThis.__afterFirst = globalThis.__got\n");

    /* After removeListener the native handler is cleared: data stays queued. */
    run(ctx, "globalThis.__stop()\n");
    g_stdin_data = "dead";
    MIK_Loop(rt);

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue after = JS_GetPropertyStr(ctx, g, "__afterFirst");
    JSValue got = JS_GetPropertyStr(ctx, g, "__got");
    const char* a = JS_ToCString(ctx, after);
    const char* t = JS_ToCString(ctx, got);
    CHECK(std::string(a ? a : "") == "ping");
    CHECK(std::string(t ? t : "") == "ping");
    JS_FreeCString(ctx, a);
    JS_FreeCString(ctx, t);
    JS_FreeValue(ctx, after);
    JS_FreeValue(ctx, got);
    JS_FreeValue(ctx, g);
}

TEST_CASE_FIXTURE(CaptureFixture, "native stdin.setHandler rejects non-functions" *
                                      doctest::test_suite("stdio")) {
    run(ctx,
        "import * as native from 'native:mikro/stdio'\n"
        "let threw = false\n"
        "try { native.stdin.setHandler(42) } catch (e) { threw = e instanceof TypeError }\n"
        "globalThis.__threw = threw\n"
        /* setHandler twice: the second replaces (frees) the first */
        "native.stdin.setHandler(() => {})\n"
        "native.stdin.setHandler(() => {})\n"
        "native.stdin.clearHandler()\n"
        "native.stdin.clearHandler()\n" /* idempotent */);
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue threw = JS_GetPropertyStr(ctx, g, "__threw");
    CHECK(JS_ToBool(ctx, threw) == 1);
    JS_FreeValue(ctx, threw);
    JS_FreeValue(ctx, g);
}
