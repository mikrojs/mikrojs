#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <mikrojs/mikrojs.h>
#include <mikrojs/platform.h>
#include <mikrojs/private.h>
#include <quickjs.h>

#include <doctest.h>

/* Host-side tests for the runtime entry APIs in mikrojs.cpp: MIK_RunEntry /
 * MIK_RunEntryErr (incl. the .bjs fallback and error returns), MIK_EvalScript,
 * memory profiling, module data slots, config, and the error / test-emit
 * handlers. */

namespace {

static int rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return remove(path);
}

struct EntryFixture {
    char root[64];
    MIKRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    EntryFixture() {
        snprintf(root, sizeof(root), "/tmp/mik_entry_test_XXXXXX");
        REQUIRE(mkdtemp(root) != nullptr);
        rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        MIK_SetFSBasePath(rt, root);
        ctx = MIK_GetJSContext(rt);
    }

    ~EntryFixture() {
        MIK_FreeRuntime(rt);
        nftw(root, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
    }

    void write(const char* rel, const void* data, size_t len) const {
        std::string path = std::string(root) + rel;
        for (size_t i = strlen(root) + 1; i < path.size(); i++) {
            if (path[i] == '/') {
                path[i] = '\0';
                mkdir(path.c_str(), 0755);
                path[i] = '/';
            }
        }
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(fwrite(data, 1, len, f) == len);
        fclose(f);
    }

    void write(const char* rel, const char* text) const { write(rel, text, strlen(text)); }
};

static std::string read_global_string(JSContext* ctx, const char* name) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
}

static std::string serialize_module(const char* src, const char* name) {
    MIKRuntime* scratch = MIK_NewRuntime();
    REQUIRE(scratch != nullptr);
    JSContext* sctx = MIK_GetJSContext(scratch);
    JSValue obj = JS_Eval(sctx, src, strlen(src), name,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    REQUIRE(!JS_IsException(obj));
    size_t len = 0;
    uint8_t* buf = JS_WriteObject(sctx, &len, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(sctx, obj);
    REQUIRE(buf != nullptr);
    std::string out(reinterpret_cast<char*>(buf), len);
    js_free(sctx, buf);
    MIK_FreeRuntime(scratch);
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(EntryFixture, "RunEntry evaluates an entry module and its imports" *
                                    doctest::test_suite("entry")) {
    write("/app/lib.js", "export const x = 5\n");
    write("/app/entry.js",
          "import {x} from './lib.js'\n"
          "globalThis.__ran = String(x)\n"
          "globalThis.__isMain = String(import.meta.main)\n");
    char err[256];
    CHECK(MIK_RunEntryErr(rt, "/app/entry.js", err, sizeof(err)) == 0);
    CHECK(err[0] == '\0');
    CHECK(read_global_string(ctx, "__ran") == "5");
    CHECK(read_global_string(ctx, "__isMain") == "true");
}

TEST_CASE_FIXTURE(EntryFixture, "RunEntry falls back to a .bjs next to the .js name" *
                                    doctest::test_suite("entry")) {
    std::string bc = serialize_module("globalThis.__bjs = 'ran'\nexport const ok = 1\n",
                                      "/app/compiled.js");
    write("/app/compiled.bjs", bc.data(), bc.size());
    CHECK(MIK_RunEntry(rt, "/app/compiled.js") == 0);
    CHECK(read_global_string(ctx, "__bjs") == "ran");
}

TEST_CASE_FIXTURE(EntryFixture, "RunEntry surfaces .bjs errors and profiles bytecode entries" *
                                    doctest::test_suite("entry")) {
    /* garbage bytecode via the .js → .bjs fallback and as a direct entry */
    const char garbage[] = {'\xba', '\xad', '\xf0', '\x0d'};
    write("/app/gb.bjs", garbage, sizeof(garbage));
    char err[256];
    CHECK(MIK_RunEntryErr(rt, "/app/gb.js", err, sizeof(err)) == -EFAULT);
    CHECK(err[0] != '\0');
    CHECK(MIK_RunEntryErr(rt, "/app/gb.bjs", err, sizeof(err)) == -EFAULT);

    /* a .bjs importing a missing binding fails at resolve */
    std::string badbind = serialize_module(
        "import {definitely_not_exported} from 'mikro/result'\n"
        "export const y = definitely_not_exported\n",
        "/app/bb.bjs");
    write("/app/bb.bjs", badbind.data(), badbind.size());
    CHECK(MIK_RunEntryErr(rt, "/app/bb.bjs", err, sizeof(err)) == -EFAULT);

    /* profiling records the entry itself on the bytecode path */
    std::string good = serialize_module("globalThis.__pbjs = 'ran'\nexport const ok = 1\n",
                                        "/app/prof.js");
    write("/app/prof.bjs", good.data(), good.size());
    MIK_EnableProfiling(rt);
    CHECK(MIK_RunEntry(rt, "/app/prof.js") == 0);
    CHECK(read_global_string(ctx, "__pbjs") == "ran");
    bool saw = false;
    const MIKProfileEntry* entries = MIK_GetProfileEntries(rt);
    for (size_t i = 0; i < MIK_GetProfileEntryCount(rt); i++) {
        if (strcmp(entries[i].name, "/app/prof.js") == 0) saw = true;
    }
    CHECK(saw);
}

TEST_CASE_FIXTURE(EntryFixture, "RunEntry error returns" * doctest::test_suite("entry")) {
    CHECK(MIK_RunEntry(rt, nullptr) == -EINVAL);
    CHECK(MIK_RunEntry(rt, "") == -EINVAL);
    CHECK(MIK_RunEntry(rt, "/app/missing.js") == -ENOENT);

    write("/app/throws.js", "throw new Error('entry boom')\n");
    char err[256];
    CHECK(MIK_RunEntryErr(rt, "/app/throws.js", err, sizeof(err)) == -EFAULT);
    CHECK(strstr(err, "entry boom") != nullptr);

    write("/app/broken.js", "export const = nope\n");
    CHECK(MIK_RunEntryErr(rt, "/app/broken.js", err, sizeof(err)) == -EFAULT);
    CHECK(strstr(err, "SyntaxError") != nullptr);
}

TEST_CASE_FIXTURE(EntryFixture, "EvalScript runs a plain script file" *
                                    doctest::test_suite("entry")) {
    write("/script.js", "globalThis.__script = 'global-ran'\n");
    std::string os_path = std::string(root) + "/script.js";
    JSValue rv = MIK_EvalScript(ctx, os_path.c_str());
    CHECK_FALSE(JS_IsException(rv));
    JS_FreeValue(ctx, rv);
    CHECK(read_global_string(ctx, "__script") == "global-ran");

    JSValue missing = MIK_EvalScript(ctx, "/definitely/not/here.js");
    CHECK(JS_IsException(missing));
    JSValue exc = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, exc);
    CHECK(strstr(s ? s : "", "could not load") != nullptr);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, exc);

    JSValue rv2 = MIK_EvalScriptContent(ctx, "globalThis.__inline = 'yes'", 27);
    CHECK_FALSE(JS_IsException(rv2));
    JS_FreeValue(ctx, rv2);
    CHECK(read_global_string(ctx, "__inline") == "yes");
}

TEST_CASE_FIXTURE(EntryFixture, "profiling records per-module load costs" *
                                    doctest::test_suite("entry")) {
    write("/app/heavy.js", "export const blob = new Array(200).fill('x')\n");
    write("/app/entry.js",
          "import {blob} from './heavy.js'\n"
          "globalThis.__n = String(blob.length)\n");
    MIK_EnableProfiling(rt);
    CHECK(MIK_RunEntry(rt, "/app/entry.js") == 0);
    CHECK(read_global_string(ctx, "__n") == "200");

    size_t count = MIK_GetProfileEntryCount(rt);
    REQUIRE(count >= 2); /* the import plus the entry itself */
    const MIKProfileEntry* entries = MIK_GetProfileEntries(rt);
    REQUIRE(entries != nullptr);
    bool saw_entry = false, saw_heavy = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, "/app/entry.js") == 0) saw_entry = true;
        if (strcmp(entries[i].name, "/app/heavy.js") == 0) saw_heavy = true;
    }
    CHECK(saw_entry);
    CHECK(saw_heavy);
    CHECK(MIK_GetProfileBaseline(rt) > 0);
}

TEST_CASE_FIXTURE(EntryFixture, "module data slots store per-module pointers" *
                                    doctest::test_suite("entry")) {
    int slot_a = MIK_ReserveModuleSlot();
    int slot_b = MIK_ReserveModuleSlot();
    REQUIRE(slot_a >= 0);
    REQUIRE(slot_b >= 0);
    CHECK(slot_a != slot_b);
    CHECK(MIK_GetModuleData(rt, slot_a) == nullptr);
    int value_a = 41, value_b = 42;
    MIK_SetModuleData(rt, slot_a, &value_a);
    MIK_SetModuleData(rt, slot_b, &value_b);
    CHECK(MIK_GetModuleData(rt, slot_a) == &value_a);
    CHECK(MIK_GetModuleData(rt, slot_b) == &value_b);
}

TEST_CASE("a reserved module slot means the same module in every runtime" *
          doctest::test_suite("entry")) {
    /* Modules cache their slot index and reuse it for the life of the process.
     * If reservation restarted per runtime, the second runtime would hand this
     * index to the next module that asked, and the two would read each other's
     * state through the wrong type. */
    MIKRuntime* first = MIK_NewRuntime();
    int slot = MIK_ReserveModuleSlot();
    int marker = 1;
    MIK_SetModuleData(first, slot, &marker);
    CHECK(MIK_GetModuleData(first, slot) == &marker);
    MIK_FreeRuntime(first);

    /* A different module comes up first in the next runtime. */
    MIKRuntime* second = MIK_NewRuntime();
    int later = MIK_ReserveModuleSlot();
    CHECK(later != slot);
    CHECK(MIK_GetModuleData(second, slot) == nullptr);
    MIK_FreeRuntime(second);
}

TEST_CASE_FIXTURE(EntryFixture, "SetConfig applies a tweaked default config" *
                                    doctest::test_suite("entry")) {
    MIKConfig config;
    MIK_DefaultConfig(&config);
    config.panic_restart_delay_ms = 1234;
    MIK_SetConfig(rt, &config);
    /* no getter; the call itself must succeed and not disturb the runtime */
    JSValue rv = MIK_EvalScriptContent(ctx, "globalThis.__cfg = 'alive'", 26);
    CHECK_FALSE(JS_IsException(rv));
    JS_FreeValue(ctx, rv);
    CHECK(read_global_string(ctx, "__cfg") == "alive");
}

namespace {

static std::string g_error_seen;
static void capture_error(JSContext* ctx, JSValue error, void* opaque) {
    (void)opaque;
    const char* s = JS_ToCString(ctx, error);
    if (s) {
        g_error_seen = s;
        JS_FreeCString(ctx, s);
    }
}

static std::string g_emit_seen;
static void capture_emit(const char* json, size_t len, void* opaque) {
    (void)opaque;
    g_emit_seen.assign(json, len);
}

}  // namespace

TEST_CASE_FIXTURE(EntryFixture, "the error handler sees unhandled rejections" *
                                    doctest::test_suite("entry")) {
    g_error_seen.clear();
    MIK_SetErrorHandler(rt, capture_error, nullptr);
    JSValue rv = MIK_EvalScriptContent(
        ctx, "Promise.reject(new Error('nobody-caught-me'))",
        (size_t)strlen("Promise.reject(new Error('nobody-caught-me'))"));
    JS_FreeValue(ctx, rv);
    MIK_Loop(rt); /* flushes the unhandled-rejection queue */
    CHECK(g_error_seen.find("nobody-caught-me") != std::string::npos);
}

/* A missing import fails inside JS_Eval's import resolution, before the
 * module body runs, so it is a plain pending exception rather than a rejected
 * eval promise. The firmware used to capture that exception into a buffer only
 * the OTA trial read, and the device idled in silence: no error line, no
 * restart. The entry API has to report it itself. */
TEST_CASE_FIXTURE(EntryFixture, "an entry whose import is missing is reported and stops" *
                                    doctest::test_suite("entry")) {
    g_error_seen.clear();
    MIK_SetErrorHandler(rt, capture_error, nullptr);
    write("/app/main.js",
          "import {board} from 'mikro/sys/foo'\n"
          "globalThis.__ran = 'yes'\n");
    char err[256];
    CHECK(MIK_RunEntryErr(rt, "/app/main.js", err, sizeof(err)) == -EFAULT);
    CHECK(strstr(err, "mikro/sys/foo") != nullptr);
    CHECK(g_error_seen.find("mikro/sys/foo") != std::string::npos);
    CHECK(MIK_IsStopRequested(rt));
    CHECK_FALSE(JS_HasException(ctx));
    CHECK(read_global_string(ctx, "__ran") != "yes");

    /* The loop has nothing left to report: no second handler call. */
    g_error_seen.clear();
    MIK_Loop(rt);
    CHECK(g_error_seen.empty());
}

/* Same failure through the no-buffer variant, on a fresh runtime so the stop
 * flag can only come from this call. */
TEST_CASE_FIXTURE(EntryFixture, "RunEntry without an err_buf reports a sync failure too" *
                                    doctest::test_suite("entry")) {
    g_error_seen.clear();
    MIK_SetErrorHandler(rt, capture_error, nullptr);
    write("/app/main.js", "import 'mikro/sys/foo'\n");
    CHECK(MIK_RunEntry(rt, "/app/main.js") == -EFAULT);
    CHECK(g_error_seen.find("mikro/sys/foo") != std::string::npos);
    CHECK(MIK_IsStopRequested(rt));
    CHECK_FALSE(JS_HasException(ctx));
}

TEST_CASE_FIXTURE(EntryFixture, "the test-emit handler receives __testEmit payloads" *
                                    doctest::test_suite("entry")) {
    g_emit_seen.clear();
    MIK_EnableTestHelpers(rt); /* __testEmit is opt-in */
    MIK_SetTestEmitHandler(rt, capture_emit, nullptr);
    const char* src = "__testEmit(JSON.stringify({kind: 'check', ok: true}))";
    JSValue rv = MIK_EvalScriptContent(ctx, src, strlen(src));
    CHECK_FALSE(JS_IsException(rv));
    JS_FreeValue(ctx, rv);
    CHECK(g_emit_seen.find("\"kind\"") != std::string::npos);
    CHECK(g_emit_seen.find("check") != std::string::npos);
}

TEST_CASE_FIXTURE(EntryFixture, "a throwing promise job stops the runtime" *
                                    doctest::test_suite("entry")) {
    g_error_seen.clear();
    MIK_SetErrorHandler(rt, capture_error, nullptr);
    JSValue rv = MIK_EvalScriptContent(
        ctx, "Promise.resolve().then(() => { throw new Error('job-boom') })",
        (size_t)strlen("Promise.resolve().then(() => { throw new Error('job-boom') })"));
    JS_FreeValue(ctx, rv);
    CHECK(MIK_Loop(rt) == 1); /* job error requests stop */
    CHECK(g_error_seen.find("job-boom") != std::string::npos);
    CHECK(MIK_IsStopRequested(rt));
}

TEST_CASE_FIXTURE(EntryFixture, "a pending C-level exception stops the loop" *
                                    doctest::test_suite("entry")) {
    g_error_seen.clear();
    MIK_SetErrorHandler(rt, capture_error, nullptr);
    JS_ThrowTypeError(ctx, "left-behind");
    CHECK(MIK_Loop(rt) == 1);
    CHECK(g_error_seen.find("left-behind") != std::string::npos);
    CHECK(MIK_IsStopRequested(rt));
}

namespace {

/* Expands __MAGIC__ to 123; must return a null-terminated malloc'd string. */
static char* entry_preprocess(const char* module_name, const char* source, size_t len,
                              size_t* out_len, void* opaque) {
    (void)module_name;
    (void)opaque;
    std::string src(source, len);
    size_t pos = src.find("__MAGIC__");
    if (pos == std::string::npos) return nullptr;
    src.replace(pos, strlen("__MAGIC__"), "123");
    char* out = static_cast<char*>(malloc(src.size() + 1));
    memcpy(out, src.data(), src.size());
    out[src.size()] = '\0';
    *out_len = src.size();
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(EntryFixture, "the preprocessor applies to direct module content" *
                                    doctest::test_suite("entry")) {
    MIK_SetPreprocessor(rt, entry_preprocess, nullptr);
    const char* src = "globalThis.__pp = __MAGIC__\nexport const ok = 1\n";
    JSValue rv = MIK_EvalModuleContent(ctx, "/app/pp.js", src, strlen(src));
    CHECK_FALSE(JS_IsException(rv));
    JS_FreeValue(ctx, rv);
    CHECK(read_global_string(ctx, "__pp") == "123");
}

/* ── Panic grace window ──────────────────────────────────────────── */

namespace {

static int64_t g_grace_now_us = 0;
static int g_grace_restarts = 0;
static int g_grace_sleeps = 0;

/* Added on every clock read so a serve-loop wait makes time pass. */
static int64_t g_grace_step_us = 0;

static int64_t grace_boot_us(void) {
    g_grace_now_us += g_grace_step_us;
    return g_grace_now_us;
}
static void grace_restart(void) {
    g_grace_restarts++;
}
static void grace_deep_sleep(uint64_t us) {
    (void)us;
    g_grace_sleeps++;
}
static int grace_read(uint8_t*, size_t, void*) {
    errno = 0;
    return -1;
}
static void grace_write(const void*, size_t, void*) {}
/* Idle transport: nothing to read until the panic action has fired, then
 * EOF. Drives the serve loop's wait through a grace window. The call cap
 * turns a wait that never reaches the deadline into a failure, not a hang. */
static int g_grace_idle_reads = 0;
static int grace_idle_read(uint8_t*, size_t, void*) {
    if (g_grace_restarts + g_grace_sleeps > 0 || ++g_grace_idle_reads > 1000) return 0;
    errno = EAGAIN;
    return -1;
}

}  // namespace

/* Restores the global platform even when a doctest assertion unwinds. */
struct PlatformGuard {
    const MIKPlatform* saved;
    explicit PlatformGuard(const MIKPlatform* fake) : saved(MIK_GetPlatform()) {
        MIK_SetPlatform(fake);
    }
    ~PlatformGuard() { MIK_SetPlatform(saved); }
};

TEST_CASE("MIK_Stop arms a grace window; the deadline triggers the panic action" *
          doctest::test_suite("entry")) {
    MIKPlatform fake = *MIK_GetPlatform();
    fake.get_boot_us = grace_boot_us;
    fake.restart = grace_restart;
    fake.deep_sleep_us = grace_deep_sleep;
    PlatformGuard guard(&fake);
    g_grace_now_us = 1000;
    g_grace_restarts = 0;
    g_grace_sleeps = 0;
    g_grace_step_us = 0;

    /* restart mode */
    {
        MIKRuntime* rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        MIKReplTransport transport = {};
        transport.read = grace_read;
        transport.write = grace_write;
        MIK_ProtocolOpen(&transport); /* repl active => MIK_Stop arms the window */
        MIK_ProtocolAttach(rt);

        MIK_Stop(rt);
        CHECK(MIK_Loop(rt) == 0); /* inside the grace window: no restart yet */
        CHECK(g_grace_restarts == 0);
        g_grace_now_us += 60LL * 1000 * 1000; /* leap past the deadline */
        MIK_Loop(rt);
        CHECK(g_grace_restarts == 1);

        MIK_ProtocolDetach();
        MIK_ProtocolClose();
        MIK_FreeRuntime(rt);
    }

    /* deep-sleep mode */
    {
        MIKRuntime* rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        MIKConfig config;
        MIK_DefaultConfig(&config);
        config.panic_mode = MIK_PANIC_DEEP_SLEEP;
        config.panic_sleep_duration_ms = 5000;
        MIK_SetConfig(rt, &config);
        MIKReplTransport transport = {};
        transport.read = grace_read;
        transport.write = grace_write;
        MIK_ProtocolOpen(&transport);
        MIK_ProtocolAttach(rt);

        MIK_Stop(rt);
        g_grace_now_us += 60LL * 1000 * 1000;
        MIK_Loop(rt);
        CHECK(g_grace_sleeps == 1);

        MIK_ProtocolDetach();
        MIK_ProtocolClose();
        MIK_FreeRuntime(rt);
    }

    /* test mode: MIK_Stop must NOT arm a restart */
    {
        MIKRuntime* rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        MIK_EnableTestHelpers(rt);
        MIKReplTransport transport = {};
        transport.read = grace_read;
        transport.write = grace_write;
        MIK_ProtocolOpen(&transport);
        MIK_ProtocolAttach(rt);

        int restarts_before = g_grace_restarts;
        MIK_Stop(rt);
        CHECK(MIK_Loop(rt) == 1); /* plain stop, no grace window */
        g_grace_now_us += 60LL * 1000 * 1000;
        MIK_Loop(rt);
        CHECK(g_grace_restarts == restarts_before);

        MIK_ProtocolDetach();
        MIK_ProtocolClose();
        MIK_FreeRuntime(rt);
    }
}

/* On device the loop idles inside the transport read, so that is where a
 * panic lands. The read must keep serving through the grace window instead
 * of bailing on the first non-zero MIK_Loop, or the firmware restarts at
 * once from its transport-closed path and onPanic (delay, deep sleep) never
 * happens. */
TEST_CASE("The serve loop waits out the grace window and takes the panic action" *
          doctest::test_suite("entry")) {
    MIKPlatform fake = *MIK_GetPlatform();
    fake.get_boot_us = grace_boot_us;
    fake.restart = grace_restart;
    fake.deep_sleep_us = grace_deep_sleep;
    PlatformGuard guard(&fake);
    g_grace_now_us = 1000;
    g_grace_restarts = 0;
    g_grace_sleeps = 0;
    g_grace_step_us = 100 * 1000;
    g_grace_idle_reads = 0;

    MIKRuntime* rt = MIK_NewRuntime();
    REQUIRE(rt != nullptr);
    MIKConfig config;
    MIK_DefaultConfig(&config);
    config.panic_mode = MIK_PANIC_DEEP_SLEEP;
    config.panic_sleep_duration_ms = 5000;
    MIK_SetConfig(rt, &config);
    MIKReplTransport transport = {};
    transport.read = grace_idle_read;
    transport.write = grace_write;
    MIK_ProtocolOpen(&transport);
    MIK_ProtocolAttach(rt);

    /* The panic must happen inside the read's own MIK_Loop pass: that is
     * the pass that returns non-zero, and the one the old code bailed on. */
    JSContext* ctx = MIK_GetJSContext(rt);
    static const char src[] = "setTimeout(() => { throw new Error('boom') }, 0)";
    JSValue v = MIK_EvalScriptContent(ctx, src, sizeof(src) - 1);
    REQUIRE_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);

    uint8_t byte;
    CHECK_FALSE(mik__proto_read_exact(&transport, &byte, 1));
    /* EOF came only after the panic action, not on the panic itself. */
    CHECK(g_grace_sleeps == 1);

    MIK_ProtocolDetach();
    MIK_ProtocolClose();
    MIK_FreeRuntime(rt);
}
