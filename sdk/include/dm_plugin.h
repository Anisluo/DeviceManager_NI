/*
 * dm_plugin.h — DeviceManager plugin ABI (C interface).
 *
 * A plugin is a shared library (.dll / .so) that exports the functions below.
 * Only C types cross the boundary; every structured argument or result is a
 * UTF-8 JSON string. This keeps plugins independent of the host's compiler,
 * C++ runtime and Qt version.
 *
 * Lifecycle
 *   manifest = plugin_manifest()             // static JSON, owned by plugin
 *   h        = plugin_create(config_json)    // one handle per device session
 *   plugin_set_callback(h, cb, user)         // optional async events
 *   rc       = plugin_invoke(h, method, args_json, out, out_len)   // repeated
 *   plugin_destroy(h)
 *
 * plugin_invoke() contract
 *   rc >= 0                : success, `out` holds a NUL-terminated JSON value,
 *                            rc is its length in bytes (without NUL).
 *   rc == DM_E_BUFFER      : result did not fit. The plugin keeps the result;
 *                            the host calls plugin_invoke(h, DM_METHOD_FETCH,
 *                            "{}", bigger_buf, n) to retrieve it without
 *                            re-running the method. The required length is
 *                            written to `out` as {"need":N} when out_len >= 32.
 *   other negative values  : failure, `out` holds {"error":"message"} if it fits.
 *
 * Threading
 *   The host serialises calls per handle (never two concurrent invokes on the
 *   same handle), but different handles may be called from different threads.
 *   The event callback may be called from any plugin thread; the host copies
 *   the string before returning.
 */
#ifndef DM_PLUGIN_H
#define DM_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define PLUGIN_API __declspec(dllexport)
#else
#  define PLUGIN_API __attribute__((visibility("default")))
#endif

#define DM_ABI_VERSION 1

/* Return codes for plugin_invoke */
#define DM_OK               0
#define DM_E_FAIL          -1   /* generic failure, see {"error":...}        */
#define DM_E_BUFFER        -2   /* output buffer too small, use DM_METHOD_FETCH */
#define DM_E_NO_METHOD     -3   /* unknown method name                       */
#define DM_E_BAD_ARGS      -4   /* args_json malformed or missing fields     */
#define DM_E_HW            -5   /* instrument / driver error                 */
#define DM_E_STATE         -6   /* handle not in a state that allows the call */

/* Reserved method names handled by every plugin (the C++ SDK does it for you) */
#define DM_METHOD_FETCH    "$fetch"    /* return the result kept after DM_E_BUFFER */
#define DM_METHOD_METHODS  "$methods"  /* list of {name, args, doc}               */

typedef void (*dm_event_cb)(const char* evt_json, void* user);

/* Function pointer types, used by hosts that resolve symbols dynamically. */
typedef int         (*dm_abi_version_fn)(void);
typedef const char* (*dm_manifest_fn)(void);
typedef void*       (*dm_create_fn)(const char* config_json);
typedef int         (*dm_invoke_fn)(void* h, const char* method, const char* args_json,
                                    char* out_json, int out_len);
typedef void        (*dm_destroy_fn)(void* h);
typedef void        (*dm_set_callback_fn)(void* h, dm_event_cb cb, void* user);

/* ABI version this plugin was built against (must equal DM_ABI_VERSION). */
PLUGIN_API int         plugin_abi_version(void);

/* Static manifest JSON: {"id","name","version","capabilities":[...],"methods":[...]} */
PLUGIN_API const char* plugin_manifest(void);

/* config_json: {"resource":"PXI1Slot8","simulate":true,"options":{...}}
 * Returns NULL on failure. */
PLUGIN_API void*       plugin_create(const char* config_json);

PLUGIN_API int         plugin_invoke(void* h, const char* method, const char* args_json,
                                     char* out_json, int out_len);

PLUGIN_API void        plugin_destroy(void* h);

/* Register (or clear with cb == NULL) the async event callback.
 * Event JSON: {"type":"...", ...}; conventional types: "log", "data", "state". */
PLUGIN_API void        plugin_set_callback(void* h, dm_event_cb cb, void* user);

#ifdef __cplusplus
}
#endif

#endif /* DM_PLUGIN_H */
