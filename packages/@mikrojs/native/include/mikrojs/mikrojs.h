#pragma once
#include <quickjs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MIKRuntime MIKRuntime;

typedef struct MIKRunOptions {
    int mem_limit;
    size_t stack_size;
    /* Allocate the QuickJS heap from PSRAM instead of internal SRAM on
     * chips with both. Frees ~150-250 KB of contiguous internal SRAM for
     * mbedTLS, WiFi/BLE, and DMA buffers, the dominant root cause of
     * HTTPS handshake failures (error 0x7002) on apps with significant
     * retained JS state. JS execution is somewhat slower (PSRAM has
     * higher access latency than internal SRAM); for typical embedded
     * IO-bound workloads this is invisible against networking and IO
     * costs. No effect on hosts and chips without PSRAM. Defaults false
     * for backward compatibility; the firmware bootstrap flips it on
     * when CONFIG_MIKROJS_QUICKJS_HEAP_PSRAM is set. */
    bool use_psram_heap;
} MIKRunOptions;

/* File-log flush policies. Mirror of `LogFlush` in the host-side TS type. */
typedef enum MIKLogFlush {
    MIK_LOG_FLUSH_ERROR = 0, /* default: buffer, flush on warn/error or buffer full */
    MIK_LOG_FLUSH_LINE = 1,
} MIKLogFlush;

/* What the device does after an uncaught exception (a "panic"). Mirror of
 * the `onPanic.mode` field in the host-side TS config. */
typedef enum MIKPanicMode {
    MIK_PANIC_RESTART = 0,    /* default: reboot after the grace window */
    MIK_PANIC_DEEP_SLEEP = 1, /* deep-sleep panic_sleep_duration_ms; wake reboots */
} MIKPanicMode;

typedef struct MIKConfig {
    /* Grace window after a panic, awake and reachable, before the action
     * fires (applies to both modes). */
    int panic_restart_delay_ms;
    MIKPanicMode panic_mode;
    /* Deep-sleep length for MIK_PANIC_DEEP_SLEEP; ignored otherwise. */
    int panic_sleep_duration_ms;
    size_t stack_size;
    uint32_t mem_reserved;
    uint32_t fs_read_max; /* 0 = keep runtime default (65536) */
    char entry_point[128];
    char wifi_country[3];   /* Two-letter country code + NUL, e.g. "NO" */
    char wifi_hostname[64]; /* DHCP hostname; empty = use mikrojs-<device-id> default */
    /* File-log config. log_dir empty disables file logging; otherwise the
     * file lives at "<log_dir>/log.txt" (rotated as ".../log.txt.1"). */
    char log_dir[64];
    uint32_t log_max_size;
    MIKLogFlush log_flush;
    /* Watchdog budgets in ms, 0 = disabled. Mirror of the `watchdog` key in
     * the host-side TS config. blocking: one event-loop turn holding the
     * loop; feed: gap between watchdog.feed() calls; awake: time since boot. */
    int blocking_timeout_ms;
    int feed_timeout_ms;
    int awake_timeout_ms;
} MIKConfig;

/* Shipped blocking default. The ESP32 side static_asserts this against the
 * hardware TWDT timeout so the two cannot drift out of order. */
#define MIK_WATCHDOG_BLOCKING_DEFAULT_MS 30000

void MIK_DefaultConfig(MIKConfig* config);
int MIK_LoadConfig(const char* base_path, MIKConfig* config);
void MIK_SetConfig(MIKRuntime* mik_rt, const MIKConfig* config);

/* Load the "tests" string array from package.json at `base_path`, applying
 * the same prefix as MIK_LoadConfig so paths are resolved relative to the
 * app directory. Presence of this array is the supervisor's sole "test
 * mode" signal — normal deploys never write it.
 * Returns 0 on success (including when the array is absent or empty),
 * nonzero on malloc failure. On success, `*out_paths` is NULL when the
 * array was absent, otherwise a malloc'd array of `*out_count` malloc'd
 * strings that the caller frees via MIK_FreeTests. */
int MIK_LoadTests(const char* base_path, char*** out_paths, size_t* out_count);
void MIK_FreeTests(char** paths, size_t count);

void MIK_DefaultOptions(MIKRunOptions* options);
MIKRuntime* MIK_NewRuntime(void);
MIKRuntime* MIK_NewRuntimeOptions(MIKRunOptions* options);
void MIK_FreeRuntime(MIKRuntime* mik_rt);

/* Evaluate `entry` as this runtime's main module. Probes the filesystem
 * (under the configured fs_base_path) for `entry` and, if the entry is a
 * `.js` path, also for the adjacent `.bjs` bytecode variant. Returns:
 *   0         success
 *   -EINVAL   runtime or entry is null/empty
 *   -ENOENT   entry (and its .bjs variant) not on disk
 *   -EFAULT   module evaluation threw
 * A synchronous throw (a missing import, a syntax error) is reported like an
 * uncaught exception found by MIK_Loop: error handler, dump, MIK_Stop. A
 * rejected eval promise is reported by the next loop's rejection flush. */
int MIK_RunEntry(MIKRuntime* mik_rt, const char* entry);

/* MIK_RunEntry variant that, on -EFAULT, copies the thrown exception's
 * string form (or the sync-rejected promise's rejection value) into
 * err_buf so callers can surface the real failure instead of a generic
 * "evaluation threw". err_buf may be NULL; when given it is always
 * NUL-terminated (empty when no message could be captured). */
int MIK_RunEntryErr(MIKRuntime* mik_rt, const char* entry, char* err_buf, size_t err_buf_size);

JSContext* MIK_GetJSContext(MIKRuntime* mik_rt);
MIKRuntime* MIK_GetRuntime(JSContext* ctx);
void MIK_SetFSBasePath(MIKRuntime* mik_rt, const char* base_path);
void MIK_SetFSRoot(MIKRuntime* mik_rt, const char* fs_root);
void MIK_SetFSLimit(MIKRuntime* mik_rt, size_t limit);
/* Maximum size (bytes) for a single readFile() call. Larger files throw
 * FSError with code EFBIG — callers use open() for streaming. 0 resets to
 * the default (65536). */
void MIK_SetFSReadMax(MIKRuntime* mik_rt, size_t bytes);
int MIK_Loop(MIKRuntime* mik_rt);
void MIK_Stop(MIKRuntime* mik_rt);

/* True once the runtime has entered a halted state — set by the promise
 * rejection tracker when an unhandled rejection is observed with no
 * host dispatcher to cancel it. MIK_Loop() returns 1 immediately in
 * this state and no further pumping happens. The supervisor uses this
 * to detect that a per-test runtime has crashed and move to the next. */
bool MIK_IsStopRequested(MIKRuntime* mik_rt);

/* Environment variables — call MIK_SetEnvVar() before MIK_RebuildEnv(),
 * or before any module evaluation if env vars need to be available. */
void MIK_SetEnvVar(MIKRuntime* mik_rt, const char* key, const char* value);
void MIK_RebuildEnv(MIKRuntime* mik_rt);

/* Native module registration */
typedef void (*MIKNativeModuleInitFn)(JSContext* ctx);
void MIK_RegisterNativeModuleInit(MIKRuntime* mik_rt, const char* name,
                                  MIKNativeModuleInitFn init_fn);

/* Loop consumer registration */
typedef void (*MIKLoopConsumeFn)(JSContext* ctx);
typedef void (*MIKLoopDestroyFn)(JSContext* ctx);
void MIK_RegisterLoopConsumer(MIKRuntime* mik_rt, MIKLoopConsumeFn consume_fn,
                              MIKLoopDestroyFn destroy_fn);

/* Virtual module registration — allows registering JS source as named modules,
 * checked by the module loader before the builtin bytecode table. */
void MIK_RegisterVirtualModule(MIKRuntime* mik_rt, const char* name, const char* source,
                               size_t source_len);

/* Error handler — called with the exception JSValue before it is consumed.
 * The handler receives a borrowed JSValue (do NOT free it). */
typedef void (*MIKErrorHandlerFn)(JSContext* ctx, JSValue error, void* opaque);
void MIK_SetErrorHandler(MIKRuntime* mik_rt, MIKErrorHandlerFn fn, void* opaque);

/* Module source preprocessor — called on source files before compilation.
 * The callback receives filename, source, and source_len. It should return
 * a malloc'd string and set *out_len, or return NULL to use the original source.
 * The returned buffer must be null-terminated (JS_Eval reads buf[len]) and
 * *out_len excludes the terminator. The caller frees it with free(). */
typedef char* (*MIKPreprocessFn)(const char* filename, const char* source, size_t source_len,
                                 size_t* out_len, void* opaque);
void MIK_SetPreprocessor(MIKRuntime* mik_rt, MIKPreprocessFn fn, void* opaque);

/* Install the two test-helper globals on a runtime:
 *   __testEmit(jsonStr)    — sends MSG_TEST TLV frames to the host
 *   __testFileDone()       — signals MIK_ProtocolExit so a supervisor can
 *                             tear down this runtime and move to the next
 *
 * Must be called before user code runs (ordinary runtimes never see these
 * globals). The test-runner supervisor calls this per-file in test mode;
 * the mikrojs/test built-in falls back to a console.log path when absent. */
void MIK_EnableTestHelpers(MIKRuntime* mik_rt);

/* Optional handler called by __testEmit when the runtime is NOT in protocol
 * mode (i.e. the firmware UART/USJ wire path is inactive). The handler
 * receives the JSON event payload and is responsible for routing it to
 * whatever channel the host expects. Used by the Node addon to forward
 * MSG_TEST events to the host bridge so `mikro sim test` can deliver them
 * over its own transport without going through the wire protocol. */
typedef void (*MIKTestEmitHandlerFn)(const char* json, size_t len, void* opaque);
void MIK_SetTestEmitHandler(MIKRuntime* mik_rt, MIKTestEmitHandlerFn fn, void* opaque);

/* Memory profiling — records per-module QuickJS heap growth. Storage lives
 * outside the tracked runtime heap so instrumentation does not pollute the
 * numbers being measured. Enable before loading user code; read entries at
 * shutdown. */
#define MIK_PROFILE_NAME_MAX 96

typedef struct MIKProfileEntry {
    char name[MIK_PROFILE_NAME_MAX]; /* Module name, truncated if longer */
    size_t delta_bytes;               /* QuickJS malloc_size delta across load */
    size_t order;                     /* Load order, starting at 0 */
} MIKProfileEntry;

void MIK_EnableProfiling(MIKRuntime* mik_rt);
size_t MIK_GetProfileEntryCount(MIKRuntime* mik_rt);
const MIKProfileEntry* MIK_GetProfileEntries(MIKRuntime* mik_rt);
size_t MIK_GetProfileBaseline(MIKRuntime* mik_rt);

/* OOM signalling — reports to the host when QuickJS's memory budget is
 * at or past the point where module evaluation is likely to fail, OR after
 * the fact when a null/primitive exception correlates with low headroom
 * (which is almost always a recursive-OOM where QuickJS couldn't even
 * allocate an Error object to throw).
 *
 * Two access patterns are supported:
 *
 *   1. Polling via `MIK_ConsumeOOMFlag()`: cheap "did OOM happen recently?"
 *      boolean check. Automatically clears the flag on read, so each call
 *      returns true at most once per event. Useful for host code that
 *      checks after each `MIK_EvalModule` / `MIK_Loop` iteration.
 *
 *   2. Async callback via `MIK_SetOOMHandler()`: fires synchronously at
 *      the detection point with a struct carrying the filename and heap
 *      stats. Useful for host code that wants to react immediately (log
 *      to a file, flash an LED, trigger a reset, forward to a telemetry
 *      channel).
 *
 * Both mechanisms operate entirely on C-level state and a C function
 * pointer — zero QuickJS allocations, safe to use when the JS heap is
 * exhausted. Hot paths are unaffected: the detection points are
 * boot/failure paths that already existed, so the added flag-set and
 * conditional callback invocation are in cold code. */
enum {
    MIK_OOM_PRE_EVAL_WARN = 1,      /* headroom crossed low-water mark before eval */
    MIK_OOM_POST_EVAL_DETECTED = 2, /* null/primitive exception + low headroom after eval */
};

typedef struct MIKOOMEvent {
    int phase;              /* one of MIK_OOM_* above */
    const char* filename;   /* module being evaluated (borrowed, not owned) */
    size_t malloc_size;     /* QuickJS heap used */
    size_t malloc_limit;    /* QuickJS heap soft cap */
    size_t headroom;        /* malloc_limit - malloc_size (clamped at 0) */
} MIKOOMEvent;

typedef void (*MIKOOMHandlerFn)(const MIKOOMEvent* event, void* opaque);
void MIK_SetOOMHandler(MIKRuntime* mik_rt, MIKOOMHandlerFn fn, void* opaque);

/* Returns true at most once per OOM event: the flag is cleared on read.
 * Callers should poll this between operations to catch events missed by
 * the synchronous handler path. */
bool MIK_ConsumeOOMFlag(MIKRuntime* mik_rt);

/* Internal: fires the handler and sets the flag. Called from detection
 * points inside libmikrojs and from addons that do their own detection
 * (e.g. the Node addon's post-eval null-exception reporter). */
void MIK_ReportOOM(MIKRuntime* mik_rt, const MIKOOMEvent* event);

/* Per-module opaque data slots (for platform-specific modules to store state).
 *
 * A slot index identifies one module for the lifetime of the process and means
 * the same thing in every runtime, so reserve it once and cache it:
 *
 *     static int my_slot = -1;
 *     if (my_slot < 0) my_slot = MIK_ReserveModuleSlot();
 *
 * Reserving per runtime instead would hand the same index to two modules as
 * soon as a second runtime initialized a different set of them, and each
 * would then read the other's state through the wrong type. */
#define MIK_MODULE_DATA_SLOTS 16
void MIK_SetModuleData(MIKRuntime* mik_rt, int slot, void* data);
void* MIK_GetModuleData(MIKRuntime* mik_rt, int slot);
int MIK_ReserveModuleSlot(void);

/* Self-registration for native modules (ESP32, board, driver modules).
 * Modules call MIK_REGISTER_MODULE() at file scope. The module loader
 * discovers registered modules and initializes them lazily on first import. */
typedef JSModuleDef* (*MIKModuleInitFn)(JSContext* ctx);

typedef struct mik_module_desc_t {
    const char* name;         /* module name, e.g. "native:mikro/pin" */
    MIKModuleInitFn init;     /* called on first import; returns the JSModuleDef */
    MIKLoopConsumeFn consume; /* event loop consumer (nullable) */
    MIKLoopDestroyFn destroy; /* cleanup on shutdown (nullable) */
    struct mik_module_desc_t* next;
} mik_module_desc_t;

extern mik_module_desc_t* mik__module_registry_head;

/* NOLINTBEGIN(bugprone-macro-parentheses,cppcoreguidelines-avoid-non-const-global-variables) */
/* Namespace governance. The build defines MIK_PACKAGE_NAME for every component:
 * "mikro" for the core runtime, and the owning npm package name for a discovered
 * driver/board package (assigned authoritatively from the dependency graph, not
 * self-declared). A native module must then be named "native:<MIK_PACKAGE_NAME>/…"
 * and a registered builtin "<MIK_PACKAGE_NAME>/…". This reserves the native:
 * namespace to mikro and stops a package from claiming or shadowing another's
 * name. Enforced at compile time; the static_assert needs string-literal args
 * (always the case here). */
#ifdef MIK_PACKAGE_NAME
#define MIK__REQUIRE_PREFIX(name_, prefix_)                                        \
    static_assert(__builtin_strncmp((name_), (prefix_), sizeof(prefix_) - 1) == 0, \
                  "mikrojs: name must start with \"" prefix_ "\"")
#define MIK__REQUIRE_NATIVE_NS(name_) MIK__REQUIRE_PREFIX(name_, "native:" MIK_PACKAGE_NAME "/")
#define MIK__REQUIRE_BUILTIN_NS(name_) MIK__REQUIRE_PREFIX(name_, MIK_PACKAGE_NAME "/")
#else
#define MIK__REQUIRE_NATIVE_NS(name_) static_assert(true, "")
#define MIK__REQUIRE_BUILTIN_NS(name_) static_assert(true, "")
#endif

/* The descriptor has external linkage so the linker can be told to keep it
 * via -u flags (static symbols in static libraries get stripped). */
#define MIK_REGISTER_MODULE(id, name_, init_, consume_, destroy_)              \
    MIK__REQUIRE_NATIVE_NS(name_);                                             \
    mik_module_desc_t mik__mod_desc_##id = {                                   \
        name_, init_, consume_, destroy_, NULL};                               \
    static void __attribute__((constructor)) mik__register_mod_##id(void) {    \
        mik__mod_desc_##id.next = mik__module_registry_head;                   \
        mik__module_registry_head = &mik__mod_desc_##id;                       \
    }
/* Self-registration for bytecode builtins (board/driver JS runtime modules).
 * Driver packages use MIK_REGISTER_BUILTIN() to embed their JS wrappers as
 * firmware builtins, resolved by the real npm package name (e.g. "@mikrojs/your-driver"). */
typedef struct mik_ext_builtin_t {
    const char* name;         /* module name, e.g. "@mikrojs/your-driver" */
    const uint8_t* data;      /* compiled bytecode */
    uint32_t data_size;
    struct mik_ext_builtin_t* next;
} mik_ext_builtin_t;

extern mik_ext_builtin_t* mik__ext_builtin_head;

/* The descriptor has external linkage so the linker can keep it via -u flags. */
#define MIK_REGISTER_BUILTIN(id, name_, data_, size_)                              \
    MIK__REQUIRE_BUILTIN_NS(name_);                                                \
    mik_ext_builtin_t mik__builtin_##id = {                                        \
        name_, data_, size_, NULL};                                                \
    static void __attribute__((constructor)) mik__register_builtin_##id(void) {    \
        mik__builtin_##id.next = mik__ext_builtin_head;                            \
        mik__ext_builtin_head = &mik__builtin_##id;                                \
    }
/* NOLINTEND(bugprone-macro-parentheses,cppcoreguidelines-avoid-non-const-global-variables) */

/* REPL (mik_repl.cpp) */
bool MIK_IsReplActive(void);

/* Protocol transport abstraction */
typedef struct MIKReplTransport MIKReplTransport;

/**
 * Command handler callback for platform-specific commands (deploy, config).
 * Called by the protocol loop when it receives a command type it doesn't
 * handle itself (i.e. anything outside the REPL command range 0x10-0x13).
 *
 * The handler receives the TLV header (type + payload_len) but the payload
 * bytes have NOT been read yet. The handler must read exactly payload_len
 * bytes from the transport using mik__proto_read_exact() before returning.
 *
 * Returns true if the command was handled, false to ignore it.
 */
typedef bool (*MIKProtoCommandHandler)(MIKReplTransport* transport, uint8_t cmd_type,
                                       uint32_t payload_len, void* ctx);

/**
 * Session teardown hook. Called once from the protocol loop when it
 * exits (transport error, client disconnect, or MIK_CMD_EXIT). Platform
 * handlers use this to reset any per-session state — e.g. a partially
 * staged deploy — so the next session starts clean.
 */
typedef void (*MIKProtoSessionEnd)(void* ctx);

struct MIKReplTransport {
    int (*read)(uint8_t* buf, size_t size, void* ctx);     /* Non-blocking read */
    void (*write)(const void* buf, size_t len, void* ctx);  /* Write */
    void* ctx;                                               /* Opaque context */
    const char* chip_name;                                   /* e.g. "esp32c6" (optional) */
    MIKProtoCommandHandler command_handler;                   /* Platform command handler */
    void* command_handler_ctx;                                /* Handler context */
    MIKProtoSessionEnd session_end;                           /* Called on loop exit (optional) */
    void* session_end_ctx;                                    /* session_end context */
};

/* Protocol transport helpers — blocking reads with event loop pumping.
 * Used by command handlers to read payload bytes from the transport. */
bool mik__proto_read_exact(MIKReplTransport* transport, void* buf, size_t n);
bool mik__proto_drain(MIKReplTransport* transport, size_t n);

/* Send a TLV-framed message */
void mik__proto_send(MIKReplTransport* transport, uint8_t type, const void* data, size_t len);
void mik__proto_send_ok(MIKReplTransport* transport);
void mik__proto_send_err(MIKReplTransport* transport, const char* msg);

/* Tap called by mik__repl_proto_send_output with the raw body bytes (no
 * TLV framing) before they're sent on the wire. Single slot — owned by
 * the file logger on the device side. msg_type is the MIK_MSG_* value so
 * the tap can pick the appropriate log level. */
typedef void (*MIKLogEmitFn)(uint8_t msg_type, const void* data, size_t len);
void mik__set_log_emit_tap(MIKLogEmitFn fn);

/* True while mik__proto_send is writing TLV bytes to the transport.
 * Platform-side transports (mik__console_write on the ESP32) check this
 * to skip any installed wire-byte taps that would otherwise capture the
 * framing in addition to the body. Single-task code path, so plain bool
 * is sufficient — there's no other writer of this flag. */
extern bool mik__proto_send_in_progress;

/* ── Session primitives ─────────────────────────────────────────────
 * A protocol session is scoped to a transport (one client connection).
 * Runtimes are attached/detached within the session; a supervisor can
 * swap runtimes between serve loops without closing the transport or
 * repeating the MSG_READY handshake. */

/* Open a protocol session on `transport`. Sends MSG_READY, enables TLV
 * framing for console/stdio output. No-op if a session is already open. */
void MIK_ProtocolOpen(MIKReplTransport* transport);

/* Close the session. Fires the transport's session_end hook (used for
 * deploy state cleanup), then clears all session state. Safe to call
 * from any state. */
void MIK_ProtocolClose(void);

/* Bind a runtime to the active session. Required before serving any
 * commands that evaluate JS. The caller retains ownership of the runtime
 * and must detach before freeing. */
void MIK_ProtocolAttach(MIKRuntime* mik_rt);

/* Record what `mik_rt` was handed at boot, reported on MSG_READY. Call once,
 * from the boot path, at the point the app's entry is about to be evaluated:
 * the figures then describe the floor an app starts from rather than whatever
 * happens to be free when a client connects. Later calls are ignored, so a
 * supervisor that swaps runtimes can't overwrite the boot reading with a
 * per-test one. MIK_ProtocolAttach calls this itself, which covers embedders
 * that never call it; a boot path with a test supervisor should call it
 * explicitly, before the supervisor allocates, so both modes report the same
 * figure. */
void MIK_CaptureBootMemory(MIKRuntime* mik_rt);

/* Unbind the current runtime. Call before MIK_FreeRuntime when swapping
 * runtimes mid-session. The session remains open. */
void MIK_ProtocolDetach(void);

/* Run the protocol loop until: (a) transport error, (b) CMD_EXIT from
 * client, or (c) MIK_ProtocolExit() is called from elsewhere (e.g. a
 * supervisor responding to a JS-side event). Handshake state persists
 * across invocations, so the MSG_READY ping dance only happens once
 * per session even if the serve loop is entered repeatedly. */
void MIK_ProtocolServeLoop(void);

/* Signal the active serve loop to return after the current iteration.
 * Does not close the session — the caller can re-attach a new runtime
 * and call MIK_ProtocolServeLoop() again. */
void MIK_ProtocolExit(void);

#ifdef __cplusplus
}
#endif

JSValue MIK_EvalScriptContent(JSContext* ctx, const char* content, size_t len);
JSValue MIK_EvalModule(JSContext* ctx, const char* filename, bool is_main);
JSValue MIK_EvalModuleContent(JSContext* ctx, const char* filename, const char* content,
                              size_t len);
