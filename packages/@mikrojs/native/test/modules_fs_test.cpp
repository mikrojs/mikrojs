#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <mikrojs/mikrojs.h>
#include <mikrojs/private.h>
#include <quickjs.h>

#include <doctest.h>

/* Host-side tests for filesystem module loading (modules.cpp): relative ES
 * modules, JSON/text wrapping, .bjs/.bjson precompiled modules and their
 * source-shadowing preference, node_modules package resolution, import.meta,
 * virtual modules, and the preprocessor hook. Modules resolve against
 * MIK_SetFSBasePath, so each case runs in a fresh mkdtemp root. */

namespace {

static int rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return remove(path);
}

struct ModFixture {
    char root[64];
    MIKRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    ModFixture() {
        snprintf(root, sizeof(root), "/tmp/mik_mod_test_XXXXXX");
        REQUIRE(mkdtemp(root) != nullptr);
        rt = MIK_NewRuntime();
        REQUIRE(rt != nullptr);
        MIK_SetFSBasePath(rt, root);
        ctx = MIK_GetJSContext(rt);
    }

    ~ModFixture() {
        MIK_FreeRuntime(rt);
        nftw(root, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
    }

    /* Write a file at logical path `rel` (creating parent dirs). */
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

/* Evaluate a main module (by default named /main.js) and report the outcome:
 * "ok" or the rejection's message. */
static std::string eval_main(JSContext* ctx, const char* src, const char* name = "/app/main.js") {
    JSValue rv = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_MODULE);
    if (JS_IsException(rv)) {
        JS_FreeValue(ctx, rv);
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        std::string out = s ? s : "eval exception";
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        return out;
    }
    JSPromiseStateEnum state = JS_PromiseState(ctx, rv);
    std::string out = "ok";
    if (state != JS_PROMISE_FULFILLED) {
        JSValue reason = JS_PromiseResult(ctx, rv);
        const char* s = JS_ToCString(ctx, reason);
        out = s ? s : "rejected";
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, reason);
    }
    JS_FreeValue(ctx, rv);
    return out;
}

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

/* Serialize a module (or a plain value parsed from JSON) in a scratch
 * runtime, so nothing registers in the fixture runtime's module map. */
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

static std::string serialize_json_value(const char* json) {
    MIKRuntime* scratch = MIK_NewRuntime();
    REQUIRE(scratch != nullptr);
    JSContext* sctx = MIK_GetJSContext(scratch);
    JSValue val = JS_ParseJSON(sctx, json, strlen(json), "<bjson>");
    REQUIRE(!JS_IsException(val));
    size_t len = 0;
    uint8_t* buf = JS_WriteObject(sctx, &len, val, 0);
    JS_FreeValue(sctx, val);
    REQUIRE(buf != nullptr);
    std::string out(reinterpret_cast<char*>(buf), len);
    js_free(sctx, buf);
    MIK_FreeRuntime(scratch);
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(ModFixture, "relative imports and import.meta" *
                                  doctest::test_suite("modules-fs")) {
    MIK_SetEnvVar(rt, "GREETING", "salut");
    MIK_RebuildEnv(rt);
    write("/app/sub/lib.js",
          "export const v = 'from-lib'\n"
          "export const meta = {url: import.meta.url, dirname: import.meta.dirname,\n"
          "  basename: import.meta.basename, path: import.meta.path,\n"
          "  main: import.meta.main, env: import.meta.env.GREETING}\n");
    CHECK(eval_main(ctx,
                    "import {v, meta} from './sub/lib.js'\n"
                    "globalThis.__v = v\n"
                    "globalThis.__meta = JSON.stringify(meta)\n") == "ok");
    CHECK(read_global_string(ctx, "__v") == "from-lib");
    CHECK(read_global_string(ctx, "__meta") ==
          "{\"url\":\"file:///app/sub/lib.js\",\"dirname\":\"/app/sub\",\"basename\":\"lib.js\","
          "\"path\":\"/app/sub/lib.js\",\"main\":false,\"env\":\"salut\"}");
}

TEST_CASE_FIXTURE(ModFixture, "json and txt imports default-export content" *
                                  doctest::test_suite("modules-fs")) {
    write("/app/cfg.json", "{\"answer\": 42, \"nested\": {\"on\": true}}");
    write("/app/note.txt", "plain text payload");
    CHECK(eval_main(ctx,
                    "import cfg from './cfg.json'\n"
                    "import note from './note.txt'\n"
                    "globalThis.__answer = String(cfg.answer)\n"
                    "globalThis.__on = String(cfg.nested.on)\n"
                    "globalThis.__note = note\n") == "ok");
    CHECK(read_global_string(ctx, "__answer") == "42");
    CHECK(read_global_string(ctx, "__on") == "true");
    CHECK(read_global_string(ctx, "__note") == "plain text payload");
}

TEST_CASE_FIXTURE(ModFixture, "missing and malformed modules reject" *
                                  doctest::test_suite("modules-fs")) {
    std::string missing = eval_main(ctx, "import './nope.js'\n");
    CHECK(missing.find("Failed to resolve module specifier") != std::string::npos);
    CHECK(missing.find("/app/nope.js") != std::string::npos);

    write("/app/bad.json", "{not json at all");
    std::string bad = eval_main(ctx, "import bad from './bad.json'\n", "/app/main2.js");
    CHECK(bad.find("SyntaxError") != std::string::npos);

    write("/app/syntax.js", "export const = broken\n");
    std::string syn = eval_main(ctx, "import './syntax.js'\n", "/app/main3.js");
    CHECK(syn.find("SyntaxError") != std::string::npos);
}

TEST_CASE_FIXTURE(ModFixture, "precompiled .bjs modules load and shadow source" *
                                  doctest::test_suite("modules-fs")) {
    std::string pre = serialize_module("export const kind = 'bytecode-direct'\n", "/app/pre.bjs");
    write("/app/pre.bjs", pre.data(), pre.size());

    write("/app/both.js", "export const which = 'source'\n");
    std::string both = serialize_module("export const which = 'bytecode'\n", "/app/both.js");
    write("/app/both.bjs", both.data(), both.size());

    CHECK(eval_main(ctx,
                    "import {kind} from './pre.bjs'\n"
                    "import {which} from './both.js'\n"
                    "globalThis.__kind = kind\n"
                    "globalThis.__which = which\n") == "ok");
    CHECK(read_global_string(ctx, "__kind") == "bytecode-direct");
    /* the .bjs next to the .js wins */
    CHECK(read_global_string(ctx, "__which") == "bytecode");
}

TEST_CASE_FIXTURE(ModFixture, "precompiled .bjson values load and shadow .json" *
                                  doctest::test_suite("modules-fs")) {
    std::string direct = serialize_json_value("{\"answer\": 7}");
    write("/app/data.bjson", direct.data(), direct.size());

    write("/app/pref.json", "{\"which\": \"json\"}");
    std::string shadow = serialize_json_value("{\"which\": \"bjson\"}");
    write("/app/pref.bjson", shadow.data(), shadow.size());

    CHECK(eval_main(ctx,
                    "import data from './data.bjson'\n"
                    "import pref from './pref.json'\n"
                    "globalThis.__answer = String(data.answer)\n"
                    "globalThis.__which = pref.which\n") == "ok");
    CHECK(read_global_string(ctx, "__answer") == "7");
    CHECK(read_global_string(ctx, "__which") == "bjson");
}

TEST_CASE_FIXTURE(ModFixture, "corrupt and missing precompiled modules reject" *
                                  doctest::test_suite("modules-fs")) {
    const char garbage[] = {'\xde', '\xad', '\xbe', '\xef'};
    write("/app/bad.bjs", garbage, sizeof(garbage));
    write("/app/bad.bjson", garbage, sizeof(garbage));

    CHECK(eval_main(ctx, "import './bad.bjs'\n").find("invalid version") != std::string::npos);
    CHECK(eval_main(ctx, "import './bad.bjson'\n", "/app/main2.js").find("invalid version") != std::string::npos);

    std::string no_bjs = eval_main(ctx, "import './absent.bjs'\n", "/app/main3.js");
    CHECK(no_bjs.find("Failed to resolve module specifier") != std::string::npos);
    std::string no_bjson = eval_main(ctx, "import './absent.bjson'\n", "/app/main4.js");
    CHECK(no_bjson.find("Failed to resolve module specifier") != std::string::npos);

    /* a .bjs whose dependency lacks the imported binding fails at resolve */
    std::string badbind = serialize_module(
        "import {definitely_not_exported} from 'mikro/result'\n"
        "export const y = definitely_not_exported\n",
        "/app/badbind.bjs");
    write("/app/badbind.bjs", badbind.data(), badbind.size());
    CHECK(eval_main(ctx, "import './badbind.bjs'\n", "/app/main5.js").find("definitely_not_exported") != std::string::npos);
}

TEST_CASE_FIXTURE(ModFixture, "node_modules resolution covers entry styles" *
                                  doctest::test_suite("modules-fs")) {
    write("/node_modules/mainpkg/package.json", "{\"main\": \"./entry.js\"}");
    write("/node_modules/mainpkg/entry.js", "export const id = 'main-field'\n");

    write("/node_modules/idxpkg/package.json", "{}");
    write("/node_modules/idxpkg/index.js", "export const id = 'index-fallback'\n");

    write("/node_modules/strpkg/package.json", "{\"exports\": \"./e.js\"}");
    write("/node_modules/strpkg/e.js", "export const id = 'exports-string'\n");

    write("/node_modules/condpkg/package.json",
          "{\"exports\": {\".\": {\"import\": \"./i.js\"}}}");
    write("/node_modules/condpkg/i.js", "export const id = 'exports-condition'\n");

    write("/node_modules/shortpkg/package.json", "{\"exports\": {\"default\": \"./d.js\"}}");
    write("/node_modules/shortpkg/d.js", "export const id = 'exports-shorthand'\n");

    write("/node_modules/subpkg/package.json", "{\"exports\": {\"./extra\": \"./x.js\"}}");
    write("/node_modules/subpkg/x.js", "export const id = 'exports-subpath'\n");

    write("/node_modules/@scope/scpkg/package.json", "{\"main\": \"./s.js\"}");
    write("/node_modules/@scope/scpkg/s.js", "export const id = 'scoped'\n");

    CHECK(eval_main(ctx,
                    "import {id as a} from 'mainpkg'\n"
                    "import {id as b} from 'idxpkg'\n"
                    "import {id as c} from 'strpkg'\n"
                    "import {id as d} from 'condpkg'\n"
                    "import {id as e} from 'shortpkg'\n"
                    "import {id as f} from 'subpkg/extra'\n"
                    "import {id as g} from '@scope/scpkg'\n"
                    "globalThis.__ids = [a, b, c, d, e, f, g].join('|')\n") == "ok");
    CHECK(read_global_string(ctx, "__ids") ==
          "main-field|index-fallback|exports-string|exports-condition|exports-shorthand|"
          "exports-subpath|scoped");
}

TEST_CASE_FIXTURE(ModFixture, "package resolution rejects unusable specifiers and exports" *
                                  doctest::test_suite("modules-fs")) {
    /* exports with neither "import" nor "default" condition */
    write("/node_modules/reqonly/package.json", "{\"exports\": {\"require\": \"./r.js\"}}");
    write("/node_modules/reqonly/r.js", "export const id = 'nope'\n");
    CHECK(eval_main(ctx, "import 'reqonly'\n").find("Failed to resolve module specifier") != std::string::npos);

    /* a scope without a package name is not a valid specifier */
    CHECK(eval_main(ctx, "import '@justscope'\n", "/app/main2.js").find("Failed to resolve module specifier") != std::string::npos);

    /* subpath not present in the exports map */
    write("/node_modules/subonly/package.json", "{\"exports\": {\"./there\": \"./t.js\"}}");
    write("/node_modules/subonly/t.js", "export const id = 't'\n");
    CHECK(eval_main(ctx, "import 'subonly/elsewhere'\n", "/app/main3.js").find("Failed to resolve module specifier") != std::string::npos);

    /* malformed package.json fails resolution instead of crashing */
    write("/node_modules/badjson/package.json", "{not json");
    CHECK(eval_main(ctx, "import 'badjson'\n", "/app/main4.js").find("Failed to resolve module specifier") != std::string::npos);
}

TEST_CASE_FIXTURE(ModFixture, "package metadata variants: package.bjson and large manifests" *
                                  doctest::test_suite("modules-fs")) {
    /* precompiled package.bjson takes precedence over package.json */
    std::string meta = serialize_json_value("{\"main\": \"./m.js\"}");
    write("/node_modules/bjpkg/package.bjson", meta.data(), meta.size());
    write("/node_modules/bjpkg/package.json", "{\"main\": \"./wrong.js\"}");
    write("/node_modules/bjpkg/m.js", "export const id = 'from-bjson'\n");

    /* a manifest larger than one 256-byte read chunk */
    std::string big = "{\"main\": \"./big.js\", \"description\": \"";
    big += std::string(400, 'd');
    big += "\"}";
    write("/node_modules/bigpkg/package.json", big.c_str());
    write("/node_modules/bigpkg/big.js", "export const id = 'big-manifest'\n");

    CHECK(eval_main(ctx,
                    "import {id as a} from 'bjpkg'\n"
                    "import {id as b} from 'bigpkg'\n"
                    "globalThis.__ids = a + '|' + b\n") == "ok");
    CHECK(read_global_string(ctx, "__ids") == "from-bjson|big-manifest");
}

TEST_CASE_FIXTURE(ModFixture, "bare imports from a slash-less importer fail resolution" *
                                  doctest::test_suite("modules-fs")) {
    write("/node_modules/rootpkg/package.json", "{\"main\": \"./p.js\"}");
    write("/node_modules/rootpkg/p.js", "export const id = 'x'\n");
    /* An importer named without any directory ('main.js') resolves bare
     * specifiers from base_dir '.', which cannot be joined onto the fs base
     * path. Loader-produced module names always carry a leading slash, so
     * this only affects hand-named evals; pin the (clean) failure mode. */
    std::string out = eval_main(ctx,
                                "import {id} from 'rootpkg'\n"
                                "globalThis.__dot = id\n",
                                "main.js");
    CHECK(out.find("Failed to resolve module specifier 'rootpkg'") != std::string::npos);
}

TEST_CASE_FIXTURE(ModFixture, "node_modules resolution walks up from nested importers" *
                                  doctest::test_suite("modules-fs")) {
    write("/node_modules/uppkg/package.json", "{\"main\": \"./u.js\"}");
    write("/node_modules/uppkg/u.js", "export const id = 'found-above'\n");
    write("/app/nested/deep/mod.js",
          "import {id} from 'uppkg'\n"
          "export const got = id\n");
    CHECK(eval_main(ctx,
                    "import {got} from './nested/deep/mod.js'\n"
                    "globalThis.__got = got\n") == "ok");
    CHECK(read_global_string(ctx, "__got") == "found-above");

    std::string missing = eval_main(ctx, "import 'no-such-pkg'\n", "/app/main2.js");
    CHECK(missing.find("Failed to resolve module specifier") != std::string::npos);
}

TEST_CASE_FIXTURE(ModFixture, "virtual modules take priority" *
                                  doctest::test_suite("modules-fs")) {
    const char* src = "export const virt = 'overridden'\n";
    MIK_RegisterVirtualModule(rt, "mikro/test-virtual", src, strlen(src));
    CHECK(eval_main(ctx,
                    "import {virt} from 'mikro/test-virtual'\n"
                    "globalThis.__virt = virt\n") == "ok");
    CHECK(read_global_string(ctx, "__virt") == "overridden");
}

TEST_CASE_FIXTURE(ModFixture, "unavailable native and builtin modules report clearly" *
                                  doctest::test_suite("modules-fs")) {
    /* From app code the normalizer gate rejects native: imports outright. */
    std::string gated = eval_main(ctx, "import 'native:mikro/no-such'\n");
    CHECK(gated.find("can only be imported by firmware builtins") != std::string::npos);

    /* From builtin-shaped code the loader reports the missing native module. */
    const char* probe = "import 'native:mikro/no-such'\n";
    MIK_RegisterVirtualModule(rt, "mikro/test-native-probe", probe, strlen(probe));
    std::string missing =
        eval_main(ctx, "import 'mikro/test-native-probe'\n", "/app/main2.js");
    CHECK(missing.find("TypeError: Failed to resolve module specifier 'native:mikro/no-such'") !=
          std::string::npos);

    std::string builtin = eval_main(ctx, "import 'mikro/no-such-builtin'\n", "/app/main3.js");
    CHECK(builtin.find("TypeError: Failed to resolve module specifier 'mikro/no-such-builtin'") !=
          std::string::npos);
}

namespace {

/* Preprocessor hook: expands the token __MAGIC__ to 123. */
static char* magic_preprocess(const char* module_name, const char* source, size_t len,
                              size_t* out_len, void* opaque) {
    (void)module_name;
    (void)opaque;
    std::string src(source, len);
    size_t pos = src.find("__MAGIC__");
    if (pos == std::string::npos) return nullptr; /* keep original */
    src.replace(pos, strlen("__MAGIC__"), "123");
    /* JS_Eval requires zero-terminated input; return a proper C string */
    char* out = static_cast<char*>(malloc(src.size() + 1));
    memcpy(out, src.data(), src.size());
    out[src.size()] = '\0';
    *out_len = src.size();
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(ModFixture, "the preprocessor rewrites fs module source" *
                                  doctest::test_suite("modules-fs")) {
    MIK_SetPreprocessor(rt, magic_preprocess, nullptr);
    write("/app/magic.js", "export const n = __MAGIC__\n");
    CHECK(eval_main(ctx,
                    "import {n} from './magic.js'\n"
                    "globalThis.__n = String(n)\n") == "ok");
    CHECK(read_global_string(ctx, "__n") == "123");
}
