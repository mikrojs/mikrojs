
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "mikrojs/mikrojs.h"
#include "mikrojs/private.h"
#include "mikrojs/utils.h"

/* Init callback for bjson-backed modules: sets the default export from private value */
static int mik__bjson_module_init(JSContext* ctx, JSModuleDef* m) {
    JSValue val = JS_GetModulePrivateValue(ctx, m);
    JS_SetModuleExport(ctx, m, "default", val);
    return 0;
}

/*
 * Load a .bjson file and create a module with the deserialized value as default export.
 */
static JSModuleDef* mik__load_bjson_module(JSContext* ctx, const char* module_name,
                                           const char* bjson_path) {
    DynBuf dbuf;
    mik_dbuf_init(ctx, &dbuf);
    if (mik__load_file(ctx, &dbuf, bjson_path) != 0) {
        dbuf_free(&dbuf);
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s'", module_name);
        return NULL;
    }
    JSValue val = JS_ReadObject(ctx, dbuf.buf, dbuf.size, 0);
    dbuf_free(&dbuf);
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    JSModuleDef* m = JS_NewCModule(ctx, module_name, mik__bjson_module_init);
    if (!m) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    JS_AddModuleExport(ctx, m, "default");
    JS_SetModulePrivateValue(ctx, m, val);
    return m;
}

/*
 * Load a pre-compiled bytecode module (.bjs) from a filesystem path.
 * Handles file I/O, deserialization, module resolution, and import.meta setup.
 */
static JSModuleDef* mik__load_bjs_module(JSContext* ctx, const char* module_name,
                                         const char* bjs_path) {
    DynBuf dbuf;
    mik_dbuf_init(ctx, &dbuf);
    if (mik__load_file(ctx, &dbuf, bjs_path) != 0) {
        dbuf_free(&dbuf);
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s'", module_name);
        return NULL;
    }
    JSValue func_val = JS_ReadObject(ctx, dbuf.buf, dbuf.size, JS_READ_OBJ_BYTECODE);
    dbuf_free(&dbuf);
    if (JS_IsException(func_val)) {
        JS_FreeValue(ctx, func_val);
        return NULL;
    }
    /* No JS_ResolveModule here: this runs inside the module loader, and its
     * failure path frees every unresolved module in the context — including
     * a partially-read parent an outer JS_ReadModule frame still holds. The
     * root resolve covers loader-returned modules. */
    js_module_set_import_meta(ctx, func_val, true, false);
    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
    JS_FreeValue(ctx, func_val);
    return m;
}

/* Load a module from the filesystem. Heap-allocates path buffers to keep
 * the stack frame small during recursive .bjs deserialization. */
static JSModuleDef* mik_module_load_from_fs(JSContext* ctx, const char* module_name) {
    static const char json_tpl_start[] = "export default JSON.parse(`";
    static const char json_tpl_end[] = "`);";
    static const char txt_tpl_start[] = "export default `";
    static const char txt_tpl_end[] = "`;";

    JSModuleDef* m;
    JSValue func_val;
    DynBuf dbuf;

    char* fs_path = static_cast<char*>(js_malloc(ctx, PATH_MAX));
    if (!fs_path) return NULL;
    mik__resolve_fs_path(ctx, module_name, fs_path, PATH_MAX);

    /* Prefer pre-compiled bytecode (.bjs) over source (.js) when available */
    if (js__has_suffix(module_name, ".js")) {
        size_t len = strlen(fs_path);
        char* bjs_path = static_cast<char*>(js_malloc(ctx, len + 2));
        if (bjs_path) {
            memcpy(bjs_path, fs_path, len - 3);
            strcpy(bjs_path + len - 3, ".bjs");
            struct stat st;
            bool found = stat(bjs_path, &st) == 0;
            if (found) {
                js_free(ctx, fs_path);
                JSModuleDef* result = mik__load_bjs_module(ctx, module_name, bjs_path);
                js_free(ctx, bjs_path);
                return result;
            }
            js_free(ctx, bjs_path);
        }
    }

    /* Pre-compiled bytecode modules (.bjs) — compiled on host via qjsc -b */
    if (js__has_suffix(module_name, ".bjs")) {
        JSModuleDef* result = mik__load_bjs_module(ctx, module_name, fs_path);
        js_free(ctx, fs_path);
        return result;
    }

    /* Prefer pre-compiled bjson over source .json when available */
    if (js__has_suffix(module_name, ".json")) {
        size_t len = strlen(fs_path);
        char* bjson_path = static_cast<char*>(js_malloc(ctx, len + 2));
        if (bjson_path) {
            memcpy(bjson_path, fs_path, len - 5);
            strcpy(bjson_path + len - 5, ".bjson");
            struct stat st;
            bool found = stat(bjson_path, &st) == 0;
            if (found) {
                js_free(ctx, fs_path);
                JSModuleDef* result = mik__load_bjson_module(ctx, module_name, bjson_path);
                js_free(ctx, bjson_path);
                return result;
            }
            js_free(ctx, bjson_path);
        }
    }

    /* Direct .bjson import */
    if (js__has_suffix(module_name, ".bjson")) {
        JSModuleDef* result = mik__load_bjson_module(ctx, module_name, fs_path);
        js_free(ctx, fs_path);
        return result;
    }

    mik_dbuf_init(ctx, &dbuf);

    int is_json = js__has_suffix(module_name, ".json");
    int is_txt = js__has_suffix(module_name, ".txt");

    /* Wrap JSON/text imports in a template that default-exports the content */
    if (is_json) {
        dbuf_put(&dbuf, (const uint8_t*)json_tpl_start, strlen(json_tpl_start));
    } else if (is_txt) {
        dbuf_put(&dbuf, (const uint8_t*)txt_tpl_start, strlen(txt_tpl_start));
    }

    if (mik__load_file(ctx, &dbuf, fs_path) != 0) {
        js_free(ctx, fs_path);
        dbuf_free(&dbuf);
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s'", module_name);
        return NULL;
    }
    js_free(ctx, fs_path);

    if (is_json) {
        dbuf_put(&dbuf, (const uint8_t*)json_tpl_end, strlen(json_tpl_end));
    } else if (is_txt) {
        dbuf_put(&dbuf, (const uint8_t*)txt_tpl_end, strlen(txt_tpl_end));
    }

    /* Add null termination, required by JS_Eval. */
    dbuf_putc(&dbuf, '\0');

    /* Run preprocessor on source files (e.g. TypeScript type stripping) */
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    char* pp_source = NULL;
    size_t pp_len = 0;
    if (!is_json && !is_txt && mik_rt && mik_rt->preprocess_fn) {
        pp_source = mik_rt->preprocess_fn(module_name, (char*)dbuf.buf, dbuf.size - 1, &pp_len,
                                          mik_rt->preprocess_opaque);
    }

    const char* eval_source = pp_source ? pp_source : (char*)dbuf.buf;
    size_t eval_len = pp_source ? pp_len : dbuf.size - 1;

    /* compile JS the module */
    func_val = JS_Eval(ctx, eval_source, eval_len, module_name,
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(pp_source);
    dbuf_free(&dbuf);
    if (JS_IsException(func_val)) {
        JS_FreeValue(ctx, func_val);
        return NULL;
    }

    js_module_set_import_meta(ctx, func_val, true, false);
    /* the module is already referenced, so we must free it */
    m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
    JS_FreeValue(ctx, func_val);

    return m;
}

static JSModuleDef* mik_module_loader_inner(JSContext* ctx, const char* module_name,
                                            void* opaque) {
    static const char mikro_prefix[] = "mikro/";

    /* Virtual modules take priority over builtins — allows host-side JS to
     * override any native:* C module (e.g. mocking device modules for dev). */
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (mik_rt) {
        auto it = mik_rt->virtual_modules.find(module_name);
        if (it != mik_rt->virtual_modules.end()) {
            JSValue func_val = JS_Eval(ctx, it->second.c_str(), it->second.size(), module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(func_val)) {
                JS_FreeValue(ctx, func_val);
                return NULL;
            }
            JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
            JS_FreeValue(ctx, func_val);
            return m;
        }
    }

    /* Lazy init: check the global native module registry for native:* modules.
     * These are registered via MIK_REGISTER_MODULE() at link time but only
     * initialized on first import. */
    if (strncmp("native:", module_name, 7) == 0) {
        for (mik_module_desc_t* d = mik__module_registry_head; d != nullptr; d = d->next) {
            if (strcmp(d->name, module_name) == 0) {
                JSModuleDef* lazy_m = d->init(ctx);
                if (lazy_m && mik_rt && d->consume) {
                    MIK_RegisterLoopConsumer(mik_rt, d->consume, d->destroy);
                }
                return lazy_m;
            }
        }
        /* Native module not found — this is a device-only module that isn't
         * available in the current build (e.g. running on desktop). Same
         * wording as every other unresolvable specifier (and browsers). */
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s'", module_name);
        return nullptr;
    }

    if (strncmp(mikro_prefix, module_name, strlen(mikro_prefix)) == 0) {
        /* C-backed mikro-prefixed modules (ported from bytecode builtins). Resolved
         * here — after the virtual-module check, before bytecode builtins —
         * so MIK_RegisterVirtualModule keeps documented precedence and
         * runtimes that never import them pay nothing. */
        static const struct {
            const char* name;
            JSModuleDef* (*load)(JSContext* ctx);
        } c_modules[] = {
            {"mikro/http/helpers", mik__http_helpers_load},
            {"mikro/http/request", mik__http_request_load},
            {"mikro/wifi", mik__wifi_client_load},
        };
        for (const auto& cm : c_modules) {
            if (strcmp(cm.name, module_name) == 0) return cm.load(ctx);
        }
        JSModuleDef* builtin_m = mik__load_builtin(ctx, module_name);
        if (builtin_m) return builtin_m;
        /* mik__load_builtin returns NULL either because the module isn't in
         * the table, or because deserialization failed (e.g. stack overflow
         * from deeply nested .bjs loading). If deserialization failed, the
         * real exception is already on ctx — don't replace it. Probe with
         * JS_HasException, NOT JS_GetException + JS_IsNull: quickjs-ng
         * returns JS_UNINITIALIZED from an empty exception slot, and
         * re-throwing that surfaced as a bare "[uninitialized]" error. */
        if (!JS_HasException(ctx)) {
            JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s'", module_name);
        }
        return NULL;
    }

    /* Check external builtins for board/driver packages (e.g. "@mikrojs/your-driver").
     * These are registered via MIK_REGISTER_BUILTIN() and resolved by package name
     * before falling through to filesystem resolution. */
    {
        JSModuleDef* ext_m = mik__load_builtin(ctx, module_name);
        if (ext_m) return ext_m;
        /* NULL with an exception pending means the builtin was found but
         * failed to load (deserialization or import resolution). Don't fall
         * through to filesystem resolution — it would replace the real error
         * with a generic "failed to resolve" one. Same probe as the mikro/
         * branch above (JS_HasException, not JS_GetException + JS_IsNull). */
        if (JS_HasException(ctx)) return NULL;
    }

    return mik_module_load_from_fs(ctx, module_name);
}

JSModuleDef* mik_module_loader(JSContext* ctx, const char* module_name, void* opaque) {
    /* Optional trace log for diagnosing module-load failures. When
     * MIK_DEBUG_MODULES is set in the environment, every module load
     * attempt is logged to stderr along with the outcome. Useful when
     * tracking down "Error: null" issues where a loader path returns
     * NULL without setting a proper JS exception. */
    static const char* debug_env = getenv("MIK_DEBUG_MODULES");
    bool debug = debug_env != nullptr && debug_env[0] != '\0';
    if (debug) {
        fprintf(stderr, "[mik-modules] load: %s\n", module_name);
        fflush(stderr);
    }

    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (mik_rt == nullptr || !mik_rt->profile_enabled) {
        JSModuleDef* result = mik_module_loader_inner(ctx, module_name, opaque);
        if (debug) {
            if (result == nullptr) {
                fprintf(stderr, "[mik-modules] FAIL: %s (exception=%s)\n", module_name,
                        JS_HasException(ctx) ? "set" : "NULL");
            } else {
                fprintf(stderr, "[mik-modules] ok:   %s\n", module_name);
            }
        }
        return result;
    }
    /* Profiling path: snapshot QuickJS malloc_size before and after the
     * inner loader runs, then record the *exclusive* delta — i.e. the
     * cost of loading THIS module's bytecode/source and resolving its
     * imports, not counting whatever its imports already contributed.
     *
     * Why exclusive: mik_module_loader is called recursively. When module
     * A imports B which imports C, the call tree is
     * loader(A) → inner(A) → loader(B) → inner(B) → loader(C) → ...
     * Subtracting children's deltas gives the cost of THIS module alone.
     *
     * Caveat: this measures the load/compilation cost, not the full
     * steady-state cost. Linking (import/export binding) and top-level
     * evaluation happen later inside JS_EvalFunction where we have no
     * per-module hook. The profile total will be lower than actual
     * QuickJS heap usage by the amount those phases allocate. */
    size_t entries_before = mik_rt->profile_entries.size();
    JSMemoryUsage before;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &before);
    JSModuleDef* m = mik_module_loader_inner(ctx, module_name, opaque);
    if (debug) {
        if (m == nullptr) {
            fprintf(stderr, "[mik-modules] FAIL: %s (exception=%s)\n", module_name,
                    JS_HasException(ctx) ? "set" : "NULL");
        } else {
            fprintf(stderr, "[mik-modules] ok:   %s\n", module_name);
        }
    }
    JSMemoryUsage after;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &after);

    size_t total_delta = after.malloc_size > before.malloc_size
                             ? (size_t)(after.malloc_size - before.malloc_size)
                             : 0;
    size_t children_delta = 0;
    for (size_t i = entries_before; i < mik_rt->profile_entries.size(); i++) {
        children_delta += mik_rt->profile_entries[i].delta_bytes;
    }
    size_t exclusive_delta = total_delta > children_delta ? total_delta - children_delta : 0;

    MIKProfileEntry entry = {};
    size_t name_len = strlen(module_name);
    if (name_len >= MIK_PROFILE_NAME_MAX) {
        name_len = MIK_PROFILE_NAME_MAX - 1;
    }
    memcpy(entry.name, module_name, name_len);
    entry.name[name_len] = '\0';
    entry.delta_bytes = exclusive_delta;
    entry.order = mik_rt->profile_entries.size();
    if (mik_rt->profile_entries.size() < mik_rt->profile_entries.capacity()) {
        mik_rt->profile_entries.push_back(entry);
    }
    return m;
}

#define MIK__PATHSEP_POSIX '/'
#define MIK__PATHSEP '/'
#define MIK__PATHSEP_STR "/"

int js_module_set_import_meta(JSContext* ctx, JSValue func_val, bool use_realpath, bool is_main) {
    JSModuleDef* m;
    char buf[PATH_MAX + 16] = {0};
    JSValue meta_obj;
    JSAtom module_name_atom;
    const char* module_name;
    char module_dirname[PATH_MAX] = {0};
    char module_basename[PATH_MAX] = {0};

    CHECK_EQ(JS_VALUE_GET_TAG(func_val), JS_TAG_MODULE);
    m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));

    module_name_atom = JS_GetModuleName(ctx, m);
    module_name = JS_AtomToCString(ctx, module_name_atom);
    JS_FreeAtom(ctx, module_name_atom);
    if (!module_name) {
        return -1;
    }

    if (use_realpath) {
        /* On LittleFS paths are already absolute, so construct file:// URL directly */
        js__pstrcpy(buf, sizeof(buf), "file://");
        js__pstrcat(buf, sizeof(buf), module_name);

        /* Extract dirname and basename from the absolute path */
        const char* start = buf + 7; /* skip file:// */
        const char* p = strrchr(start, MIK__PATHSEP);
        if (p) {
            strncpy(module_dirname, start, p - start);
            strcpy(module_basename, p + 1);
        } else {
            module_dirname[0] = '\0';
            strcpy(module_basename, start);
        }
    } else {
        js__pstrcpy(buf, sizeof(buf), module_name);
    }

    JS_FreeCString(ctx, module_name);

    meta_obj = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta_obj)) {
        return -1;
    }
    JS_DefinePropertyValueStr(ctx, meta_obj, "url", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta_obj, "main", JS_NewBool(ctx, is_main), JS_PROP_C_W_E);
    if (use_realpath) {
        JS_DefinePropertyValueStr(ctx, meta_obj, "dirname", JS_NewString(ctx, module_dirname),
                                  JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, meta_obj, "basename", JS_NewString(ctx, module_basename),
                                  JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, meta_obj, "path", JS_NewString(ctx, buf + 7),
                                  JS_PROP_C_W_E);
    }
    /* Set import.meta.env from cached frozen env object. */
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (mik_rt && !JS_IsUndefined(mik_rt->env_obj)) {
        JS_DefinePropertyValueStr(ctx, meta_obj, "env", JS_DupValue(ctx, mik_rt->env_obj),
                                  JS_PROP_C_W_E);
    }

    JS_FreeValue(ctx, meta_obj);
    return 0;
}

/*
 * Resolve an export condition value to a string path.
 * Handles: string literals, and objects with "import" / "default" conditions.
 */
static const char* mik__resolve_export_condition(JSContext* ctx, JSValue exp) {
    if (JS_IsString(exp)) {
        return JS_ToCString(ctx, exp);
    }
    if (!JS_IsObject(exp)) {
        return NULL;
    }
    static const char* conditions[] = {"import", "default"};
    for (size_t i = 0; i < countof(conditions); i++) {
        JSValue val = JS_GetPropertyStr(ctx, exp, conditions[i]);
        if (!JS_IsUndefined(val) && !JS_IsException(val)) {
            const char* result = mik__resolve_export_condition(ctx, val);
            JS_FreeValue(ctx, val);
            return result;
        }
        JS_FreeValue(ctx, val);
    }
    return NULL;
}

/*
 * Given a parsed package.json (as JSValue) and a subpath (e.g. "." or "./utils"),
 * resolve the entry point file path relative to the package directory.
 *
 * TODO: check publishConfig.exports before exports
 */
/* Copy a JS_ToCString result into a js_malloc'd buffer, so every return
 * from mik__resolve_pkg_entry uses one allocator — the "./index.js"
 * fallback has no JSString to borrow, and mixing JS_FreeCString with
 * js_strdup'd pointers walks back to a fake string header. */
static char* mik__dup_cstring(JSContext* ctx, const char* cstr) {
    if (!cstr) return NULL;
    char* copy = js_strdup(ctx, cstr);
    JS_FreeCString(ctx, cstr);
    return copy;
}

/* Returns a js_malloc'd entry path (caller frees with js_free), or NULL. */
static char* mik__resolve_pkg_entry(JSContext* ctx, JSValue pkg_json, const char* subpath) {
    JSValue exports = JS_GetPropertyStr(ctx, pkg_json, "exports");

    if (JS_IsUndefined(exports)) {
        JS_FreeValue(ctx, exports);
        /* No exports field — fall back to "main", then "./index.js" */
        JSValue main_val = JS_GetPropertyStr(ctx, pkg_json, "main");
        if (JS_IsString(main_val)) {
            char* result = mik__dup_cstring(ctx, JS_ToCString(ctx, main_val));
            JS_FreeValue(ctx, main_val);
            return result;
        }
        JS_FreeValue(ctx, main_val);
        return js_strdup(ctx, "./index.js");
    }

    /* exports is a plain string — use it directly */
    if (JS_IsString(exports)) {
        char* result = mik__dup_cstring(ctx, JS_ToCString(ctx, exports));
        JS_FreeValue(ctx, exports);
        return result;
    }

    /* exports is an object — look up the subpath */
    if (JS_IsObject(exports)) {
        JSValue subpath_val = JS_GetPropertyStr(ctx, exports, subpath);
        if (!JS_IsUndefined(subpath_val) && !JS_IsException(subpath_val)) {
            char* result = mik__dup_cstring(ctx, mik__resolve_export_condition(ctx, subpath_val));
            JS_FreeValue(ctx, subpath_val);
            JS_FreeValue(ctx, exports);
            return result;
        }
        JS_FreeValue(ctx, subpath_val);

        /* No subpath key found — when resolving ".", treat the exports object
         * itself as a conditions object. This handles the common shorthand where
         * exports is {"default": "./index.js"} instead of {".": {"default": ...}} */
        if (strcmp(subpath, ".") == 0) {
            char* result = mik__dup_cstring(ctx, mik__resolve_export_condition(ctx, exports));
            JS_FreeValue(ctx, exports);
            return result;
        }
    }

    JS_FreeValue(ctx, exports);
    return NULL;
}

/*
 * Parse a bare specifier into package name (+ length) and subpath.
 * E.g. "@scope/pkg/utils" -> pkg_name="@scope/pkg", subpath="./utils"
 *      "lodash"           -> pkg_name="lodash",     subpath="."
 */
struct MIKPkgSpecifier {
    const char* pkg_name;
    int pkg_name_len;
    const char* subpath;
    char subpath_buf[PATH_MAX];
};

static bool mik__parse_pkg_specifier(const char* specifier, MIKPkgSpecifier* out) {
    out->pkg_name = specifier;

    const char* split = NULL;
    if (specifier[0] == '@') {
        const char* first_slash = strchr(specifier, '/');
        if (!first_slash) {
            return false;
        }
        split = strchr(first_slash + 1, '/');
    } else {
        split = strchr(specifier, '/');
    }

    if (split) {
        out->pkg_name_len = (int)(split - specifier);
        snprintf(out->subpath_buf, sizeof(out->subpath_buf), "./%s", split + 1);
        out->subpath = out->subpath_buf;
    } else {
        out->pkg_name_len = (int)strlen(specifier);
        out->subpath = ".";
    }
    return true;
}

/*
 * Try to resolve a package entry from a specific node_modules directory.
 * Returns a js_malloc'd absolute path on success, NULL on failure.
 */
static char* mik__try_resolve_pkg(JSContext* ctx, const char* dir, const MIKPkgSpecifier* spec) {
    char pkg_json_path[PATH_MAX];
    snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/node_modules/%.*s/package.json", dir,
             spec->pkg_name_len, spec->pkg_name);

    /* Resolve logical path to filesystem path for file I/O */
    char pkg_json_fs_path[PATH_MAX];
    mik__resolve_fs_path(ctx, pkg_json_path, pkg_json_fs_path, sizeof(pkg_json_fs_path));

    JSValue pkg_json = JS_UNDEFINED;

    /* Prefer pre-compiled .bjson over source .json when available */
    char bjson_fs_path[PATH_MAX];
    size_t len = strlen(pkg_json_fs_path);
    memcpy(bjson_fs_path, pkg_json_fs_path, len - 5); /* strip ".json" */
    strcpy(bjson_fs_path + len - 5, ".bjson");

    DynBuf dbuf;
    struct stat st;
    if (stat(bjson_fs_path, &st) == 0) {
        mik_dbuf_init(ctx, &dbuf);
        if (mik__load_file(ctx, &dbuf, bjson_fs_path) == 0) {
            pkg_json = JS_ReadObject(ctx, dbuf.buf, dbuf.size, 0);
        }
        dbuf_free(&dbuf);
    }

    /* Fall back to plain JSON */
    if (JS_IsUndefined(pkg_json)) {
        FILE* f = fopen(pkg_json_fs_path, "r");
        if (!f) {
            return NULL;
        }
        mik_dbuf_init(ctx, &dbuf);
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            dbuf_put(&dbuf, (const uint8_t*)buf, n);
        }
        fclose(f);
        dbuf_putc(&dbuf, '\0');
        pkg_json = JS_ParseJSON(ctx, (const char*)dbuf.buf, dbuf.size - 1, pkg_json_path);
        dbuf_free(&dbuf);
    }

    if (JS_IsException(pkg_json)) {
        JS_FreeValue(ctx, pkg_json);
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        return NULL;
    }

    char* entry = mik__resolve_pkg_entry(ctx, pkg_json, spec->subpath);
    JS_FreeValue(ctx, pkg_json);

    if (!entry) {
        return NULL;
    }

    const char* entry_path = entry;
    if (entry_path[0] == '.' && entry_path[1] == '/') {
        entry_path += 2;
    }

    char result[PATH_MAX];
    snprintf(result, sizeof(result), "%s/node_modules/%.*s/%s", dir, spec->pkg_name_len,
             spec->pkg_name, entry_path);
    js_free(ctx, entry);
    return js_strdup(ctx, result);
}

/*
 * Resolve a bare package specifier by walking up from base_dir looking for
 * node_modules/<package_name>/package.json, then resolving the entry point.
 *
 * Returns a js_malloc'd absolute path on success, NULL on failure.
 */
static char* mik__resolve_node_modules(JSContext* ctx, const char* base_dir,
                                       const char* specifier) {
    MIKPkgSpecifier spec;
    if (!mik__parse_pkg_specifier(specifier, &spec)) {
        return NULL;
    }

    char dir[PATH_MAX];
    js__pstrcpy(dir, sizeof(dir), base_dir);

    for (;;) {
        char* resolved = mik__try_resolve_pkg(ctx, dir, &spec);
        if (resolved) {
            return resolved;
        }

        char* last_sep = strrchr(dir, '/');
        if (!last_sep) {
            break;
        }
        if (last_sep == dir) {
            /* At root — try "/" once, then stop */
            if (dir[1] == '\0') {
                break;
            }
            dir[1] = '\0';
        } else {
            *last_sep = '\0';
        }
    }

    return NULL;
}

/* Anchored names (native:, mikro/, @mikrojs/) are never unloaded
 * and don't contribute meaningfully to the import graph for orphan
 * detection. We skip recording edges where the target is anchored. */
static bool mik__is_anchored_name(const char* name) {
    return strncmp(name, "native:", 7) == 0 || strncmp(name, "mikro/", 6) == 0 ||
           strncmp(name, "@mikrojs/", 9) == 0;
}

/* A name registered via MIK_REGISTER_BUILTIN() is bytecode compiled into the
 * firmware — the same trust level as @mikrojs builtins, regardless of the
 * package's npm scope. */
static bool mik__is_ext_builtin_name(const char* name) {
    for (mik_ext_builtin_t* p = mik__ext_builtin_head; p != nullptr; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Record an edge in the module dependency graph: `base` imports `target`.
 * No-ops if the MIKRuntime is absent, base is empty/non-module, or target
 * is anchored. Keyed by normalized names. */
static void mik__record_import_edge(MIKRuntime* mik_rt, const char* base,
                                    const char* target) {
    if (!mik_rt || !base || !*base || !target || !*target) return;
    if (mik__is_anchored_name(target)) return;
    std::string b(base), t(target);
    mik_rt->module_imports[b].insert(t);
    mik_rt->module_importers[t].insert(b);
}

static char* mik__module_normalizer_impl(JSContext* ctx, const char* base_name,
                                         const char* name, void* opaque) {
    char *filename, *p;
    const char* r;
    int len;

    static const char internal_prefix[] = "native:";
    if (strncmp(name, internal_prefix, strlen(internal_prefix)) == 0) {
        // Only firmware-embedded modules (native:, mikro/, @mikrojs/, registered
        // external builtins) may import native: internals. A builtin may import
        // any native: module, including another package's — native-level
        // composition (e.g. an audio driver reusing a codec package's bindings)
        // is intentionally allowed.
        if (strncmp(base_name, "native:", 7) != 0 && strncmp(base_name, "mikro/", 6) != 0 &&
            strncmp(base_name, "@mikrojs/", 9) != 0 && !mik__is_ext_builtin_name(base_name)) {
            JS_ThrowTypeError(
                ctx, "Cannot import '%s': native modules can only be imported by firmware builtins",
                name);
            return NULL;
        }
    }

    if (name[0] != '.' && name[0] != '/') {
        /* Built-in modules pass through unchanged */
        if (strncmp(name, "mikro/", 6) == 0 || strncmp(name, "native:", 7) == 0) {
            return js_strdup(ctx, name);
        }
        /* Check if this bare specifier matches an external builtin (board/driver
         * package). If so, pass through unchanged to avoid filesystem resolution. */
        if (mik__is_ext_builtin_name(name)) {
            return js_strdup(ctx, name);
        }
        /* Bare specifier — try node_modules resolution.
         * Heap-allocate the dir buffer to keep the normalizer stack frame
         * small during recursive .bjs loading. */
        {
            const char* sep = strrchr(base_name, '/');
            int dir_len = sep ? (int)(sep - base_name) : 0;
            char* base_dir = static_cast<char*>(js_malloc(ctx, dir_len + 2));
            if (base_dir) {
                if (dir_len > 0) {
                    memcpy(base_dir, base_name, dir_len);
                    base_dir[dir_len] = '\0';
                } else {
                    base_dir[0] = '.';
                    base_dir[1] = '\0';
                }
                char* resolved = mik__resolve_node_modules(ctx, base_dir, name);
                js_free(ctx, base_dir);
                if (resolved) {
                    return resolved;
                }
            }
        }
        /* Fall back to returning the name as-is (for built-in prefixes etc.) */
        return js_strdup(ctx, name);
    }

    /* Absolute paths are already fully qualified — don't prepend the
     * caller's directory. */
    if (name[0] == MIK__PATHSEP_POSIX) {
        return js_strdup(ctx, name);
    }

    const char* sep = strrchr(base_name, MIK__PATHSEP);
    if (sep) {
        len = sep - base_name;
    } else {
        len = 0;
    }
    /* A rooted base ('/main.js') has dirname '/', not "" */
    bool rooted = base_name[0] == MIK__PATHSEP_POSIX;

    filename = static_cast<char*>(js_malloc(ctx, len + strlen(name) + 1 + 1));
    if (!filename) {
        return NULL;
    }
    memcpy(filename, base_name, len);
    filename[len] = '\0';

    /* we only normalize the leading '..' or '.' */
    r = name;
    for (;;) {
        if (r[0] == '.' && r[1] == MIK__PATHSEP_POSIX) {
            r += 2;
        } else if (r[0] == '.' && r[1] == '.' && r[2] == MIK__PATHSEP_POSIX) {
            /* remove the last path element of filename, except if "."
               or ".." */
            if (filename[0] == '\0') {
                if (!rooted) {
                    break;
                }
                /* the parent of the root is the root */
                r += 3;
                continue;
            }
            p = strrchr(filename, MIK__PATHSEP);
            if (!p) {
                p = filename;
            } else {
                p++;
            }
            if (!strcmp(p, ".") || !strcmp(p, "..")) {
                break;
            }
            if (p > filename) {
                p--;
            }
            *p = '\0';
            r += 3;
        } else {
            break;
        }
    }
    if (filename[0] != '\0' || rooted) {
        strcat(filename, MIK__PATHSEP_STR);
    }
    strcat(filename, r);

    return filename;
}

#undef MIK__PATHSEP
#undef MIK__PATHSEP_STR
#undef MIK__PATHSEP_POSIX

char* mik_module_normalizer(JSContext* ctx, const char* base_name, const char* name,
                            void* opaque) {
    char* normalized = mik__module_normalizer_impl(ctx, base_name, name, opaque);
    if (normalized) {
        mik__record_import_edge(MIK_GetRuntime(ctx), base_name, normalized);
    }
    return normalized;
}

/* ─── Module unload ─────────────────────────────────────────────────── */

/* Mirror of QuickJS's JSModuleStatus enum. The header can't expose the
 * constants (would clash with the internal enum when quickjs.c compiles),
 * so we re-declare the one status value we care about. */
#define MIK__MODULE_STATUS_EVALUATED 5

int mik__unload_module(JSContext* ctx, const char* normalized_name) {
    MIKRuntime* mik_rt = MIK_GetRuntime(ctx);
    if (!mik_rt) {
        JS_ThrowInternalError(ctx, "mik__unload_module requires a MIKRuntime");
        return -1;
    }
    if (mik__is_anchored_name(normalized_name)) {
        JS_ThrowTypeError(ctx, "cannot unload builtin module '%s'", normalized_name);
        return -1;
    }

    /* Verify the root module is loaded before we start unwinding. */
    JSAtom root_atom = JS_NewAtom(ctx, normalized_name);
    JSModuleDef* root_m = JS_FindLoadedModule(ctx, root_atom);
    JS_FreeAtom(ctx, root_atom);
    if (!root_m) {
        JS_ThrowTypeError(ctx, "module '%s' is not loaded", normalized_name);
        return -1;
    }
    int root_status = JS_GetModuleStatus(ctx, root_m);
    if (root_status != MIK__MODULE_STATUS_EVALUATED) {
        JS_ThrowTypeError(ctx,
                          "cannot unload '%s': module is not evaluated (status=%d)",
                          normalized_name, root_status);
        return -1;
    }

    /* BFS through the import graph, collecting modules to unload.
     * A dep is scheduled only if every one of its importers is itself
     * being unloaded in this pass (i.e. it becomes an orphan). */
    std::unordered_set<std::string> scheduled;
    std::vector<std::string> order;
    scheduled.insert(normalized_name);
    order.emplace_back(normalized_name);

    for (size_t i = 0; i < order.size(); i++) {
        const std::string& cur = order[i];
        auto imports_it = mik_rt->module_imports.find(cur);
        if (imports_it == mik_rt->module_imports.end()) continue;
        for (const std::string& dep : imports_it->second) {
            if (scheduled.count(dep)) continue;
            if (mik__is_anchored_name(dep.c_str())) continue;

            auto importers_it = mik_rt->module_importers.find(dep);
            if (importers_it == mik_rt->module_importers.end()) continue;

            bool becomes_orphan = true;
            for (const std::string& importer : importers_it->second) {
                if (scheduled.count(importer) == 0) {
                    becomes_orphan = false;
                    break;
                }
            }
            if (becomes_orphan) {
                scheduled.insert(dep);
                order.emplace_back(dep);
            }
        }
    }

    /* Free each scheduled module. Skip any that are gone or aren't in a
     * freeable state. */
    int freed_count = 0;
    for (const std::string& name : order) {
        JSAtom atom = JS_NewAtom(ctx, name.c_str());
        JSModuleDef* m = JS_FindLoadedModule(ctx, atom);
        JS_FreeAtom(ctx, atom);
        if (!m) continue;
        int status = JS_GetModuleStatus(ctx, m);
        if (status != MIK__MODULE_STATUS_EVALUATED) continue;
        JS_FreeModule(ctx, m);
        freed_count++;
    }

    /* Scrub the graph: drop unloaded nodes as importers of anything they
     * imported, and remove their own import/importer entries. */
    for (const std::string& name : order) {
        auto imports_it = mik_rt->module_imports.find(name);
        if (imports_it != mik_rt->module_imports.end()) {
            for (const std::string& dep : imports_it->second) {
                auto imp_it = mik_rt->module_importers.find(dep);
                if (imp_it != mik_rt->module_importers.end()) {
                    imp_it->second.erase(name);
                    if (imp_it->second.empty()) mik_rt->module_importers.erase(imp_it);
                }
            }
            mik_rt->module_imports.erase(imports_it);
        }
        mik_rt->module_importers.erase(name);
    }

    return freed_count;
}

/* Resolve `ns` to the loaded module it is the namespace of, returning its
 * normalized name as a C string (caller frees via JS_FreeCString), or NULL
 * if `ns` is not an object or not a loaded module's namespace. */
static const char* mik__module_name_from_ns(JSContext* ctx, JSValueConst ns) {
    if (!JS_IsObject(ns)) return nullptr;
    JSModuleDef* m = JS_FindModuleByNamespace(ctx, ns);
    if (!m) return nullptr;
    JSAtom atom = JS_GetModuleName(ctx, m);
    const char* name = JS_AtomToCString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    return name;
}

int mik__unload_namespace(JSContext* ctx, JSValueConst ns) {
    const char* name = mik__module_name_from_ns(ctx, ns);
    if (!name) return 0; /* not a loaded module's namespace: idempotent no-op */
    int rv = mik__unload_module(ctx, name);
    JS_FreeCString(ctx, name);
    return rv;
}

bool mik__is_unloadable_namespace(JSContext* ctx, JSValueConst ns) {
    const char* name = mik__module_name_from_ns(ctx, ns);
    if (!name) return false;
    bool ok = !mik__is_anchored_name(name);
    JS_FreeCString(ctx, name);
    return ok;
}

/* Load `specifier` through the module loader on behalf of `importer` and
 * return its namespace. Goes through the loader (not the C-module table
 * directly) so virtual-module overrides keep documented precedence.
 *
 * A PENDING evaluation promise is normal even for fully synchronous modules:
 * quickjs-ng settles it through the job queue when this runs inside an outer
 * module graph. Two invariants, picked by `require_evaluated`:
 *  - false: the caller only stores the namespace or reads hoisted function
 *    exports, which are safe from instantiation onward.
 *  - true: the caller immediately reads or invokes const/class exports, so
 *    the module body must have finished running (status EVALUATED); a module
 *    parked in top-level await fails with a clear error instead of TDZ. */
JSValue mik__load_module_ns(JSContext* ctx, const char* importer, const char* specifier,
                            bool require_evaluated) {
    JSAtom name = JS_NewAtom(ctx, specifier);
    JSModuleDef* m = JS_FindLoadedModule(ctx, name);
    if (!m) {
        JSValue p = JS_LoadModule(ctx, importer, specifier);
        if (JS_IsException(p)) {
            /* Propagate the loader's real error (e.g. a syntax error in a
             * virtual stub), not a generic resolution failure. */
            JS_FreeAtom(ctx, name);
            return p;
        }
        if (JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, p);
            JS_PromiseMarkAsHandled(ctx, p);
            JS_FreeValue(ctx, p);
            JS_FreeAtom(ctx, name);
            return JS_Throw(ctx, reason);
        }
        /* A PENDING promise (top-level-await stub) may still reject later;
         * mark it handled so that surfaces as a load error, not as an
         * unhandled-rejection report. Harmless for the fulfilled case. */
        JS_PromiseMarkAsHandled(ctx, p);
        JS_FreeValue(ctx, p);
        m = JS_FindLoadedModule(ctx, name);
    }
    JS_FreeAtom(ctx, name);
    if (!m) {
        return JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s'", specifier);
    }
    /* 5 == EVALUATED; quickjs.h documents the numbering but exports no enum. */
    if (require_evaluated && JS_GetModuleStatus(ctx, m) != 5) {
        return JS_ThrowReferenceError(
            ctx, "Module '%s' has not finished evaluating (top-level await is not supported here)",
            specifier);
    }
    return JS_GetModuleNamespace(ctx, m);
}
