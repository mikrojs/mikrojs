#include <quickjs.h>

#include "mikrojs/private.h"
#include "mikrojs/utils.h"

/* AbortController / AbortSignal globals.
 *
 * The implementation lives in runtime/abort/abort.ts and ships as
 * precompiled bytecode in the builtins table ("mikro/abort").
 * Evaluating that module installs AbortController, AbortSignal,
 * AbortError, and TimeoutError on globalThis.
 *
 * Lazy-initialized: this file installs lazy getters for the four
 * globals; the first access evaluates the bytecode module. Bytecode
 * (rather than JS source evaluated at runtime) keeps the QuickJS
 * parser out of the install path — the parse peak pushed low-memory
 * chips into OOM when the heap was already near full.
 */

/* Names of the globals the abort module installs */
static const char* const abort_global_names[] = {"AbortController", "AbortSignal", "AbortError",
                                                  "TimeoutError"};
static constexpr int ABORT_GLOBAL_COUNT = 4;

/**
 * Lazy getter: on first access to any of the four globals, delete all
 * lazy getters, evaluate the bytecode module to install real values,
 * then return the requested one. The magic parameter identifies which
 * global.
 *
 * Signature must match JS_CFUNC_getter_magic: (ctx, this_val, magic).
 */
static JSValue mik__abort_lazy_get(JSContext* ctx, JSValue this_val, int magic) {

    JSValue global_obj = JS_GetGlobalObject(ctx);

    /* Remove all four lazy getters so the module's globalThis
     * assignments define plain properties */
    for (int i = 0; i < ABORT_GLOBAL_COUNT; i++) {
        JSAtom prop = JS_NewAtom(ctx, abort_global_names[i]);
        JS_DeleteProperty(ctx, global_obj, prop, 0);
        JS_FreeAtom(ctx, prop);
    }

    /* Evaluate the precompiled module; its side effect installs the
     * globals. The module is synchronous, so the returned promise is
     * already settled. */
    JSModuleDef* m = mik__load_builtin(ctx, "mikro/abort");
    if (m == NULL) {
        /* A table miss returns NULL with no pending exception (only
         * deserialize failures throw); without this guard the getter
         * would surface a bare 'null'. Mirrors the module loader's
         * missing-builtin fallback. */
        if (!JS_HasException(ctx)) {
            JS_ThrowTypeError(ctx, "Failed to resolve module specifier 'mikro/abort'");
        }
        JS_FreeValue(ctx, global_obj);
        return JS_EXCEPTION;
    }
    /* The loader no longer resolves bindings (unsafe when nested); this is
     * a direct evaluation, so resolve here before running the module. */
    JSValue mod_val = JS_MKPTR(JS_TAG_MODULE, m);
    if (JS_ResolveModule(ctx, mod_val) < 0) {
        JS_FreeValue(ctx, global_obj);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_EvalFunction(ctx, JS_DupValue(ctx, mod_val));
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, global_obj);
        return JS_EXCEPTION;
    }
    if (JS_PromiseState(ctx, ret) == JS_PROMISE_REJECTED) {
        JSValue err = JS_PromiseResult(ctx, ret);
        /* The rejection is re-thrown below; mark the promise handled so the
         * unhandled-rejection flush doesn't report the same error again. */
        JS_PromiseMarkAsHandled(ctx, ret);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, global_obj);
        return JS_Throw(ctx, err);
    }
    JS_FreeValue(ctx, ret);

    /* Return the requested global */
    JSValue val = JS_GetPropertyStr(ctx, global_obj, abort_global_names[magic]);
    JS_FreeValue(ctx, global_obj);
    return val;
}

void mik__abort_init(JSContext* ctx, JSValue global_obj) {
    /* Install lazy getters instead of eagerly evaluating the module.
     * Each getter shares the same C function, distinguished by magic. */
    for (int i = 0; i < ABORT_GLOBAL_COUNT; i++) {
        JSAtom prop = JS_NewAtom(ctx, abort_global_names[i]);
        JSCFunctionType ft;
        ft.getter_magic = mik__abort_lazy_get;
        JSValue getter = JS_NewCFunction2(ctx, ft.generic, abort_global_names[i], 0,
                                          JS_CFUNC_getter_magic, i);
        JS_DefinePropertyGetSet(ctx, global_obj, prop, getter, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, prop);
    }
}
