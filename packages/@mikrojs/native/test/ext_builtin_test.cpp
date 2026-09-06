#include <cstring>
#include <string>
#include <vector>

#include <mikrojs/mikrojs.h>
#include <mikrojs/private.h>
#include <quickjs.h>

#include <doctest.h>

/* External builtins (MIK_REGISTER_BUILTIN) are how board/driver packages embed
 * their JS wrappers as firmware bytecode, resolved by npm package name. These
 * tests pin the loader paths for packages under a third-party scope (outside
 * the @mikrojs scope): the native: import gate must trust registered builtins, and a
 * builtin that fails to load must propagate its real error instead of falling
 * through to filesystem resolution. */

namespace {

/* Registers a bytecode builtin under `name` for the duration of a test,
 * mirroring what MIK_REGISTER_BUILTIN() does in a driver package. The entry
 * is linked into the registry before compilation so the native: import gate
 * sees the name as a firmware builtin (on device, registration happens at
 * static-constructor time, long before any bytecode loads). */
struct ExtBuiltin {
    mik_ext_builtin_t entry;
    std::vector<uint8_t> bytecode;

    explicit ExtBuiltin(const char* name) : entry{name, nullptr, 0, mik__ext_builtin_head} {
        mik__ext_builtin_head = &entry;
    }
    ~ExtBuiltin() { mik__ext_builtin_head = entry.next; }

    /* Compile `source` to bytecode as a module named `entry.name`, in a
     * scratch runtime so the module doesn't pre-exist in the runtime under
     * test. JS_Eval resolves imports even with COMPILE_ONLY, so native:
     * modules unavailable on host can be satisfied with a virtual stub. */
    void compile(const char* source, const char* stub_name = nullptr,
                 const char* stub_source = nullptr) {
        MIKRuntime* scratch = MIK_NewRuntime();
        JSContext* ctx = MIK_GetJSContext(scratch);
        if (stub_name != nullptr) {
            MIK_RegisterVirtualModule(scratch, stub_name, stub_source, strlen(stub_source));
        }
        JSValue mod = JS_Eval(ctx, source, strlen(source), entry.name,
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        REQUIRE(!JS_IsException(mod));
        size_t size = 0;
        uint8_t* buf = JS_WriteObject(ctx, &size, mod, JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(ctx, mod);
        REQUIRE(buf != nullptr);
        bytecode.assign(buf, buf + size);
        js_free(ctx, buf);
        MIK_FreeRuntime(scratch);
        entry.data = bytecode.data();
        entry.data_size = (uint32_t)bytecode.size();
    }
};

std::string pending_exception_message(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, exc);
    std::string msg = s != nullptr ? s : "";
    if (s != nullptr) {
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, exc);
    return msg;
}

}  // namespace

TEST_CASE("ext builtin under a third-party scope can import native: modules" *
          doctest::test_suite("modules")) {
    ExtBuiltin driver("@acme/driver-x/x");
    driver.compile("import {memoryUsage} from 'native:mikro/sys'\n"
                   "export function ping() { return typeof memoryUsage === 'function' ? 42 : -1 }\n");

    auto* rt = MIK_NewRuntime();
    auto* ctx = MIK_GetJSContext(rt);

    const char* code = "import {ping} from '@acme/driver-x/x'\n"
                       "globalThis.__ping = ping()\n";
    JSValue ret = MIK_EvalModuleContent(ctx, "/app/main.js", code, strlen(code));
    if (JS_IsException(ret)) {
        FAIL_CHECK("eval threw: " << pending_exception_message(ctx));
    }
    JS_FreeValue(ctx, ret);
    MIK_Loop(rt);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ping = JS_GetPropertyStr(ctx, global, "__ping");
    int32_t value = 0;
    JS_ToInt32(ctx, &value, ping);
    CHECK_EQ(42, value);
    JS_FreeValue(ctx, ping);
    JS_FreeValue(ctx, global);
    MIK_FreeRuntime(rt);
}

TEST_CASE("app code still cannot import native: modules directly" *
          doctest::test_suite("modules")) {
    auto* rt = MIK_NewRuntime();
    auto* ctx = MIK_GetJSContext(rt);

    const char* code = "import * as sys from 'native:mikro/sys'\n";
    JSValue ret = MIK_EvalModuleContent(ctx, "/app/main.js", code, strlen(code));
    CHECK(JS_IsException(ret));
    JS_FreeValue(ctx, ret);
    std::string msg = pending_exception_message(ctx);
    CHECK_MESSAGE(msg.find("can only be imported by firmware builtins") != std::string::npos,
                  "unexpected error: " << msg);
    MIK_FreeRuntime(rt);
}

TEST_CASE("ext builtin load failure propagates instead of falling through to fs" *
          doctest::test_suite("modules")) {
    /* The builtin compiles against a virtual native:mikro/wifi stub, but the
     * runtime under test has no native:mikro/wifi (host build, no stub). Loading
     * must surface the real failure naming 'native:mikro/wifi', not a
     * resolution failure for the package name. */
    ExtBuiltin driver("@acme/bad-driver/bad");
    driver.compile("import {Wifi} from 'native:mikro/wifi'\n"
                   "export function f() { return new Wifi() }\n",
                   "native:mikro/wifi", "export class Wifi {}\n");

    auto* rt = MIK_NewRuntime();
    auto* ctx = MIK_GetJSContext(rt);

    const char* code = "import {f} from '@acme/bad-driver/bad'\n";
    JSValue ret = MIK_EvalModuleContent(ctx, "/app/main.js", code, strlen(code));
    CHECK(JS_IsException(ret));
    JS_FreeValue(ctx, ret);
    std::string msg = pending_exception_message(ctx);
    CHECK_MESSAGE(msg.find("native:mikro/wifi") != std::string::npos, "unexpected error: " << msg);
    CHECK_MESSAGE(msg.find("@acme/bad-driver/bad") == std::string::npos,
                  "real error was masked: " << msg);
    MIK_FreeRuntime(rt);
}

TEST_CASE("a driver may import another package's native: modules" *
          doctest::test_suite("modules")) {
    /* Native-level composition across packages is allowed: an @acme driver may
     * import @other's native: binding directly (e.g. an audio driver reusing a
     * codec package's primitives). There is no such module on the host build, so
     * it fails to resolve at load — proving the gate let it through rather
     * than rejecting the cross-package import. */
    ExtBuiltin driver("@acme/audio/audio");
    driver.compile("import 'native:@other/codec/decode'\n"
                   "export function f() {}\n",
                   "native:@other/codec/decode", "export default {}\n");

    auto* rt = MIK_NewRuntime();
    auto* ctx = MIK_GetJSContext(rt);
    const char* code = "import {f} from '@acme/audio/audio'\n";
    JSValue ret = MIK_EvalModuleContent(ctx, "/app/main.js", code, strlen(code));
    CHECK(JS_IsException(ret));
    JS_FreeValue(ctx, ret);
    std::string msg = pending_exception_message(ctx);
    CHECK_MESSAGE(msg.find("Failed to resolve module specifier 'native:@other/codec/decode'") !=
                      std::string::npos,
                  "unexpected error: " << msg);
    CHECK_MESSAGE(msg.find("can only be imported by firmware builtins") == std::string::npos,
                  "cross-package native: import was wrongly rejected: " << msg);
    MIK_FreeRuntime(rt);
}

TEST_CASE("Builtin whose bytecode is not a module fails cleanly" *
          doctest::test_suite("ext_builtin")) {
    /* A registry entry pointing at serialized plain data (not module
     * bytecode) must fail the tag check, not crash or half-load. */
    ExtBuiltin bad("@acme/notmodule/x");
    {
        MIKRuntime* scratch = MIK_NewRuntime();
        JSContext* sctx = MIK_GetJSContext(scratch);
        JSValue val = JS_ParseJSON(sctx, "{\"a\":1}", 7, "<v>");
        REQUIRE(!JS_IsException(val));
        size_t size = 0;
        uint8_t* buf = JS_WriteObject(sctx, &size, val, 0);
        JS_FreeValue(sctx, val);
        REQUIRE(buf != nullptr);
        bad.bytecode.assign(buf, buf + size);
        js_free(sctx, buf);
        MIK_FreeRuntime(scratch);
        bad.entry.data = bad.bytecode.data();
        bad.entry.data_size = (uint32_t)bad.bytecode.size();
    }

    auto* rt = MIK_NewRuntime();
    auto* ctx = MIK_GetJSContext(rt);
    const char* code = "import '@acme/notmodule/x'\n";
    JSValue ret = MIK_EvalModuleContent(ctx, "/app/main.js", code, strlen(code));
    CHECK(JS_IsException(ret));
    JS_FreeValue(ctx, ret);
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
    MIK_FreeRuntime(rt);
}

TEST_CASE("Builtin importing a missing export fails at resolve" *
          doctest::test_suite("ext_builtin")) {
    /* The dependency deserializes fine; binding resolution then fails on
     * the nonexistent export name. The real resolve error must propagate. */
    ExtBuiltin bad("@acme/badbind/x");
    bad.compile("import {definitely_not_exported} from 'mikro/result'\n"
                "export const y = definitely_not_exported\n");

    auto* rt = MIK_NewRuntime();
    auto* ctx = MIK_GetJSContext(rt);
    const char* code = "import '@acme/badbind/x'\n";
    JSValue ret = MIK_EvalModuleContent(ctx, "/app/main.js", code, strlen(code));
    CHECK(JS_IsException(ret));
    JS_FreeValue(ctx, ret);
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
    MIK_FreeRuntime(rt);
}
