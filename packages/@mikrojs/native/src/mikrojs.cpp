#include "mikrojs/mikrojs.h"

#include <quickjs.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include "mikrojs/mem.h"
#include "mikrojs/platform.h"
#include "mikrojs/private.h"
#include "mikrojs/utils.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
/* ASan inflates native frames severalfold; the regular default overflows
 * on shallow JS recursion in instrumented builds. Never active on device. */
#define MIK__DEFAULT_STACK_SIZE 8 * 1024 * 1024
#else
#define MIK__DEFAULT_STACK_SIZE 1024 * 1024  // 1 MB
#endif

/* JS malloc functions. Routed through the mik__js_* family so the QuickJS
 * heap can be relocated to PSRAM on chips with both internal SRAM and PSRAM,
 * leaving internal SRAM free for mbedTLS, WiFi/BLE, and DMA buffers. */

static void* mik__mf_calloc(void* opaque, size_t count, size_t size) {
    (void)opaque;
    return mik__js_calloc(count, size);
}

static void* mik__mf_malloc(void* opaque, size_t size) {
    (void)opaque;
    return mik__js_malloc(size);
}

static void mik__mf_free(void* opaque, void* ptr) {
    (void)opaque;
    mik__free(ptr);
}

static void* mik__mf_realloc(void* opaque, void* ptr, size_t size) {
    (void)opaque;
    return mik__js_realloc(ptr, size);
}

static const JSMallocFunctions mik_mf = {
    .js_calloc = mik__mf_calloc,
    .js_malloc = mik__mf_malloc,
    .js_free = mik__mf_free,
    .js_realloc = mik__mf_realloc,
    .js_malloc_usable_size = mik__malloc_usable_size,
};

/* Build a frozen object from stored env vars. */
static JSValue mik__build_env_object(JSContext* ctx) {
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    JSValue env = JS_NewObject(ctx);

    for (const auto& [key, value] : mik_rt->env_vars) {
        JS_DefinePropertyValueStr(ctx, env, key.c_str(), JS_NewString(ctx, value.c_str()),
                                  JS_PROP_ENUMERABLE);
    }

    /* Freeze to prevent mutation */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue object_ctor = JS_GetPropertyStr(ctx, global, "Object");
    JSValue freeze = JS_GetPropertyStr(ctx, object_ctor, "freeze");
    JSValue frozen = JS_Call(ctx, freeze, object_ctor, 1, &env);
    JS_FreeValue(ctx, frozen);
    JS_FreeValue(ctx, freeze);
    JS_FreeValue(ctx, object_ctor);
    JS_FreeValue(ctx, global);

    return env;
}

/* Helper: copy named properties from a namespace object into module exports */
static void mik__export_from_ns(JSContext* ctx, JSModuleDef* m, JSValue ns,
                                const char* const* names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        JS_SetModuleExport(ctx, m, names[i], JS_GetPropertyStr(ctx, ns, names[i]));
    }
}

/* Helper: register export names on a module definition */
static void mik__add_exports(JSContext* ctx, JSModuleDef* m, const char* const* names,
                             size_t count) {
    for (size_t i = 0; i < count; i++) {
        JS_AddModuleExport(ctx, m, names[i]);
    }
}

static const char* const sys_exports[] = {
    "evalScript",      "memoryUsage",           "storageUsage", "jsMemoryUsage",
    "gc",              "setTime",               "uptime",       "restart",
    "version",         "board",                 "firmware",     "deviceId",
    "deviceName",      "setDeviceName",         "resetReason",  "activeTimers",
    "unloadNamespace", "isUnloadableNamespace"};

static int mik__sys_module_init(JSContext* ctx, JSModuleDef* m) {
    JSValue ns = JS_NewObjectProto(ctx, JS_NULL);
    mik__sys_api_init(ctx, ns);
    mik__export_from_ns(ctx, m, ns, sys_exports, countof(sys_exports));
    JS_FreeValue(ctx, ns);
    return 0;
}

static const char* const stdio_exports[] = {"stdout", "stdin"};

static int mik__stdio_module_init(JSContext* ctx, JSModuleDef* m) {
    JSValue ns = JS_NewObjectProto(ctx, JS_NULL);
    mik__stdio_init(ctx, ns);
    mik__export_from_ns(ctx, m, ns, stdio_exports, countof(stdio_exports));
    JS_FreeValue(ctx, ns);
    return 0;
}

/* Host promise-rejection tracker. Rather than report eagerly the instant a
 * promise rejects, defer the decision to the end-of-turn flush
 * (mik__flush_unhandled_rejections): a promise that rejects without a
 * handler is queued, and a promise that later gets a handler is removed
 * from the queue. This mirrors the HTML/WinterCG unhandledrejection
 * algorithm. It matters because some promises are born rejected before a
 * handler can be attached. The clearest example is an async function that
 * throws synchronously: its result promise rejects before the caller's
 * `await` attaches a handler. Reporting eagerly surfaced that transient
 * rejection as a spurious "Uncaught (in promise)".
 *
 * This is the same add-on-reject / remove-on-handle / report-after-drain
 * pattern quickjs-libc's js_std_promise_rejection_tracker uses. */
static void mik__promise_rejection_tracker(JSContext* ctx, JSValue promise, JSValue reason,
                                           bool is_handled, void* opaque) {
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    CHECK_NOT_NULL(mik_rt);

    if (mik_rt->freeing) {
        return;
    }

    auto& pending = mik_rt->pending_rejections;
    void* key = JS_VALUE_GET_PTR(promise);

    if (is_handled) {
        /* A handler was attached to a previously-rejected promise: cancel its
         * pending report. */
        for (size_t i = 0; i < pending.size(); i++) {
            if (JS_VALUE_GET_PTR(pending[i].promise) == key) {
                JS_FreeValue(ctx, pending[i].promise);
                JS_FreeValue(ctx, pending[i].reason);
                pending.erase(pending.begin() + i);
                return;
            }
        }
        return;
    }

    /* Rejected with no handler (yet): queue it. Hold references to both the
     * promise and the reason so they stay alive until the end-of-turn check. */
    pending.push_back({JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)});
}

/* End-of-turn unhandled-rejection check. Any promise still queued after a
 * microtask drain rejected and never got a handler: report it, notify the
 * host error handler, and halt. */
void mik__flush_unhandled_rejections(JSContext* ctx) {
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (!mik_rt || mik_rt->freeing || mik_rt->pending_rejections.empty()) {
        return;
    }
    /* During interactive REPL eval, the eval pump polls its result promise
     * and reports rejections via the throw path; leave the queue alone until
     * the eval finishes so we don't fight that model. */
    if (mik__repl_is_evaluating()) {
        return;
    }

    /* Move the queue aside before reporting. Reporting doesn't enqueue JS
     * jobs, but this keeps the runtime's vector in a clean state regardless. */
    std::vector<MIKRejectedPromise> rejected;
    rejected.swap(mik_rt->pending_rejections);

    /* The same reason can reach reporting twice: the entry-eval path reads a
     * rejected module promise by value and the host dumps it via the throw
     * path while the promise itself stays queued here. mik__report_uncaught
     * dedups by reason-object identity, so the duplicate is a no-op; only act
     * on a fresh report so the host bridge and the halt don't fire twice.
     * (quickjs-libc reports every entry; we collapse same-error duplicates.) */
    for (const MIKRejectedPromise& rp : rejected) {
        if (mik__report_uncaught(ctx, rp.reason, true)) {
            /* Notify the error handler (e.g. host bridge) directly. Don't
             * JS_Throw here: we're between jobs, and throwing would be picked
             * up as a spurious pending exception. */
            if (mik_rt->error_handler_fn) {
                mik_rt->error_handler_fn(ctx, rp.reason, mik_rt->error_handler_opaque);
            }
            MIK_Stop(mik_rt);
        }
        JS_FreeValue(ctx, rp.promise);
        JS_FreeValue(ctx, rp.reason);
    }
}

/* QuickJS only runs the cycle collector automatically once malloc_size
 * crosses the runtime's GC threshold (256 KB by default), and js_malloc_rt
 * fails against the memory limit without attempting a GC first. On devices
 * where mem_limit is below the default threshold the collector would never
 * fire before allocations start failing, so cyclic garbage (promise chains,
 * closures) accumulates straight into OOM with collectable memory still
 * alive. Cap the threshold below the limit so collection happens first.
 * QuickJS raises the threshold to 1.5x the live size after every GC pass,
 * which can push it back above the limit, so MIK_Loop re-applies the cap
 * each turn. */
static void mik__clamp_gc_threshold(MIKRuntime* mik_rt) {
    if (mik_rt->options.mem_limit <= 0) {
        return;
    }
    size_t limit = (size_t)mik_rt->options.mem_limit;
    size_t cap = limit - limit / 8;
    if (JS_GetGCThreshold(mik_rt->rt) > cap) {
        JS_SetGCThreshold(mik_rt->rt, cap);
    }
}

void MIK_DefaultOptions(MIKRunOptions* options) {
    static MIKRunOptions default_options = {
        .mem_limit = 0,
        .stack_size = MIK__DEFAULT_STACK_SIZE,
        .use_psram_heap = false,
    };

    memcpy(options, &default_options, sizeof(*options));
}

MIKRuntime* MIK_NewRuntime(void) {
    MIKRunOptions options;
    MIK_DefaultOptions(&options);
    return MIK_NewRuntimeInternal(&options);
}

MIKRuntime* MIK_NewRuntimeOptions(MIKRunOptions* options) {
    return MIK_NewRuntimeInternal(options);
}

static void mik__check_module_collisions(void);

MIKRuntime* MIK_NewRuntimeInternal(MIKRunOptions* options) {
    const MIKPlatform* platform = MIK_GetPlatform();
    JSRuntime* rt = NULL;
    JSContext* ctx = NULL;
    MIKRuntime* mik_rt = static_cast<MIKRuntime*>(mik__mallocz(sizeof(*mik_rt)));

    /* Placement-new to initialize C++ members (vectors, strings) */
    new (mik_rt) MIKRuntime();

    memcpy(&mik_rt->options, options, sizeof(*options));
    MIK_DefaultConfig(&mik_rt->config);
    /* Runtimes that never see MIK_SetConfig (host tests, the Node addon)
     * still get the default blocking budget. */
    mik__watchdog_arm(mik_rt);

    /* Switch the QuickJS heap to PSRAM before constructing the runtime,
     * so JS_NewRuntime2 and every subsequent QuickJS allocation lands
     * in PSRAM. The MIKRuntime struct itself stays in internal SRAM:
     * it was allocated before this point. */
    mik__set_quickjs_heap_psram(options->use_psram_heap);

    rt = JS_NewRuntime2(&mik_mf, NULL);
    CHECK_NOT_NULL(rt);
    mik_rt->rt = rt;

    /* Seed Math.random() with real entropy.
     * On ESP32 the system clock starts near epoch 0 on every boot, producing
     * the same seed. Use platform RNG to inject entropy. */
    struct timeval tv_orig;
    gettimeofday(&tv_orig, NULL);
    struct timeval tv_rand = {
        .tv_sec = tv_orig.tv_sec,
        .tv_usec = static_cast<suseconds_t>(platform->random() % 1000000),
    };
    settimeofday(&tv_rand, NULL);

    /* Replacement for JS_NewContext that drops DOMException. Per-intrinsic
     * measurement showed DOMException alone costs ~4.3 KB heap per runtime,
     * while the other optional intrinsics (Proxy ~274 B, Performance ~303 B)
     * are cheap enough to keep for language/standards completeness. Eval
     * must be kept because the JS_Eval C API crashes without it. */
    ctx = JS_NewContextRaw(rt);
    CHECK_NOT_NULL(ctx);
    if (JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicDate(ctx) ||
        JS_AddIntrinsicEval(ctx) || JS_AddIntrinsicRegExp(ctx) ||
        JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicTypedArrays(ctx) ||
        JS_AddIntrinsicPromise(ctx) || JS_AddIntrinsicWeakRef(ctx) ||
        JS_AddPerformance(ctx)) {
        JS_FreeContext(ctx);
        ctx = NULL;
    }
    CHECK_NOT_NULL(ctx);

    settimeofday(&tv_orig, NULL);
    mik_rt->ctx = ctx;

    JS_SetRuntimeOpaque(rt, mik_rt);
    JS_SetContextOpaque(ctx, mik_rt);

    /* Initialize lazily-assigned JSValue fields so module init code can
     * distinguish "unset" from "set to some object". */
    mik_rt->result_proto = JS_UNDEFINED;
    mik_rt->result_ok_void_singleton = JS_UNDEFINED;
    mik_rt->http_body_consumed_ctor = JS_UNDEFINED;

    /* Default fs read cap: 64 KiB. Large enough for typical config/JSON
     * payloads on MCU; small enough that a runaway readFile() can't
     * exhaust the heap. Overridden via MIK_SetFSReadMax. */
    mik_rt->fs_read_max = 65536;

    /* Reserve the unhandled-rejection queue up-front so the rejection tracker
     * never has to grow a std::vector on the rejection path. That path fires
     * for a QuickJS null-OOM rejection, and a reallocation there could throw
     * std::bad_alloc — which, with CONFIG_COMPILER_CXX_EXCEPTIONS=n on ESP32,
     * aborts and masks the very OOM we're trying to report. Four slots covers
     * any realistic per-turn rejection count; the runtime halts on the first
     * one anyway. */
    mik_rt->pending_rejections.reserve(4);

    /* Check for duplicate native module registrations (global registry) */
    mik__check_module_collisions();

    /* Set memory limit */
    JS_SetMemoryLimit(rt, options->mem_limit);

    /* Make sure the cycle collector fires before the memory limit does
     * (see mik__clamp_gc_threshold). */
    mik__clamp_gc_threshold(mik_rt);

    /* Set stack size */
    JS_SetMaxStackSize(rt, options->stack_size);
    /* loader for ES modules */
    JS_SetModuleLoaderFunc(rt, mik_module_normalizer, mik_module_loader, mik_rt);

    /* unhandled promise rejection tracker */
    JS_SetHostPromiseRejectionTracker(rt, mik__promise_rejection_tracker, NULL);

    /* Blocking watchdog: polled every 10k branches/calls. */
    JS_SetInterruptHandler(rt, mik__watchdog_interrupt, mik_rt);

    /* Register internal C modules */
    JSModuleDef* sys_mod = JS_NewCModule(ctx, "native:mikro/sys", mik__sys_module_init);
    CHECK_NOT_NULL(sys_mod);
    mik__add_exports(ctx, sys_mod, sys_exports, countof(sys_exports));

    /* Core native modules registered explicitly (not via MIK_REGISTER_MODULE
     * which relies on linker magic that doesn't work in all build contexts).
     *
     * Result must be initialized before anything that might allocate a
     * Result object — the helpers mik__result_ok/mik__result_err rely on
     * rt->result_proto being set. */
    mik__result_init(ctx);
    mik__cbor_init(ctx);
    mik__udp_init(ctx);
    mik__watchdog_init(ctx);
    mik__observable_init(ctx);

    /* Native mikrojs modules (replace bytecode builtins). The http client
     * modules are NOT registered here: they load lazily through the C-module
     * table in modules.cpp so virtual modules keep precedence. Schema takes
     * the other lazy route, putting a descriptor on the native: registry,
     * because mikro/schema keeps a small facade for its Result-returning
     * parse(); both are resolved after the virtual-module check. */
    mik__schema_register();
    mik__inspect_register(ctx);
    mik__pub_fs_register(ctx);

    /* Call registered native module init functions */
    for (const auto& mod : mik_rt->native_modules) {
        mod.init_fn(ctx);
    }

    JSModuleDef* stdio_mod = JS_NewCModule(ctx, "native:mikro/stdio", mik__stdio_module_init);
    CHECK_NOT_NULL(stdio_mod);
    mik__add_exports(ctx, stdio_mod, stdio_exports, countof(stdio_exports));

    /* Web standard globals */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    mik__text_encoding_init(ctx, global_obj);
    mik__abort_init(ctx, global_obj);

    /* Timers */
    mik_rt->timers = MIK_NewTimerRegistry();
    mik__timers_init(ctx, global_obj);

    /* Console global (native C++) */
    mik__console_init(ctx, global_obj);

    JS_FreeValue(ctx, global_obj);

    /* Build import.meta.env as a frozen object from stored env vars. */
    mik_rt->env_obj = mik__build_env_object(ctx);

    /* Stdin */
    mik_rt->stdin_state.on_data = JS_UNDEFINED;

    return mik_rt;
}

void MIK_FreeRuntime(MIKRuntime* mik_rt) {
    mik_rt->freeing = true;

    /* Release stdin handler */
    if (!JS_IsUndefined(mik_rt->stdin_state.on_data)) {
        JS_FreeValue(mik_rt->ctx, mik_rt->stdin_state.on_data);
        mik_rt->stdin_state.on_data = JS_UNDEFINED;
    }

    /* Release any still-pending unhandled-rejection entries. */
    for (const MIKRejectedPromise& rp : mik_rt->pending_rejections) {
        JS_FreeValue(mik_rt->ctx, rp.promise);
        JS_FreeValue(mik_rt->ctx, rp.reason);
    }
    mik_rt->pending_rejections.clear();

    /* Destroy registered loop consumers */
    for (const auto& consumer : mik_rt->loop_consumers) {
        if (consumer.destroy_fn) {
            consumer.destroy_fn(mik_rt->ctx);
        }
    }

    /* Destroy all timers */
    mik__timers_destroy(mik_rt->ctx);
    delete mik_rt->timers;
    mik_rt->timers = nullptr;

    mik__observable_dispatch_free(mik_rt);

    /* Destroy the JS engine. */
    JS_FreeValue(mik_rt->ctx, mik_rt->env_obj);
    mik_rt->env_obj = JS_UNDEFINED;
    JS_FreeValue(mik_rt->ctx, mik_rt->result_ok_void_singleton);
    mik_rt->result_ok_void_singleton = JS_UNDEFINED;
    JS_FreeValue(mik_rt->ctx, mik_rt->http_body_consumed_ctor);
    mik_rt->http_body_consumed_ctor = JS_UNDEFINED;
    JS_FreeValue(mik_rt->ctx, mik_rt->result_proto);
    mik_rt->result_proto = JS_UNDEFINED;
    JS_FreeContext(mik_rt->ctx);
    JS_FreeRuntime(mik_rt->rt);

    /* Call destructor for C++ members, then free */
    mik_rt->~MIKRuntime();
    mik__free(mik_rt);
}

JSContext* MIK_GetJSContext(MIKRuntime* mik_rt) { return mik_rt->ctx; }

MIKRuntime* MIK_GetRuntime(JSContext* ctx) {
    return static_cast<MIKRuntime*>(JS_GetContextOpaque(ctx));
}

void MIK_SetFSBasePath(MIKRuntime* mik_rt, const char* base_path) {
    mik_rt->fs_base_path = base_path;
}

void MIK_SetFSRoot(MIKRuntime* mik_rt, const char* fs_root) {
    mik_rt->fs_root = fs_root;
}

void MIK_SetFSLimit(MIKRuntime* mik_rt, size_t limit) {
    mik_rt->fs_limit = limit;
}

void MIK_SetFSReadMax(MIKRuntime* mik_rt, size_t bytes) {
    mik_rt->fs_read_max = bytes ? bytes : 65536;
}

void MIK_SetEnvVar(MIKRuntime* mik_rt, const char* key, const char* value) {
    for (auto& [k, v] : mik_rt->env_vars) {
        if (k == key) {
            v = value;
            return;
        }
    }
    mik_rt->env_vars.emplace_back(key, value);
}

void MIK_RebuildEnv(MIKRuntime* mik_rt) {
    JS_FreeValue(mik_rt->ctx, mik_rt->env_obj);
    mik_rt->env_obj = mik__build_env_object(mik_rt->ctx);
}

void MIK_RegisterVirtualModule(MIKRuntime* mik_rt, const char* name, const char* source,
                               size_t source_len) {
    mik_rt->virtual_modules[name] = std::string(source, source_len);
}

void MIK_SetPreprocessor(MIKRuntime* mik_rt, MIKPreprocessFn fn, void* opaque) {
    mik_rt->preprocess_fn = fn;
    mik_rt->preprocess_opaque = opaque;
}

void MIK_SetErrorHandler(MIKRuntime* mik_rt, MIKErrorHandlerFn fn, void* opaque) {
    mik_rt->error_handler_fn = fn;
    mik_rt->error_handler_opaque = opaque;
}

void MIK_SetTestEmitHandler(MIKRuntime* mik_rt, MIKTestEmitHandlerFn fn, void* opaque) {
    mik_rt->test_emit_fn = fn;
    mik_rt->test_emit_opaque = opaque;
}

void MIK_RegisterNativeModuleInit(MIKRuntime* mik_rt, const char* name,
                                  MIKNativeModuleInitFn init_fn) {
    mik_rt->native_modules.push_back({name, init_fn});
}

void MIK_RegisterLoopConsumer(MIKRuntime* mik_rt, MIKLoopConsumeFn consume_fn,
                              MIKLoopDestroyFn destroy_fn) {
    /* Idempotent: a module can be brought up twice — once by native code that
     * uses its machinery directly, once by the loader when JS imports it — and
     * registering the pair twice would run its destroy twice at teardown. */
    for (const auto& consumer : mik_rt->loop_consumers) {
        if (consumer.consume_fn == consume_fn && consumer.destroy_fn == destroy_fn) return;
    }
    mik_rt->loop_consumers.push_back({consume_fn, destroy_fn});
}

void MIK_SetModuleData(MIKRuntime* mik_rt, int slot, void* data) {
    if (slot >= 0 && slot < MIK_MODULE_DATA_SLOTS) {
        mik_rt->module_data[slot] = data;
    }
}

void* MIK_GetModuleData(MIKRuntime* mik_rt, int slot) {
    if (slot >= 0 && slot < MIK_MODULE_DATA_SLOTS) {
        return mik_rt->module_data[slot];
    }
    return nullptr;
}

/* ── Global module registry (populated by MIK_REGISTER_MODULE constructors) ── */

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
mik_module_desc_t* mik__module_registry_head = nullptr;

int MIK_ReserveModuleSlot(void) {
    /* Process-wide and never released: a module keeps one index for the life
     * of the process, so every runtime agrees on what that index holds. A
     * per-runtime counter would restart at zero and alias modules across
     * runtimes. Callers reserve once (guard on their cached slot being < 0);
     * reserving per runtime would exhaust the table instead. */
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static int next_slot = 0;
    int slot = next_slot++;
    CHECK(slot < MIK_MODULE_DATA_SLOTS);
    return slot;
}

static void mik__check_module_collisions(void) {
    for (mik_module_desc_t* a = mik__module_registry_head; a != nullptr; a = a->next) {
        /* Every native: name must be package-qualified ("native:<package>/…").
         * The compile-time macro enforces this when MIK_PACKAGE_NAME is set;
         * this catches a registration that bypassed it (e.g. a hand-rolled
         * driver), so an unqualified name can't squat the bare namespace. */
        if (strncmp(a->name, "native:", 7) == 0 && strchr(a->name + 7, '/') == nullptr) {
            fprintf(stderr, "FATAL: native module name not package-qualified: \"%s\"\n", a->name);
            abort();
        }
        for (mik_module_desc_t* b = a->next; b != nullptr; b = b->next) {
            if (strcmp(a->name, b->name) == 0) {
                fprintf(stderr, "FATAL: native module name collision: \"%s\"\n", a->name);
                abort();
            }
        }
    }
    for (mik_ext_builtin_t* a = mik__ext_builtin_head; a != nullptr; a = a->next) {
        for (mik_ext_builtin_t* b = a->next; b != nullptr; b = b->next) {
            if (strcmp(a->name, b->name) == 0) {
                fprintf(stderr, "FATAL: builtin module name collision: \"%s\"\n", a->name);
                abort();
            }
        }
    }
}

/* A chain of already-settled promises drains as one uninterrupted storm of
 * jobs, and the test supervisor strings whole files of such chains together
 * with runtime recycles in between. On ESP32 the main task runs above the
 * idle task, so a multi-second stretch like that starves idle: the task
 * watchdog (5 s, watching IDLE0) prints a register dump, and FreeRTOS
 * housekeeping (freeing TCBs of exited tasks, e.g. per-request HTTP tasks)
 * stalls. Force one platform yield per second of continuous job execution
 * so idle always gets a slice. The timestamp is global on purpose: the
 * starvation accumulates across MIK_Loop calls and across runtime recycles.
 * Costs at most one tick (~10 ms) per second of busy work. */
#define MIK__YIELD_INTERVAL_US (1000 * 1000)

/* Atomic because the Node addon can pump two runtimes' loops from
 * different worker threads; relaxed ordering is enough — a missed or
 * extra yield is harmless, a torn 64-bit write is not. */
static std::atomic<int64_t> mik__last_yield_us{0};

static void mik__maybe_yield(void) {
    const MIKPlatform* platform = MIK_GetPlatform();
    int64_t now = platform->get_boot_us();
    int64_t last = mik__last_yield_us.load(std::memory_order_relaxed);
    /* now < last happens when the boot clock resets (deep sleep) or a test
     * swaps in a platform with a different clock; restart the interval. */
    if (last == 0 || now < last) {
        mik__last_yield_us.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - last >= MIK__YIELD_INTERVAL_US) {
        platform->yield();
        mik__last_yield_us.store(platform->get_boot_us(), std::memory_order_relaxed);
    }
}

void mik__execute_jobs(JSContext* ctx) {
    JSContext* ctx1;
    int err;
    bool ran_any = false;

    /* execute the pending jobs */
    for (;;) {
        err = JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1);
        if (err <= 0) {
            if (err < 0) {
                MIKRuntime* mik_rt = MIK_GetRuntime(ctx1);
                if (mik_rt && mik_rt->error_handler_fn && JS_HasException(ctx1)) {
                    JSValue exc = JS_GetException(ctx1);
                    mik_rt->error_handler_fn(ctx1, exc, mik_rt->error_handler_opaque);
                    JS_Throw(ctx1, exc);
                }
                mik_dump_error(ctx1);
                /* Same rule as every other uncaught throw: panic. */
                if (mik_rt) MIK_Stop(mik_rt);
            }
            break;
        }
        ran_any = true;
        /* A GC pass inside this job raises the threshold to 1.5x live size,
         * which lands above mem_limit whenever live >= ~2/3 of the limit —
         * silently disabling cycle GC for the rest of the storm. Re-clamp
         * between jobs so collection keeps firing; the check is a field
         * read + compare when no GC ran. */
        MIKRuntime* job_rt = MIK_GetRuntime(ctx1);
        if (job_rt) {
            mik__clamp_gc_threshold(job_rt);
        }
        mik__maybe_yield();
    }

    /* The yield interval measures continuous BUSY work. An empty drain
     * means the loop is idling (the idle task is getting time between
     * turns), so re-arm the interval — otherwise the first job after a
     * quiet period pays a spurious ~10 ms yield for wall-clock time spent
     * doing nothing. */
    if (!ran_any) {
        mik__last_yield_us.store(MIK_GetPlatform()->get_boot_us(), std::memory_order_relaxed);
    }

    /* Microtask checkpoint: every reject/handle transition for this turn has
     * now run, so whatever is still queued is a genuine unhandled rejection. */
    mik__flush_unhandled_rejections(ctx);
}

/* main loop which calls the user JS callbacks */
int MIK_Loop(MIKRuntime* mik_rt) {
    const MIKPlatform* platform = MIK_GetPlatform();
    /* Feed the hardware watchdog before the grace-window return below, so
     * a long onPanic.delay never counts against it. */
    if (platform->feed_watchdog) {
        platform->feed_watchdog();
    }
    /* One blocking budget per pass, not per job: a fresh budget per job
     * would let `while (true) await 0` spin forever unnoticed. */
    mik__blocking_begin(mik_rt);
    /* Deferred restart (see MIK_Stop): once the grace window elapses, reboot
     * the device. While we're still in the window, return 0 without pumping
     * timers/consumers so the protocol serve loop keeps reading host
     * commands without firing any more user JS on the dead runtime. The
     * serve loop also skips its microtask drain on this condition. */
    if (mik_rt->restart_at_us > 0) {
        if (platform->get_boot_us() >= mik_rt->restart_at_us) {
            /* The grace window has elapsed; take the configured panic action.
             * In deep-sleep mode the timer wake reboots the chip, so the wake
             * IS the restart — and the live --recover window is forfeit by
             * design (the CPU is suspended). If the platform has no deep-sleep
             * hook (hosts), fall through to a plain restart. */
            if (mik_rt->config.panic_mode == MIK_PANIC_DEEP_SLEEP && platform->deep_sleep_us) {
                mik__print_error_line("[panic] deep-sleeping for %d ms",
                                      mik_rt->config.panic_sleep_duration_ms);
                platform->deep_sleep_us((uint64_t)mik_rt->config.panic_sleep_duration_ms * 1000);
            }
            mik__print_error_line("[panic] restarting");
            platform->restart();
        }
        return 0;
    }
    /* Feed/awake deadlines: a feed miss lands in the exception branch below,
     * an awake overrun in the stop_requested one. */
    mik__watchdog_check(mik_rt);
    if (mik_rt->stop_requested) {
        return 1;
    }
    if (JS_HasException(mik_rt->ctx)) {
        if (mik_rt->error_handler_fn) {
            JSValue exc = JS_GetException(mik_rt->ctx);
            mik_rt->error_handler_fn(mik_rt->ctx, exc, mik_rt->error_handler_opaque);
            /* Re-set the exception so mik_dump_error can consume it */
            JS_Throw(mik_rt->ctx, exc);
        }
        mik_dump_error(mik_rt->ctx);
        MIK_Stop(mik_rt);
        return 1;
    }
    mik__stdin_consume(mik_rt->ctx);
    mik__timers_consume(mik_rt->ctx);

    /* Call registered loop consumers */
    for (const auto& consumer : mik_rt->loop_consumers) {
        consumer.consume_fn(mik_rt->ctx);
    }

    mik__execute_jobs(mik_rt->ctx);

    /* Backstop for GC passes outside the job drain (timer callbacks, loop
     * consumers, top-level eval): a pass there may have raised the
     * threshold above the memory limit; pull it back so the next turn's
     * collection still fires before allocations fail. Within the job
     * drain, mik__execute_jobs re-clamps after every job. */
    mik__clamp_gc_threshold(mik_rt);

    /* The end-of-turn unhandled-rejection flush inside mik__execute_jobs may
     * have requested a stop; surface it this iteration rather than making the
     * caller loop once more to notice. */
    return mik_rt->stop_requested ? 1 : 0;
}

void MIK_SetConfig(MIKRuntime* mik_rt, const MIKConfig* config) {
    CHECK_NOT_NULL(mik_rt);
    CHECK_NOT_NULL(config);
    memcpy(&mik_rt->config, config, sizeof(*config));
    if (config->fs_read_max > 0) {
        mik_rt->fs_read_max = config->fs_read_max;
    }
    mik__watchdog_arm(mik_rt);
}

/* Record a profile entry for the entry module loaded via MIK_EvalModule.
 * Uses the same exclusive-delta accounting as the module loader. */
static void mik__record_entry_profile(MIKRuntime* mik_rt,
                                      JSRuntime* rt,
                                      const char* name,
                                      int64_t malloc_before,
                                      size_t entries_before) {
    JSMemoryUsage after;
    JS_ComputeMemoryUsage(rt, &after);
    size_t total_delta = after.malloc_size > malloc_before
                             ? (size_t)(after.malloc_size - malloc_before)
                             : 0;
    size_t children_delta = 0;
    for (size_t i = entries_before; i < mik_rt->profile_entries.size(); i++) {
        children_delta += mik_rt->profile_entries[i].delta_bytes;
    }
    size_t exclusive_delta = total_delta > children_delta ? total_delta - children_delta : 0;

    MIKProfileEntry entry = {};
    size_t name_len = strlen(name);
    if (name_len >= MIK_PROFILE_NAME_MAX) {
        name_len = MIK_PROFILE_NAME_MAX - 1;
    }
    memcpy(entry.name, name, name_len);
    entry.name[name_len] = '\0';
    entry.delta_bytes = exclusive_delta;
    entry.order = mik_rt->profile_entries.size();
    if (mik_rt->profile_entries.size() < mik_rt->profile_entries.capacity()) {
        mik_rt->profile_entries.push_back(entry);
    }
}

void MIK_EnableProfiling(MIKRuntime* mik_rt) {
    CHECK_NOT_NULL(mik_rt);
    mik_rt->profile_enabled = true;
    /* Capture the QuickJS heap baseline (runtime + context + built-in
     * prototypes). Everything profiled after this is module cost. */
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(mik_rt->rt, &mu);
    mik_rt->profile_baseline = mu.malloc_size > 0 ? (size_t)mu.malloc_size : 0;
    /* Reserve up-front so push_back during module loads never reallocates —
     * a reallocation mid-load would be invisible to JS_ComputeMemoryUsage
     * but could still blip memory the app doesn't see, which we want to
     * avoid even in the "profile buffer" itself. 256 slots covers every
     * realistic app import graph. */
    mik_rt->profile_entries.reserve(256);
}

size_t MIK_GetProfileEntryCount(MIKRuntime* mik_rt) {
    CHECK_NOT_NULL(mik_rt);
    return mik_rt->profile_entries.size();
}

const MIKProfileEntry* MIK_GetProfileEntries(MIKRuntime* mik_rt) {
    CHECK_NOT_NULL(mik_rt);
    return mik_rt->profile_entries.data();
}

size_t MIK_GetProfileBaseline(MIKRuntime* mik_rt) {
    CHECK_NOT_NULL(mik_rt);
    return mik_rt->profile_baseline;
}

void MIK_SetOOMHandler(MIKRuntime* mik_rt, MIKOOMHandlerFn fn, void* opaque) {
    CHECK_NOT_NULL(mik_rt);
    mik_rt->oom_handler_fn = fn;
    mik_rt->oom_handler_opaque = opaque;
}

bool MIK_ConsumeOOMFlag(MIKRuntime* mik_rt) {
    CHECK_NOT_NULL(mik_rt);
    bool was_set = mik_rt->oom_flagged;
    mik_rt->oom_flagged = false;
    return was_set;
}

void MIK_ReportOOM(MIKRuntime* mik_rt, const MIKOOMEvent* event) {
    CHECK_NOT_NULL(mik_rt);
    CHECK_NOT_NULL(event);
    mik_rt->oom_flagged = true;
    if (mik_rt->oom_handler_fn) {
        mik_rt->oom_handler_fn(event, mik_rt->oom_handler_opaque);
    }
}

bool MIK_IsStopRequested(MIKRuntime* mik_rt) {
    return mik_rt && mik_rt->stop_requested;
}

void MIK_Stop(MIKRuntime* mik_rt) {
    CHECK_NOT_NULL(mik_rt);
    /* REPL-evaluated code gets no exemption: a typo throws synchronously and
     * the eval path reports it without reaching here, so only async fallout
     * from typed code panics. */
    /* The single place stop_requested is set. Host embedders (Node addon)
     * observe it via MIK_Loop's return value; the firmware test supervisor
     * reads it via MIK_IsStopRequested. */
    mik_rt->stop_requested = true;
    /* Only firmware (protocol REPL attached) auto-restarts on uncaught
     * exceptions. Host embedders (Node addon, standalone tests) own their
     * own process lifecycle and surface errors via the error handler. */
    if (!MIK_IsReplActive()) {
        return;
    }
    /* In test mode, the supervisor wants stop_requested to bubble up so it
     * can synthesize a failing-test event and move to the next file. Arming
     * a panic-restart here would reboot the device mid-manifest on the
     * first async rejection. */
    if (mik_rt->test_mode) {
        return;
    }
    /* Defer the restart so the host can still issue deploy/clean/--recover
     * commands during the grace window. The actual platform->restart()
     * fires from MIK_Loop once the deadline elapses. */
    if (mik_rt->restart_at_us == 0) {
        const MIKPlatform* platform = MIK_GetPlatform();
        mik_rt->restart_at_us =
            platform->get_boot_us() + (int64_t)mik_rt->config.panic_restart_delay_ms * 1000;
        /* Say what comes next: with deep sleep the device goes silent. */
        if (mik_rt->config.panic_mode == MIK_PANIC_DEEP_SLEEP && platform->deep_sleep_us) {
            mik__print_error_line("[panic] deep-sleeping for %d ms in %d ms",
                                  mik_rt->config.panic_sleep_duration_ms,
                                  mik_rt->config.panic_restart_delay_ms);
        } else {
            mik__print_error_line("[panic] restarting in %d ms",
                                  mik_rt->config.panic_restart_delay_ms);
        }
    }
}

int mik__load_file(JSContext* ctx, DynBuf* dbuf, const char* filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    /* Reserve the exact file size up front: DynBuf grows 1.5x from zero, so
     * letting it grow incrementally peaks at ~2.5x the file size mid-copy.
     * +4 covers the NUL/wrapper suffix callers append after loading. */
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        if (dbuf_claim(dbuf, (size_t)st.st_size + 4)) {
            close(fd);
            return -1;
        }
    }

    uint8_t buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (dbuf_put(dbuf, buf, n)) {
            close(fd);
            return -1;
        }
    }

    if (n < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

void mik__resolve_fs_path(JSContext* ctx, const char* module_name, char* out, size_t out_size) {
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (mik_rt && mik_rt->fs_base_path) {
        snprintf(out, out_size, "%s%s", mik_rt->fs_base_path, module_name);
    } else {
        snprintf(out, out_size, "%s", module_name);
    }
}

/* Minimum QuickJS heap budget that should be available after the module
 * graph is fully compiled, before entering the evaluation phase. Top-level
 * code in each module typically allocates closures, atoms, and any
 * eagerly-constructed data structures, so the eval phase needs its own
 * headroom separate from what the compiled bytecode already consumed.
 *
 * Falling below this threshold is the #1 cause of `Error: null` failures —
 * QuickJS OOMs partway through eval, and the recursive-OOM guard in
 * JS_ThrowOutOfMemory leaves the exception slot holding whatever garbage
 * was there. Emitting a proactive warning here gives the user a fighting
 * chance to diagnose it before they see the cryptic symptom.
 *
 * 16 KB is a deliberate middle-ground: large enough that normal apps won't
 * trigger it, small enough that any realistic app running this close to
 * the ceiling IS actually at risk. */
#define MIK__PRE_EVAL_HEADROOM_WARN 16384

static void mik__check_pre_eval_headroom(JSContext* ctx, const char* filename) {
    JSMemoryUsage mem;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mem);
    if (mem.malloc_limit <= 0) return; /* No limit configured — nothing to warn about. */

    long headroom = (long)mem.malloc_limit - (long)mem.malloc_size;
    /* The malloc limit is fixed at runtime creation, but the physical heap
     * moves underneath it — the real budget is whichever ceiling is nearer.
     * 0 means the platform can't report system memory (desktop builds). */
    const MIKPlatform* platform = MIK_GetPlatform();
    size_t sys_free = platform->get_free_system_mem();
    bool sys_bound = sys_free > 0 && (long)sys_free < headroom;
    if (sys_bound) {
        headroom = (long)sys_free;
    }
    if (headroom >= MIK__PRE_EVAL_HEADROOM_WARN) return;

    if (sys_bound) {
        platform->log(MIK_LOG_WARN, "mikrojs",
                      "Low system heap before evaluating '%s': %ld bytes free "
                      "(QuickJS %ld / %ld used has room, but native subsystems have "
                      "overrun the reserve). Module evaluation may fail with OOM. Raise "
                      "`memReserved` in mikro.config.ts or free native resources.",
                      filename, headroom, (long)mem.malloc_size, (long)mem.malloc_limit);
    } else {
        platform->log(MIK_LOG_WARN, "mikrojs",
                      "Low QuickJS heap headroom before evaluating '%s': %ld bytes left "
                      "(%ld / %ld used). Module evaluation may fail with OOM. Lower "
                      "`memReserved` in mikro.config.ts or split the module graph with "
                      "dynamic imports.",
                      filename, headroom, (long)mem.malloc_size, (long)mem.malloc_limit);
    }

    /* Signal the host so it can take application-level action (flash LED,
     * log metric, trigger reset, forward to telemetry). Operates on C state
     * only, safe to call even though the JS heap is nearly exhausted. */
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (mik_rt != nullptr) {
        MIKOOMEvent event = {};
        event.phase = MIK_OOM_PRE_EVAL_WARN;
        event.filename = filename;
        event.malloc_size = (size_t)mem.malloc_size;
        event.malloc_limit = (size_t)mem.malloc_limit;
        event.headroom = headroom > 0 ? (size_t)headroom : 0;
        MIK_ReportOOM(mik_rt, &event);
    }
}

JSValue MIK_EvalModuleContent(JSContext* ctx, const char* filename, const char* content,
                              size_t len) {
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    char* pp_source = NULL;
    size_t pp_len = 0;
    if (mik_rt && mik_rt->preprocess_fn) {
        pp_source =
            mik_rt->preprocess_fn(filename, content, len, &pp_len, mik_rt->preprocess_opaque);
    }
    const char* eval_src = pp_source ? pp_source : content;
    size_t eval_len = pp_source ? pp_len : len;

    mik__blocking_begin(mik_rt);
    /* Compile then run to be able to set import.meta */
    JSValue ret =
        JS_Eval(ctx, eval_src, eval_len, filename, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(pp_source);
    if (!JS_IsException(ret)) {
        js_module_set_import_meta(ctx, ret, true, true);
        mik__check_pre_eval_headroom(ctx, filename);
        ret = JS_EvalFunction(ctx, ret);
    }
    return ret;
}

JSValue MIK_EvalScriptContent(JSContext* ctx, const char* content, size_t len) {
    mik__blocking_begin(MIK_GetRuntime(ctx));
    return JS_Eval(ctx, content, len, "<eval>", JS_EVAL_TYPE_GLOBAL);
}

JSValue MIK_EvalScript(JSContext* ctx, const char* filename) {
    DynBuf dbuf;
    size_t dbuf_size;
    int r;
    JSValue ret;

    mik_dbuf_init(ctx, &dbuf);
    r = mik__load_file(ctx, &dbuf, filename);
    if (r != 0) {
        dbuf_free(&dbuf);
        JS_ThrowReferenceError(ctx, "could not load '%s': %s", filename, strerror(errno));
        return JS_EXCEPTION;
    }

    dbuf_size = dbuf.size;

    /* Add null termination, required by JS_Eval. */
    dbuf_putc(&dbuf, '\0');

    mik__blocking_begin(MIK_GetRuntime(ctx));
    ret = JS_Eval(ctx, (char*)dbuf.buf, dbuf_size - 1, filename, JS_EVAL_TYPE_GLOBAL);

    dbuf_free(&dbuf);
    return ret;
}

JSValue MIK_EvalModule(JSContext* ctx, const char* filename, bool is_main) {
    DynBuf dbuf;
    size_t dbuf_size;
    int r;
    JSValue ret;

    /* Resolve logical module name to filesystem path */
    char fs_path[PATH_MAX];
    mik__resolve_fs_path(ctx, filename, fs_path, sizeof(fs_path));

    /* Prefer pre-compiled bytecode (.bjs) over source (.js), mirroring
     * the same fallback in mik_module_loader_inner. This lets callers
     * always pass .js names; the profile then records consistent .js
     * names for both the entry and its imports. */
    bool is_bjs = js__has_suffix(filename, ".bjs");
    if (!is_bjs && js__has_suffix(filename, ".js")) {
        char bjs_path[PATH_MAX];
        size_t len = strlen(fs_path);
        memcpy(bjs_path, fs_path, len - 3);
        strcpy(bjs_path + len - 3, ".bjs");
        struct stat st;
        if (stat(bjs_path, &st) == 0) {
            memcpy(fs_path, bjs_path, strlen(bjs_path) + 1);
            is_bjs = true;
        }
    }

    mik_dbuf_init(ctx, &dbuf);
    r = mik__load_file(ctx, &dbuf, fs_path);
    if (r != 0) {
        dbuf_free(&dbuf);
        JS_ThrowReferenceError(ctx, "could not load '%s': %s", filename, strerror(errno));

        return JS_EXCEPTION;
    }

    dbuf_size = dbuf.size;

    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    mik__blocking_begin(mik_rt);
    bool profile = mik_rt != nullptr && mik_rt->profile_enabled;
    int64_t malloc_before = 0;
    size_t entries_before = 0;
    if (profile) {
        JSMemoryUsage before;
        JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &before);
        malloc_before = before.malloc_size;
        entries_before = mik_rt->profile_entries.size();
    }

    /* Pre-compiled bytecode (.bjs) — deserialize directly */
    if (is_bjs) {
        ret = JS_ReadObject(ctx, dbuf.buf, dbuf_size, JS_READ_OBJ_BYTECODE);
        dbuf_free(&dbuf);
        if (JS_IsException(ret)) {
            return ret;
        }
        if (JS_ResolveModule(ctx, ret) < 0) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        js_module_set_import_meta(ctx, ret, true, is_main);

        /* Record the entry's load cost before JS_EvalFunction. The entry
         * bypasses the module loader, so it needs its own profile entry.
         * JS_ResolveModule above triggered the loader for all imports,
         * which are profiled as children and subtracted. */
        if (profile) {
            mik__record_entry_profile(mik_rt, JS_GetRuntime(ctx), filename, malloc_before,
                                      entries_before);
        }

        mik__check_pre_eval_headroom(ctx, filename);
        return JS_EvalFunction(ctx, ret);
    }

    /* Source module — compile, resolve, evaluate.
     * JS_Eval with COMPILE_ONLY + JS_ResolveModule triggers the module
     * loader for imports (profiled as children). Record the entry's own
     * load cost, then call JS_EvalFunction separately. */
    dbuf_putc(&dbuf, '\0');
    {
        const char* src = (char*)dbuf.buf;
        size_t src_len = dbuf_size - 1;
        char* pp_source = NULL;
        size_t pp_len = 0;
        if (mik_rt && mik_rt->preprocess_fn) {
            pp_source = mik_rt->preprocess_fn(filename, src, src_len, &pp_len,
                                              mik_rt->preprocess_opaque);
        }
        const char* eval_src = pp_source ? pp_source : src;
        size_t eval_len = pp_source ? pp_len : src_len;

        ret = JS_Eval(ctx, eval_src, eval_len, filename,
                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        free(pp_source);
        dbuf_free(&dbuf);

        if (JS_IsException(ret)) {
            return ret;
        }
        js_module_set_import_meta(ctx, ret, true, is_main);

        if (profile) {
            mik__record_entry_profile(mik_rt, JS_GetRuntime(ctx), filename, malloc_before,
                                      entries_before);
        }

        mik__check_pre_eval_headroom(ctx, filename);
        return JS_EvalFunction(ctx, ret);
    }
}

int MIK_RunEntry(MIKRuntime* mik_rt, const char* entry) {
    return MIK_RunEntryErr(mik_rt, entry, NULL, 0);
}

int MIK_RunEntryErr(MIKRuntime* mik_rt, const char* entry, char* err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) {
        err_buf[0] = '\0';
    }
    if (!mik_rt || !entry || entry[0] == '\0') {
        return -EINVAL;
    }

    JSContext* ctx = MIK_GetJSContext(mik_rt);

    char fs_path[PATH_MAX];
    mik__resolve_fs_path(ctx, entry, fs_path, sizeof(fs_path));

    struct stat st;
    bool found = stat(fs_path, &st) == 0;
    if (!found && js__has_suffix(entry, ".js")) {
        char bjs_path[PATH_MAX];
        size_t len = strlen(fs_path);
        if (len >= 3 && len - 3 + sizeof(".bjs") <= sizeof(bjs_path)) {
            memcpy(bjs_path, fs_path, len - 3);
            memcpy(bjs_path + len - 3, ".bjs", sizeof(".bjs"));
            found = stat(bjs_path, &st) == 0;
        }
    }
    if (!found) {
        return -ENOENT;
    }

    JSValue result = MIK_EvalModule(ctx, entry, true);
    bool failed = JS_IsException(result);
    bool rejected = false;
    /* Modules with top-level await return a Promise. A module body that
     * throws synchronously rejects that promise before JS_EvalFunction
     * returns, but the value itself is still an Object (rejected Promise),
     * not a JS_EXCEPTION sentinel. Treat sync-rejected as an eval failure
     * so callers (e.g. the test supervisor) don't mistake a fatal module
     * for a successful launch and wait for events that will never come.
     * Pending promises (real TLA awaiting something) continue as normal. */
    if (!failed && JS_IsObject(result)) {
        JSPromiseStateEnum state = JS_PromiseState(ctx, result);
        if (state == JS_PROMISE_REJECTED) {
            failed = true;
            rejected = true;
        }
    }
    if (failed) {
        JSValue exc = rejected ? JS_PromiseResult(ctx, result) : JS_GetException(ctx);
        if (err_buf && err_buf_size > 0) {
            /* Capture the failure's string form for the caller. */
            const char* msg = JS_ToCString(ctx, exc);
            if (msg) {
                snprintf(err_buf, err_buf_size, "%s", msg);
                JS_FreeCString(ctx, msg);
            } else {
                /* Stringifying the exception itself threw (e.g. OOM) — drop
                 * the secondary exception so it can't leak into later evals. */
                JSValue stray = JS_GetException(ctx);
                JS_FreeValue(ctx, stray);
            }
        }
        /* A synchronous throw (a missing import, a syntax error) exists only
         * as the pending exception consumed above, so report it here the way
         * MIK_Loop reports one it finds pending: handler, dump, stop. Left
         * unreported, the device idles in silence. A rejected eval promise is
         * still queued for the loop's rejection flush, which reports it the
         * same way. */
        if (!rejected) {
            if (mik_rt->error_handler_fn) {
                mik_rt->error_handler_fn(ctx, exc, mik_rt->error_handler_opaque);
            }
            mik_dump_error1(ctx, exc);
            MIK_Stop(mik_rt);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    return failed ? -EFAULT : 0;
}
