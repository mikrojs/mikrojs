#include <cerrno>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include <cinttypes>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mikrojs.h"
#include "mikrojs/platform.h"
#include "mikrojs/private.h"
#include "nvs_flash.h"

static const char* TAG = "mikrojs";

#if CONFIG_ESP_TASK_WDT_INIT
/* Ordering invariant: the JS blocking watchdog interrupts (with a stack trace)
 * before the hardware TWDT resets the chip. The margin covers unwinding and
 * printing the trace. onPanic.delay is not part of the sum: MIK_Loop feeds
 * the TWDT through the grace window. CONFIG_ESP_TASK_WDT_TIMEOUT_S only
 * exists under CONFIG_ESP_TASK_WDT_INIT. */
#define MIK_TWDT_MARGIN_S 5
static_assert(MIK_WATCHDOG_BLOCKING_DEFAULT_MS / 1000 + MIK_TWDT_MARGIN_S <
                  CONFIG_ESP_TASK_WDT_TIMEOUT_S,
              "watchdog.blocking default (30 s) must fire before the task watchdog: set "
              "CONFIG_ESP_TASK_WDT_TIMEOUT_S=60 in sdkconfig, or delete sdkconfig and re-run "
              "`idf.py set-target` so sdkconfig.defaults applies");
#endif

/* Error handler armed during a normal boot: when the app hits a fatal JS error
 * (uncaught exception or unhandled rejection) while running an OTA trial build,
 * flag the trial so the next reconcile reverts it. A JS error reboots as a clean
 * software reset, which the trial state machine can't otherwise tell from a
 * healthy restart. No-op outside a trial. */
static void ota_trial_error_handler(JSContext* ctx, JSValue error, void* /*opaque*/) {
    if (!mik__ota_in_trial()) {
        return;
    }
    char detail[160] = "uncaught error";
    if (JS_IsObject(error)) {
        JSValue name_v = JS_GetPropertyStr(ctx, error, "name");
        JSValue msg_v = JS_GetPropertyStr(ctx, error, "message");
        const char* name = JS_ToCString(ctx, name_v);
        const char* msg = JS_ToCString(ctx, msg_v);
        if (name && name[0] && msg && msg[0]) {
            snprintf(detail, sizeof(detail), "%s: %s", name, msg);
        } else if (msg && msg[0]) {
            snprintf(detail, sizeof(detail), "%s", msg);
        } else if (name && name[0]) {
            snprintf(detail, sizeof(detail), "%s", name);
        }
        /* A throwing getter (or a toString that throws) leaves an exception
         * pending on ctx that nothing downstream would ever clear. */
        if (!name || !msg) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        if (name) JS_FreeCString(ctx, name);
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, name_v);
        JS_FreeValue(ctx, msg_v);
    }
    mik__ota_note_trial_failure(detail);
}

/* Apply the MIK_LOG_LEVEL env var (if set in NVS) to our log tags.
 *
 * The firmware compiles in all levels up to DEBUG
 * (CONFIG_LOG_MAXIMUM_LEVEL=4) but keeps the runtime default at NONE so
 * the serial console stays clean in production. Setting
 * `MIK_LOG_LEVEL=debug` (or info/warn/error/verbose) via `mikro env set`
 * flips the listed tags to that level on the next boot.
 *
 * We target specific tags rather than "*" to avoid flooding the output
 * with unrelated ESP-IDF subsystems (WiFi, lwIP, driver framework etc.).
 * Add more tags here if new mikrojs-component modules are introduced. */
static void mik__apply_nvs_log_level(void) {
    nvs_handle_t handle;
    if (nvs_open("mik.env", NVS_READONLY, &handle) != ESP_OK) return;

    char buf[16];
    size_t buf_len = sizeof(buf);
    esp_err_t err = nvs_get_str(handle, "MIK_LOG_LEVEL", buf, &buf_len);
    nvs_close(handle);
    if (err != ESP_OK) return;

    esp_log_level_t level;
    if (strcasecmp(buf, "error") == 0) {
        level = ESP_LOG_ERROR;
    } else if (strcasecmp(buf, "warn") == 0 || strcasecmp(buf, "warning") == 0) {
        level = ESP_LOG_WARN;
    } else if (strcasecmp(buf, "info") == 0) {
        level = ESP_LOG_INFO;
    } else if (strcasecmp(buf, "debug") == 0) {
        level = ESP_LOG_DEBUG;
    } else if (strcasecmp(buf, "verbose") == 0 || strcasecmp(buf, "trace") == 0) {
        level = ESP_LOG_VERBOSE;
    } else {
        return; /* unknown value — leave defaults alone */
    }

    esp_log_level_set("mikrojs", level);
    esp_log_level_set("mik_repl", level);
    esp_log_level_set("mik_app_config", level);
}

/* Scratch buffers for the supervisor loop, heap-allocated for the duration
 * of a test-manifest run so normal boots don't carry them in BSS (~3 KB).
 *
 * Kept off the stack because the main task stack is ~24 KB and cannot
 * accommodate 1-2 KB of local buffers on top of the nested eval/module
 * normalizer call chain (observed as a stack-protection fault in
 * mik_module_normalizer on the first test file). Fixed sizes instead of
 * PATH_MAX scaling — on newlib PATH_MAX can be 4096, which is absurd for
 * our purposes. These sizes fit any realistic on-device test path. Only
 * one test manifest runs per boot, so single-ownership is safe. */
#define MIK_SUP_PATH_MAX 384
struct MIKSupScratch {
    char dbg[MIK_SUP_PATH_MAX + 64];
    char esc[MIK_SUP_PATH_MAX * 2 + 8];
    char buf[MIK_SUP_PATH_MAX * 2 + 512];
    /* Exception text captured by MIK_RunEntryErr when a test file fails to
     * evaluate, and its JSON-escaped form for the synthesized test event. */
    char err[192];
    char err_esc[sizeof(err) * 2 + 8];
};

/* Minimal JSON string-escape into a bounded buffer. Handles `"`, `\`, and
 * control characters; everything else copies verbatim. Returns bytes
 * written (excluding NUL) on success, or -1 on overflow. Used to synthesize
 * test-event JSON frames from raw C strings where the path may contain
 * characters that would otherwise corrupt the frame. */
static int mik__json_escape(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return -1;
    size_t w = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (w + 2 >= dst_size) return -1;
            dst[w++] = '\\';
            dst[w++] = (char)c;
        } else if (c < 0x20) {
            if (w + 7 >= dst_size) return -1;
            int n = snprintf(dst + w, dst_size - w, "\\u%04x", c);
            if (n < 0 || (size_t)n >= dst_size - w) return -1;
            w += (size_t)n;
        } else {
            if (w + 1 >= dst_size) return -1;
            dst[w++] = (char)c;
        }
    }
    if (w >= dst_size) return -1;
    dst[w] = '\0';
    return (int)w;
}

/* ── Serial transport ───────────────────────────────────────────── */

static int serial_transport_read(uint8_t* buf, size_t size, void* ctx) {
    (void)ctx;
    return mik__console_read(buf, size);
}

static void serial_transport_write(const void* buf, size_t len, void* ctx) {
    (void)ctx;
    mik__console_write(buf, len);
}

/* ── Platform command handler (deploy, config, restart) ─────────── */

/* Forward declarations for deploy and config handlers.
 * These receive the TLV header info but must read the payload bytes
 * themselves from the transport via mik__proto_read_exact(). */
bool mik__handle_deploy_command(MIKReplTransport* transport, uint8_t cmd_type,
                                uint32_t payload_len);
bool mik__handle_config_command(MIKReplTransport* transport, uint8_t cmd_type,
                                uint32_t payload_len);
bool mik__handle_fs_get(MIKReplTransport* transport, uint32_t payload_len);
void mik__deploy_session_reset(void);

static void platform_session_end(void* ctx) {
    (void)ctx;
    mik__deploy_session_reset();
    /* Safety net: if the CLI paused the runtime via CMD_RUNTIME_PAUSE and
     * disconnected before sending RESUME or RESTART, the app would stay
     * frozen until the next reboot. Always resume on session end. */
    mik__repl_set_paused(false);
}

static bool platform_command_handler(MIKReplTransport* transport, uint8_t cmd_type,
                                     uint32_t payload_len, void* ctx) {
    (void)ctx;

    /* Restart: drain payload (should be 0), then restart */
    if (cmd_type == MIK_CMD_RESTART) {
        mik__proto_drain(transport, payload_len);
        /* Say why: a silent reset is indistinguishable from a crash. */
        printf("[mikrojs] restart requested by host\n");
        fflush(stdout);
        esp_restart();
        return true; /* unreachable */
    }

    /* Runtime pause/resume: freeze the JS event loop so user code can't
     * interleave log output with TLV frames during a deploy. */
    if (cmd_type == MIK_CMD_RUNTIME_PAUSE || cmd_type == MIK_CMD_RUNTIME_RESUME) {
        mik__proto_drain(transport, payload_len);
        mik__repl_set_paused(cmd_type == MIK_CMD_RUNTIME_PAUSE);
        mik__proto_send_ok(transport);
        return true;
    }

    /* Deploy commands (0x20-0x27) plus the build-stage command (0x2D) */
    if ((cmd_type >= MIK_CMD_DEPLOY_PUT && cmd_type <= MIK_CMD_DEPLOY_CHECKSUM) ||
        cmd_type == MIK_CMD_DEPLOY_BUILD) {
        return mik__handle_deploy_command(transport, cmd_type, payload_len);
    }

    /* Install outcome (0x2E): report + clear the result of the last staged
     * build install. Handled here, not in the deploy handler: it is a
     * standalone read after the post-deploy reboot and must not start a
     * deploy session (pause the runtime, clear the staging dir). */
    if (cmd_type == MIK_CMD_DEPLOY_RESULT) {
        mik__proto_drain(transport, payload_len);
        MIKDeployResult res;
        mik__ota_take_deploy_result(&res);
        uint8_t buf[1 + 3 * 2 + sizeof(res.checksum) + sizeof(res.reason) + sizeof(res.detail)];
        size_t n = 0;
        buf[n++] = res.status;
        const char* fields[] = {res.checksum, res.reason, res.detail};
        for (const char* s : fields) {
            size_t len = strlen(s);
            buf[n++] = (uint8_t)(len & 0xFF);
            buf[n++] = (uint8_t)((len >> 8) & 0xFF);
            memcpy(buf + n, s, len);
            n += len;
        }
        mik__proto_send(transport, MIK_MSG_OK, buf, n);
        return true;
    }

    /* File pull (0x2B) */
    if (cmd_type == MIK_CMD_FS_GET) {
        return mik__handle_fs_get(transport, payload_len);
    }

    /* Log reset (0x2C): clear the on-device log files */
    if (cmd_type == MIK_CMD_LOG_RESET) {
        mik__proto_drain(transport, payload_len);
        mik_logfile_reset();
        mik__proto_send_ok(transport);
        return true;
    }

    /* Config + kv provisioning commands (0x40-0x44) */
    if (cmd_type >= MIK_CMD_CONFIG_LIST && cmd_type <= MIK_CMD_KV_DELETE) {
        return mik__handle_config_command(transport, cmd_type, payload_len);
    }

    /* Unknown command: drain payload to stay in sync */
    mik__proto_drain(transport, payload_len);
    return false;
}

/* ── LittleFS mount ─────────────────────────────────────────────── */

static esp_err_t mount_littlefs(const char* base_path, const char* partition_label) {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = base_path;
    conf.partition_label = partition_label;
    conf.format_if_mount_failed = true;

#if CONFIG_ESP_TASK_WDT_EN
    /* A first-boot format runs for seconds inside this one call, where no
     * feed can reach; unsubscribe the main task for its duration. */
    esp_task_wdt_delete(NULL);
#endif
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(NULL);
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS partition '%s' at '%s': %s", partition_label,
                 base_path, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Mounted LittleFS partition '%s' at '%s'", partition_label, base_path);
    }
    return ret;
}

/* ── Main entry point ───────────────────────────────────────────── */

void MIK_Main(void) {
    /* Detect whether USB Serial/JTAG or UART is connected and set up
     * the active console.  On chips with USB Serial/JTAG this checks
     * for USB SOF packets; if absent it falls back to UART. */
    mik__console_init();

    /* Gather chip/feature info early — cheap, no side effects, used to
     * print the banner AFTER runtime init succeeds. We deliberately defer
     * the banner until everything is up so its mere presence signals
     * "mikrojs is alive and the JS runtime is ready." If anything between
     * here and the printf below crashes or aborts, the banner doesn't
     * print — the absence IS the diagnostic.
     *
     * Feature advertising: we only include features for which mikrojs has
     * an actual JS API. ESP-IDF Kconfig flags like CONFIG_IEEE802154_ENABLED
     * get set by default on capable silicon (e.g. ESP32-C6) just because
     * the low-level radio driver is linked, even when no higher-level
     * stack is compiled in. WiFi and BLE each require both silicon support
     * and a corresponding `mikrojs/wifi`/`mikrojs/ble` JS module — we gate
     * on the CONFIG_* flags as a proxy. 802.15.4 is deliberately omitted
     * until a `mikrojs/zigbee` or `mikrojs/thread` module ships. */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

#if CONFIG_ESP_WIFI_ENABLED
    bool has_wifi = (chip_info.features & CHIP_FEATURE_WIFI_BGN) != 0;
#else
    bool has_wifi = false;
#endif
#if CONFIG_BT_ENABLED
    bool has_bt = (chip_info.features & CHIP_FEATURE_BT) != 0;
    bool has_ble = (chip_info.features & CHIP_FEATURE_BLE) != 0;
#else
    bool has_bt = false;
    bool has_ble = false;
#endif

    /* Capture starting heap for the later runtime-cost INFO log. */
    size_t heap_free_at_boot = esp_get_free_heap_size();
    size_t heap_total_at_boot = heap_free_at_boot;
    {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        heap_total_at_boot = info.total_free_bytes + info.total_allocated_bytes;
    }

    /* Recovery window: gives the host (or a double-reset) a chance to
     * skip the autorun if the deployed app is crash-looping.  Costs ~500ms
     * on every cold boot but is the only window we have before user code
     * runs and may panic. */
    bool safe_mode = mik__check_recovery(500);
    if (safe_mode) {
        printf("\n*** SAFE MODE: autorun skipped, dropping to REPL ***\n\n");
    }

#if CONFIG_ESP_TASK_WDT_EN
    /* Watch the main task: a hang in native code below JS now resets the
     * chip instead of sitting forever. Fed from MIK_Loop and platform->yield. */
    {
        esp_err_t err = esp_task_wdt_add(NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not add the main task to the task watchdog: %s", esp_err_to_name(err));
        }
    }
#endif

    /* Initialize NVS (needed for env vars, secrets, and WiFi) */
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        }
    }

    /* Apply MIK_LOG_LEVEL from NVS as early as possible so subsequent
     * initialization logs respect the configured level. */
    mik__apply_nvs_log_level();

    /* Mount filesystem */
    if (mount_littlefs("/appfs", "user") != ESP_OK) {
        ESP_LOGE(TAG, "Cannot mount /appfs partition, aborting");
        return;
    }

    /* Recover from incomplete deploys */
    MIK_DeployRecover();

    /* Reconcile OTA state before the JS app loads: reflash guard, trial
     * verdict, and a deferred clean-heap install of a staged build (may
     * restart). Runs after DeployRecover so an interrupted unpack's staging
     * dir is already cleaned.
     *
     * Skipped in safe mode. The install budget bounds a crashing install, but
     * safe mode is what a developer reaches for while it is still burning
     * through that budget, and reconcile runs long before the REPL comes up:
     * without this, the recovery window opens onto an install that panics
     * before anything can be typed into it. */
    if (!safe_mode) {
        mik__ota_boot_reconcile();
    }

    /* Load app config (mikro.config.json) if present */
    MIKConfig app_config;
    MIK_LoadConfig("/appfs", &app_config);

#if CONFIG_ESP_TASK_WDT_INIT
    /* Same ordering invariant as the static_assert above, for a configured
     * value. Clamp and warn rather than fail, as the memReserved clamp does. */
    {
        constexpr int kMaxBlockingMs = (CONFIG_ESP_TASK_WDT_TIMEOUT_S - MIK_TWDT_MARGIN_S) * 1000;
        if (app_config.blocking_timeout_ms > kMaxBlockingMs) {
            ESP_LOGW(TAG,
                     "watchdog.blocking %d ms must stay %d s under the %d s task watchdog; "
                     "clamping to %d ms",
                     app_config.blocking_timeout_ms, MIK_TWDT_MARGIN_S,
                     CONFIG_ESP_TASK_WDT_TIMEOUT_S, kMaxBlockingMs);
            app_config.blocking_timeout_ms = kMaxBlockingMs;
        }
    }
#endif

    /* Start file logging if configured (no-op when log_file is empty). */
    mik_logfile_init(&app_config);

    /* Create JS runtime — reserve heap for WiFi, TLS, HTTP, LittleFS, and other
     * ESP-IDF subsystems that allocate after the runtime is created.
     * Measured with dev/feature-costs (C6 and S3, 2026-08): WiFi driver
     * ≈ 33 KB steady, association ≈ 1-2 KB, the rest transient — HTTP
     * ≈ 20-25 KB, TLS handshake ≈ 25-40 KB — stacking to ≈ 80 KB worst case.
     * The 64 KB default under-covers that peak; it holds in practice because
     * JS rarely sits at its cap when a burst lands. */
    MIKRunOptions options;
    MIK_DefaultOptions(&options);
    /* Floor for the JS budget: the runtime's cold-start cost is ~78 KB
     * (atoms, class tables, builtin bytecode), so anything much below this
     * can't even finish MIK_NewRuntimeOptions. */
    constexpr uint32_t kMinJsHeap = 96 * 1024;
    uint32_t free_heap = esp_get_free_heap_size();
    if (free_heap < kMinJsHeap) {
        ESP_LOGE(TAG, "Not enough memory to start JS runtime (free: %" PRIu32
                      ", need: %" PRIu32 ")",
                 free_heap, kMinJsHeap);
        return;
    }
    /* The budget is computed ONCE, from boot-time free heap, and reused for
     * every runtime this boot creates (test supervisor, post-test session).
     * memReserved is the native subsystems' FUTURE demand from this point, so
     * heap they consume later already counts against it; re-subtracting the
     * full reserve from current free heap at a later creation double-counts
     * what is already resident and shrinks the budget by exactly that amount
     * (observed as e2e module-graph OOMs on the C3). */
    uint32_t reserved = app_config.mem_reserved;
    if (free_heap < reserved || free_heap - reserved < kMinJsHeap) {
        /* Clamp instead of failing: honoring an oversized memReserved would
         * leave the device without REPL/deploy, so a bad deployed config
         * couldn't be fixed over the wire. */
        uint32_t clamped = free_heap - kMinJsHeap;
        ESP_LOGE(TAG,
                 "memReserved %" PRIu32 " leaves less than %" PRIu32
                 " bytes for JS (free: %" PRIu32 "); clamping reserve to %" PRIu32,
                 reserved, kMinJsHeap, free_heap, clamped);
        reserved = clamped;
    }
    options.mem_limit = (int)(free_heap - reserved);
    /* QuickJS's stack overflow check compares the current SP against
     * (stack_top − stack_size). The library default (1 MB) assumes a desktop
     * stack and leaves the limit far below the real task stack bottom, so the
     * hardware faults before QuickJS can throw. Cap at ~2/3 of the main task
     * stack to leave headroom for the throw path itself. */
    options.stack_size = (CONFIG_ESP_MAIN_TASK_STACK_SIZE * 2) / 3;
    if (app_config.stack_size > 0) {
        options.stack_size = app_config.stack_size;
    }
#ifdef CONFIG_MIKROJS_QUICKJS_HEAP_PSRAM
    options.use_psram_heap = true;
#endif

    auto create_runtime = [&]() -> MIKRuntime* {
        MIKRuntime* rt = MIK_NewRuntimeOptions(&options);
        MIK_LoadEnvFromNVS(rt);
        MIK_RebuildEnv(rt);
        MIK_SetFSBasePath(rt, "/appfs");
        MIK_SetConfig(rt, &app_config);
        return rt;
    };

    MIKRuntime* mik_rt = create_runtime();

    /* ── Boot banner ──
     * Printed here, AFTER the runtime is fully initialized and the app
     * config is applied, because its mere presence signals "everything
     * came up successfully." If a step earlier in boot panicked or
     * aborted, the banner doesn't print — the absence is the diagnostic.
     *
     * Two lines: first identifies the device (version, engine version,
     * chip, cores, radios), second reports free app heap.
     *
     * The "free app heap" number is QuickJS's `malloc_limit − malloc_size`
     * immediately after runtime init — i.e. how much the user's JS code
     * can still allocate before hitting the configured soft cap. This is
     * the number users actually care about when they see "out of memory."
     * It's distinct from raw system heap, which includes memory reserved
     * for native subsystems (WiFi/lwIP/TLS) and already counts the
     * ~78 KB cold-start floor (QuickJS atoms + class tables + builtin
     * bytecode) that's unavailable for user allocations. */
    {
        /* Build the banner into a local buffer and push it through
         * mik__console_write so it lands on the user-visible console
         * (USB-Serial/JTAG). `printf` goes to newlib stdio, which
         * ESP-IDF routes to UART0 and is invisible over USB. */
        const char* core_word = chip_info.cores == 1 ? "core" : "cores";
        const MIKPlatform* plat = MIK_GetPlatform();
        const char* dev_id = plat->get_device_id ? plat->get_device_id() : nullptr;
        char banner[160];
        int off = 0;
#ifdef MIK_FW_VERSION
        off += snprintf(banner + off, sizeof(banner) - off, "Mikro.js v%s (quickjs-ng %s) on %s",
                        MIK_FW_VERSION, JS_GetVersion(), CONFIG_IDF_TARGET);
#else
        off += snprintf(banner + off, sizeof(banner) - off, "Mikro.js (quickjs-ng %s) on %s",
                        JS_GetVersion(), CONFIG_IDF_TARGET);
#endif
        /* Stable per-board device id (chip MAC as Crockford Base32). */
        if (dev_id && dev_id[0]) {
            off += snprintf(banner + off, sizeof(banner) - off, " (%s)", dev_id);
        }
        off += snprintf(banner + off, sizeof(banner) - off, ", %d %s", chip_info.cores, core_word);
        if (has_wifi || has_bt || has_ble) {
            off += snprintf(banner + off, sizeof(banner) - off, ", ");
            bool first = true;
            if (has_wifi) {
                off += snprintf(banner + off, sizeof(banner) - off, "WiFi");
                first = false;
            }
            if (has_bt) {
                off += snprintf(banner + off, sizeof(banner) - off, "%sBT", first ? "" : "/");
                first = false;
            }
            if (has_ble) {
                off += snprintf(banner + off, sizeof(banner) - off, "%sBLE", first ? "" : "/");
            }
        }
        off += snprintf(banner + off, sizeof(banner) - off, "\n");
        if (off > 0 && off < (int)sizeof(banner)) {
            mik__console_write(banner, (size_t)off);
        }

        JSMemoryUsage mem;
        JS_ComputeMemoryUsage(JS_GetRuntime(MIK_GetJSContext(mik_rt)), &mem);
        long heap_available =
            mem.malloc_limit > 0 ? (long)mem.malloc_limit - (long)mem.malloc_size : 0;
        char heap_line[48];
        int hl = snprintf(heap_line, sizeof(heap_line), "%.0f KB free heap\n",
                          heap_available / 1024.0);
        if (hl > 0 && hl < (int)sizeof(heap_line)) {
            mik__console_write(heap_line, (size_t)hl);
        }
    }

    /* Detailed memory breakdown, gated behind INFO logging. The banner
     * already shows the headline "free app heap" number; these logs add
     * the detail you want when diagnosing OOM or tuning memReserved, but
     * would be noise on every normal boot. Set `mikro env set
     * MIK_LOG_LEVEL info` to see them. */
    const MIKPlatform* platform = MIK_GetPlatform();
    {
        size_t heap_free_now = esp_get_free_heap_size();
        size_t runtime_cost =
            heap_free_at_boot > heap_free_now ? (heap_free_at_boot - heap_free_now) : 0;
        platform->log(MIK_LOG_INFO, "mikrojs",
                      "System heap: %.1f / %.1f KB free (runtime init used %.1f KB)",
                      heap_free_now / 1024.0, (double)heap_total_at_boot / 1024.0,
                      runtime_cost / 1024.0);

        JSMemoryUsage mem;
        JS_ComputeMemoryUsage(JS_GetRuntime(MIK_GetJSContext(mik_rt)), &mem);
        if (mem.malloc_limit > 0) {
            double used_pct = (double)mem.malloc_size / (double)mem.malloc_limit * 100.0;
            long available = (long)mem.malloc_limit - (long)mem.malloc_size;
            platform->log(MIK_LOG_INFO, "mikrojs",
                          "App heap: %.1f KB available (%.1f / %.1f KB used, %.0f%%)",
                          available / 1024.0, mem.malloc_size / 1024.0,
                          mem.malloc_limit / 1024.0, used_pct);
        }
    }

    /* Enter binary serial mode (suppresses ESP logging, disables line-ending
     * translation) and switch to unified protocol mode. From here on,
     * everything goes through TLV framing — including app console output. */
    mik__serial_binary_begin_no_echo();

    MIKReplTransport transport = {};
    transport.read = serial_transport_read;
    transport.write = serial_transport_write;
    transport.ctx = nullptr;
    transport.chip_name = CONFIG_IDF_TARGET;
    transport.command_handler = platform_command_handler;
    transport.command_handler_ctx = nullptr;
    transport.session_end = platform_session_end;
    transport.session_end_ctx = nullptr;

    /* Record the memory the app starts from, taken here rather than at the
     * first runtime attach: in test mode that attach comes after the test path
     * list, the supervisor's scratch buffer, the test helper globals and a
     * runtime free/create cycle, so the reading would describe the harness
     * instead of the app. Nothing below this line has run yet in either mode,
     * so both report the same figure. */
    MIK_CaptureBootMemory(mik_rt);

    /* Test-harness mode: if package.json carries a non-empty "tests" array,
     * run each listed file in its own fresh runtime through one transport
     * session. The test runtime calls __testFileDone after emitting its
     * final run_done event, which signals MIK_ProtocolExit so the serve
     * loop returns and the supervisor can swap in the next runtime.
     * Skipped in safe mode. */
    char** test_paths = nullptr;
    size_t test_count = 0;
    if (!safe_mode) {
        MIK_LoadTests("/appfs", &test_paths, &test_count);
    }
    bool test_mode = test_count > 0;

    /* Open the protocol session. In test mode we'll attach/detach per
     * test runtime; otherwise we attach the single primary runtime. */
    MIK_ProtocolOpen(&transport);

    if (test_mode) {
        auto* sup = static_cast<MIKSupScratch*>(malloc(sizeof(MIKSupScratch)));
        if (!sup) {
            ESP_LOGE(TAG, "Not enough memory for the test supervisor");
            /* Fail loud: a synthesized failing run-done plus end-of-manifest
             * lets the CLI report the failure instead of hanging on a stream
             * that will never produce results. */
            static const char kOomRunDone[] = "{\"e\":6,\"p\":0,\"f\":1,\"k\":0,\"o\":0,\"d\":0}";
            mik__proto_send(&transport, MIK_MSG_TEST, kOomRunDone, sizeof(kOomRunDone) - 1);
            mik__proto_send(&transport, MIK_MSG_MANIFEST_DONE, nullptr, 0);
            return;
        }

        /* Discard the primary runtime — each test gets a fresh one. */
        MIK_FreeRuntime(mik_rt);
        mik_rt = nullptr;

        for (size_t i = 0; i < test_count; i++) {
            /* Diagnostic: announce the file about to run so the CLI can
             * confirm the supervisor's iteration matches its own testFiles
             * order. The MSG_DEBUG frame is rendered as a dim log line.
             * Uses heap scratch to avoid bloating the main task stack —
             * the normalizer call chain during module resolution already
             * sits a few KB deep. */
            {
                int n = snprintf(sup->dbg, sizeof(sup->dbg),
                                 "[supervisor] running %zu/%zu: %s", i + 1, test_count,
                                 test_paths[i]);
                if (n > 0 && n < (int)sizeof(sup->dbg)) {
                    mik__proto_send(&transport, MIK_MSG_DEBUG, sup->dbg, n);
                }
            }
            MIKRuntime* rt = create_runtime();
            MIK_EnableTestHelpers(rt);
            MIK_ProtocolAttach(rt);
            int rc = MIK_RunEntryErr(rt, test_paths[i], sup->err, sizeof(sup->err));
            const char* fail_reason = nullptr;
            if (rc == -ENOENT) {
                fail_reason = "Test file not found";
            } else if (rc == -EFAULT) {
                fail_reason = "Evaluation threw";
            } else {
                /* Entry eval returned successfully (rc == 0). The test module
                 * may still asynchronously reject (e.g. top-level throw in a
                 * module eval'd as a Promise) — that's caught after ServeLoop
                 * by inspecting MIK_IsStopRequested below. */
                MIK_ProtocolServeLoop();
                if (MIK_IsStopRequested(rt)) {
                    fail_reason = "Unhandled rejection";
                }
            }
            if (fail_reason) {
                /* Synthesize a failing test + run_done so the CLI accounts
                 * for this file instead of stalling waiting for a run_done
                 * the runtime will never emit. Escape the path so any `"`
                 * or `\` in it doesn't corrupt the JSON frame. */
                ESP_LOGE(TAG, "%s: %s", fail_reason, test_paths[i]);
                if (mik__json_escape(sup->esc, sizeof(sup->esc), test_paths[i]) < 0) {
                    /* Path too long to fit even escaped — fall back to
                     * basename so the frame at least identifies something. */
                    const char* base = strrchr(test_paths[i], '/');
                    if (!base ||
                        mik__json_escape(sup->esc, sizeof(sup->esc), base + 1) < 0) {
                        sup->esc[0] = '?';
                        sup->esc[1] = '\0';
                    }
                }
                /* Append the captured exception text (escaped) so the CLI
                 * shows the actual error, not just "Evaluation threw". */
                sup->err_esc[0] = '\0';
                if (rc == -EFAULT && sup->err[0] != '\0') {
                    if (mik__json_escape(sup->err_esc, sizeof(sup->err_esc), sup->err) < 0) {
                        sup->err_esc[0] = '\0';
                    }
                }
                int n;
                if (sup->err_esc[0] != '\0') {
                    n = snprintf(sup->buf, sizeof(sup->buf),
                                 "{\"e\":3,\"s\":\"<load>\",\"t\":\"%s\",\"d\":0,"
                                 "\"m\":\"%s: %s\"}",
                                 sup->esc, fail_reason, sup->err_esc);
                } else {
                    n = snprintf(sup->buf, sizeof(sup->buf),
                                 "{\"e\":3,\"s\":\"<load>\",\"t\":\"%s\",\"d\":0,"
                                 "\"m\":\"%s\"}",
                                 sup->esc, fail_reason);
                }
                if (n > 0 && n < (int)sizeof(sup->buf)) {
                    mik__proto_send(&transport, MIK_MSG_TEST, sup->buf, n);
                }
                static const char kRunDone[] =
                    "{\"e\":6,\"p\":0,\"f\":1,\"k\":0,\"o\":0,\"d\":0}";
                mik__proto_send(&transport, MIK_MSG_TEST, kRunDone, sizeof(kRunDone) - 1);
            }
            MIK_ProtocolDetach();
            MIK_FreeRuntime(rt);
        }
        free(sup);

        /* Signal end-of-manifest so the CLI can finalize its report
         * without waiting on a silent stream. */
        mik__proto_send(&transport, MIK_MSG_MANIFEST_DONE, nullptr, 0);

        /* Keep the session alive for REPL / deploy commands. */
        mik_rt = create_runtime();
        MIK_ProtocolAttach(mik_rt);
    } else {
        /* Normal mode: attach primary runtime and evaluate the main entry. */
        MIK_ProtocolAttach(mik_rt);

        if (safe_mode) {
            ESP_LOGW(TAG, "Safe mode: skipping entry point %s", app_config.entry_point);
        } else {
            /* Catch fatals during an OTA trial: MIK_RunEntry hands a synchronous
             * throw to this handler itself, the loop's rejection flush hands it
             * unhandled rejections. Both note the failure and arm the panic
             * restart so reconcile reverts on the next (clean-looking) boot. */
            MIK_SetErrorHandler(mik_rt, ota_trial_error_handler, nullptr);
            int rc = MIK_RunEntry(mik_rt, app_config.entry_point);
            if (rc == -EINVAL) {
                ESP_LOGW(TAG, "No entry point configured (no \"main\" field in package.json)");
            } else if (rc == -ENOENT) {
                ESP_LOGW(TAG, "Entry point not found: %s", app_config.entry_point);
            } else if (rc == -EFAULT) {
                /* Already reported to the console by the runtime. */
                ESP_LOGE(TAG, "Failed to evaluate %s", app_config.entry_point);
            }
        }
    }

    /* Serve until transport error or CMD_EXIT. */
    MIK_ProtocolServeLoop();
    MIK_ProtocolDetach();
    MIK_FreeRuntime(mik_rt);
    MIK_ProtocolClose();
    MIK_FreeTests(test_paths, test_count);

    /* If we get here, the transport died. Restart, and say so: this reset
     * reads as "software" on the next boot, same as a host restart. */
    printf("[mikrojs] console transport closed, restarting\n");
    fflush(stdout);
    esp_restart();
}
