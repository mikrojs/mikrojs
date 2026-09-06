#include <cstdio>
#include <cstring>
#include <string>

#include <mikrojs/mikrojs.h>
#include <mikrojs/private.h>
#include <quickjs.h>

#include <doctest.h>

/* mik_inspect tests:
 * - direct mik_inspect() calls for symbol rendering and the guarantee that
 *   inspect never leaves a pending exception behind (throwing getters,
 *   proxy traps, throwing type probes)
 * - the mikro/inspect builtin for rendering depth: quoted keys, showHidden,
 *   class tags, typed arrays, custom hooks, and large collections */

namespace {

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

/* ── Direct mik_inspect() fixture ────────────────────────────────── */

struct DirectFixture {
    MIKRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    DirectFixture() {
        rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        ctx = MIK_GetJSContext(rt);
    }

    ~DirectFixture() { MIK_FreeRuntime(rt); }

    /* Eval `code` (must not throw) and inspect the result. */
    std::string inspect_eval(const char* code) {
        JSValue v = JS_Eval(ctx, code, strlen(code), "main.js", JS_EVAL_TYPE_GLOBAL);
        REQUIRE_MESSAGE(!JS_IsException(v), "test eval should not throw");
        std::string out = mik_inspect(ctx, v, 2, false, false);
        JS_FreeValue(ctx, v);
        return out;
    }
};

/* ── mikro/inspect builtin fixture ───────────────────────────────── */

struct BuiltinFixture {
    MIKRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    BuiltinFixture() {
        rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        ctx = MIK_GetJSContext(rt);
    }

    ~BuiltinFixture() { MIK_FreeRuntime(rt); }

    void run(const char* body) {
        std::string code = "import {inspect} from 'mikro/inspect'\n" + std::string(body);
        JSValue rv = JS_Eval(ctx, code.c_str(), code.size(), "mikro/test-inspect-driver",
                             JS_EVAL_TYPE_MODULE);
        REQUIRE(!JS_IsException(rv));
        JSPromiseStateEnum state = JS_PromiseState(ctx, rv);
        if (state == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, rv);
            const char* s = JS_ToCString(ctx, reason);
            if (s) {
                fprintf(stderr, "[inspect run] rejected: %s\n", s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, reason);
        }
        JS_FreeValue(ctx, rv);
        REQUIRE(state == JS_PROMISE_FULFILLED);
    }

    std::string global_str(const char* name) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue v = JS_GetPropertyStr(ctx, g, name);
        JS_FreeValue(ctx, g);
        const char* s = JS_ToCString(ctx, v);
        std::string out = s ? s : "";
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        return out;
    }
};

}  // namespace

/* ── Symbols ─────────────────────────────────────────────────────── */

TEST_CASE_FIXTURE(DirectFixture, "inspect renders symbol descriptions" *
                                     doctest::test_suite("inspect")) {
    CHECK(inspect_eval("Symbol('tag')") == "Symbol(tag)");
    CHECK(inspect_eval("Symbol()") == "Symbol()");
    CHECK(contains(inspect_eval("({key: Symbol('nested')})"), "key: Symbol(nested)"));
    CHECK(contains(inspect_eval("new Map([[Symbol('mk'), 1]])"), "Symbol(mk) => 1"));
    /* failed ToString must not leave a pending TypeError */
    CHECK_FALSE(JS_HasException(ctx));
}

/* ── Throwing getters and traps ──────────────────────────────────── */

TEST_CASE_FIXTURE(DirectFixture, "throwing getter renders placeholder, other keys intact" *
                                     doctest::test_suite("inspect")) {
    std::string out =
        inspect_eval("({ok: 1, get boom() { throw new Error('nope') }, also: 'yes'})");
    CHECK(contains(out, "boom: [getter threw]"));
    CHECK(contains(out, "ok: 1"));
    CHECK(contains(out, "also: 'yes'"));
    CHECK_FALSE(JS_HasException(ctx));
}

TEST_CASE_FIXTURE(DirectFixture, "throwing type probes leave no pending exception" *
                                     doctest::test_suite("inspect")) {
    /* Symbol.toStringTag getter */
    CHECK(contains(
        inspect_eval("({a: 1, get [Symbol.toStringTag]() { throw new Error('tag') }})"),
        "a: 1"));
    CHECK_FALSE(JS_HasException(ctx));
    /* custom inspect hook that throws falls back to normal rendering */
    CHECK(contains(
        inspect_eval("({x: 42, [Symbol.for('mikrojs.inspect')]() { throw new Error('hook') }})"),
        "x: 42"));
    CHECK_FALSE(JS_HasException(ctx));
    /* fake collection/date/error/typed-array shapes with throwing internals */
    inspect_eval("({[Symbol.toStringTag]: 'Map', get size() { throw new Error('size') }})");
    CHECK_FALSE(JS_HasException(ctx));
    inspect_eval("({[Symbol.toStringTag]: 'Date', toJSON() { throw new Error('json') }})");
    CHECK_FALSE(JS_HasException(ctx));
    inspect_eval(
        "({[Symbol.toStringTag]: 'Error', get name() { throw new Error('n') },"
        "  get message() { throw new Error('m') }})");
    CHECK_FALSE(JS_HasException(ctx));
    inspect_eval("({[Symbol.toStringTag]: 'Uint8Array'})");
    CHECK_FALSE(JS_HasException(ctx));
}

TEST_CASE_FIXTURE(DirectFixture, "throwing proxy traps leave no pending exception" *
                                     doctest::test_suite("inspect")) {
    std::string out = inspect_eval("new Proxy({a: 1}, {get() { throw new Error('trap') }})");
    CHECK(contains(out, "a: [getter threw]"));
    CHECK_FALSE(JS_HasException(ctx));
    inspect_eval("new Proxy({a: 1}, {ownKeys() { throw new Error('keys') }})");
    CHECK_FALSE(JS_HasException(ctx));
    inspect_eval("new Proxy([1, 2], {get() { throw new Error('len') }})");
    CHECK_FALSE(JS_HasException(ctx));
}

TEST_CASE_FIXTURE(DirectFixture, "nested throwing getter renders placeholder" *
                                     doctest::test_suite("inspect")) {
    std::string out =
        inspect_eval("({outer: {get inner() { throw new Error('deep') }}, tail: true})");
    CHECK(contains(out, "inner: [getter threw]"));
    CHECK(contains(out, "tail: true"));
    CHECK_FALSE(JS_HasException(ctx));
}

/* ── Rendering depth via the mikro/inspect builtin ───────────────── */

TEST_CASE_FIXTURE(BuiltinFixture, "keys, hidden props, tags, and scalar edge cases" *
                                      doctest::test_suite("inspect")) {
    run("const show = (label, v, opts) => { globalThis['__' + label] = inspect(v, opts) }\n"
        "show('quoted', {'not simple': 1, 'has\\'quote': 2, valid_key: 3})\n"
        "const hidden = Object.defineProperty({a: 1}, 'secret', {value: 42})\n"
        "show('hiddenOff', hidden)\n"
        "show('hiddenOn', hidden, {showHidden: true})\n"
        "class Tagged { get [Symbol.toStringTag]() { return 'MyTag' } }\n"
        "show('tagged', new Tagged())\n"
        "show('sparse', [1, , 3])\n"
        "show('negzero', -0)\n"
        "show('promise', Promise.resolve(1))\n"
        "show('proxy', new Proxy({}, {}))\n");
    CHECK(global_str("__quoted") == "{ 'not simple': 1, 'has\\'quote': 2, valid_key: 3 }");
    CHECK(global_str("__hiddenOff") == "{ a: 1 }");
    CHECK(global_str("__hiddenOn") == "{ a: 1, secret: 42 }");
    CHECK(global_str("__tagged") == "MyTag{}");
    CHECK(global_str("__sparse") == "[ 1, undefined, 3 ]");
    CHECK(global_str("__negzero") == "-0");
    CHECK(contains(global_str("__promise"), "Promise"));
    CHECK(global_str("__proxy") == "{}");
}

TEST_CASE_FIXTURE(BuiltinFixture, "typed arrays, DataView, getters, and custom hooks" *
                                      doctest::test_suite("inspect")) {
    run("const show = (label, v, opts) => { globalThis['__' + label] = inspect(v, opts) }\n"
        "show('i16', new Int16Array([1, -2]))\n"
        "show('f32', new Float32Array([0.5]))\n"
        "show('dv', new DataView(new ArrayBuffer(4)))\n"
        "show('getter', {get lazy() { return 7 }})\n"
        "show('custom', {[Symbol.for('mikrojs.inspect')](depth, ins) {\n"
        "  return 'custom<' + ins({inner: 1}, depth) + '>'\n"
        "}})\n");
    CHECK(global_str("__i16") == "Int16Array[ 1, -2 ]");
    CHECK(global_str("__f32") == "Float32Array[ 0.5 ]");
    CHECK(global_str("__dv") == "DataView{}");
    CHECK(global_str("__getter") == "{ lazy: 7 }");
    CHECK(global_str("__custom") == "custom<{ inner: 1 }>");
}

TEST_CASE_FIXTURE(BuiltinFixture, "large collections render in full" *
                                      doctest::test_suite("inspect")) {
    run("const show = (label, v) => { globalThis['__' + label] = inspect(v) }\n"
        "show('bigarr', Array.from({length: 150}, (_, i) => i))\n"
        "show('bigmap', new Map(Array.from({length: 120}, (_, i) => [i, i])))\n"
        "show('bigset', new Set(Array.from({length: 120}, (_, i) => i)))\n"
        "globalThis.__strlen = String(inspect('x'.repeat(500)).length)\n");
    CHECK(contains(global_str("__bigarr"), ", 149 ]"));
    CHECK(contains(global_str("__bigmap"), "119 => 119"));
    CHECK(contains(global_str("__bigset"), ", 119 }"));
    CHECK(global_str("__strlen") == "502"); /* 500 chars + surrounding quotes */
}

TEST_CASE_FIXTURE(BuiltinFixture, "the builtin path drains getter exceptions too" *
                                      doctest::test_suite("inspect")) {
    run("globalThis.__thrower = inspect({get boom() { throw new Error('nope') }})\n");
    CHECK_FALSE(JS_HasException(ctx));
    CHECK(contains(global_str("__thrower"), "boom: [getter threw]"));
}

/* ── Errors: fields, stack, cause chain ──────────────────────────── */

TEST_CASE_FIXTURE(DirectFixture, "inspect renders an Error with fields, stack and cause chain" *
                                     doctest::test_suite("inspect")) {
    std::string out = inspect_eval(
        "const root = Object.assign(new TypeError('root'), {errno: 2})\n"
        "Object.assign(new Error('top', {cause: root}), {code: 7, path: '/x'})");
    CHECK(contains(out, "Error: top { code: 7, path: '/x' }\n    at "));
    CHECK(contains(out, "\n  [cause]: TypeError: root { errno: 2 }\n      at "));

    /* A plain-object cause (a Result error) inspects as an object even past
     * the depth limit, since the cause is the point of the log line */
    out = inspect_eval("new Error('panic', {cause: {name: 'ConnectFailed', message: 'timeout'}})");
    CHECK(contains(out, "\n  [cause]: { name: 'ConnectFailed', message: 'timeout' }"));

    /* Cyclic causes terminate */
    out = inspect_eval("const e = new Error('loop'); e.cause = e; e");
    CHECK(contains(out, "Error: loop\n    at "));
    CHECK(contains(out, "\n  [cause]: Error: loop [Circular]"));

    /* No stack property: header only, no trailing newline */
    out = inspect_eval("({[Symbol.toStringTag]: 'Error', name: 'Fake', message: 'no frames'})");
    CHECK(out == "Fake: no frames");
}

TEST_CASE_FIXTURE(DirectFixture, "inspect chains the cause of a plain error-like object" *
                                     doctest::test_suite("inspect")) {
    /* Result errors are plain objects; a cause field chains the same way
     * as an Error's, past the depth limit, instead of hitting [Object] */
    std::string out = inspect_eval(
        "({name: 'ConnectFailed', message: 'auth failed',"
        "  cause: {name: 'Dhcp', message: 'no lease', cause: {name: 'Deep'}}})");
    CHECK(out == "{ name: 'ConnectFailed', message: 'auth failed' }\n"
                 "  [cause]: { name: 'Dhcp', message: 'no lease' }\n"
                 "    [cause]: { name: 'Deep' }");

    /* An Error inside a plain error keeps its frames, indented */
    out = inspect_eval(
        "({name: 'ConnectFailed', cause: Object.assign(new Error('reset'), {errno: 104})})");
    CHECK(contains(out, "{ name: 'ConnectFailed' }\n  [cause]: Error: reset { errno: 104 }\n      at "));

    /* Only a cause field; cycles terminate */
    CHECK(inspect_eval("({cause: 'user'})") == "{}\n  [cause]: 'user'");
    out = inspect_eval("const o = {name: 'Loop'}; o.cause = o; o");
    CHECK(out == "{ name: 'Loop' }\n  [cause]: [Circular]");
}

TEST_CASE_FIXTURE(DirectFixture, "inspect nests a multi-line Error under its container" *
                                     doctest::test_suite("inspect")) {
    std::string out = inspect_eval("({attempt: 3, err: new Error('boom')})");
    CHECK(contains(out, "{ attempt: 3, err: Error: boom\n      at "));
    out = inspect_eval("[new Error('boom')]");
    CHECK(contains(out, "[ Error: boom\n      at "));
}

TEST_CASE_FIXTURE(DirectFixture, "inspect caps the cause chain like the uncaught reporter" *
                                     doctest::test_suite("inspect")) {
    std::string out = inspect_eval(
        "let e = {name: 'c6'}\n"
        "for (const n of ['c5', 'c4', 'c3', 'c2', 'c1', 'top']) e = {name: n, cause: e}\n"
        "e");
    CHECK(contains(out, "[cause]: { name: 'c4' }"));
    CHECK(contains(out, "[cause]: \xe2\x80\xa6"));
    CHECK_FALSE(contains(out, "c5"));
}
