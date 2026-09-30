/*
 * lua_config.c - Lua configuration bridge
 *
 * This is a thin, safe control-plane layer for the UI.  It does not embed a
 * Lua VM by itself; instead it stores the current configuration and provides a
 * single place where a future Lua backend can be attached without touching the
 * GUI again.
 */

#include "lua_config.h"
#include "../../include/serial.h"
#include "../../include/string.h"

#include <string.h>

static cos_lua_config_t g_lua_cfg;
static bool g_lua_cfg_inited = false;
static char g_pending_script[256];
static bool g_has_pending_script = false;

static void lua_copy_reason(char* dst, size_t dst_sz, const char* msg) {
    if (!dst || dst_sz == 0) {
        return;
    }
    cos_strlcpy(dst, msg ? msg : "Lua runtime is not linked in this build", dst_sz);
}

static bool lua_has_suffix(const char* s, const char* suffix) {
    if (!s || !suffix) {
        return false;
    }
    size_t slen = strlen(s);
    size_t tlen = strlen(suffix);
    return slen >= tlen && strcmp(s + (slen - tlen), suffix) == 0;
}

static bool lua_is_path_safe(const char* path, bool require_lua_suffix, char* reason, size_t reason_sz) {
    if (!path || path[0] == '\0') {
        lua_copy_reason(reason, reason_sz, "Lua script path is empty");
        return false;
    }

    /* Reject control characters, backslashes, and path traversal. */
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        if (*p < 0x20 || *p == 0x7f || *p == '\\') {
            lua_copy_reason(reason, reason_sz, "Lua script path contains unsafe characters");
            return false;
        }
    }

    size_t path_len = strlen(path);
    for (size_t i = 0; i + 1 < path_len; ++i) {
        if (path[i] == '.' && path[i + 1] == '.' && (i + 2 == path_len || path[i + 2] == '/')) {
            lua_copy_reason(reason, reason_sz, "Lua script path must not traverse directories");
            return false;
        }
    }

    if (require_lua_suffix && !(lua_has_suffix(path, ".lua") || lua_has_suffix(path, ".luac"))) {
        lua_copy_reason(reason, reason_sz, "Lua script must end with .lua or .luac");
        return false;
    }

    return true;
}

static void lua_normalize_dir(char* dst, size_t dst_sz, const char* fallback) {
    if (!dst || dst_sz == 0) {
        return;
    }
    const char* src = (fallback && fallback[0]) ? fallback : "/scripts";
    cos_strlcpy(dst, src, dst_sz);
    if (dst[0] == '\0') {
        cos_strlcpy(dst, "/scripts", dst_sz);
    }
}

static bool lua_resolve_under_base(const char* base_dir, const char* requested,
                                   bool require_lua_suffix, char* out, size_t out_sz,
                                   char* reason, size_t reason_sz) {
    if (!out || out_sz == 0) {
        lua_copy_reason(reason, reason_sz, "Lua output buffer is invalid");
        return false;
    }
    out[0] = '\0';

    if (!lua_is_path_safe(requested, require_lua_suffix, reason, reason_sz)) {
        return false;
    }

    char base[256];
    lua_normalize_dir(base, sizeof(base), base_dir);
    size_t base_len = strlen(base);

    if (requested[0] == '/') {
        size_t requested_len = strlen(requested);
        if (requested_len < base_len ||
            strncmp(requested, base, base_len) != 0 ||
            !(requested[base_len] == '\0' || requested[base_len] == '/')) {
            lua_copy_reason(reason, reason_sz, "Lua script must stay under the configured scripts path");
            return false;
        }
        cos_strlcpy(out, requested, out_sz);
    } else {
        cos_strlcpy(out, base, out_sz);
        size_t used = strlen(out);
        if (used + 1 < out_sz && out[used - 1] != '/') {
            out[used++] = '/';
            out[used] = '\0';
        }
        cos_strlcpy(out + used, requested, out_sz - used);
    }

    if (!lua_is_path_safe(out, require_lua_suffix, reason, reason_sz)) {
        return false;
    }
    return true;
}

void cos_lua_config_defaults(cos_lua_config_t* cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cos_strlcpy(cfg->scripts_path, "/scripts", sizeof(cfg->scripts_path));
    cfg->sandbox = true;
    cfg->debug = false;
    cfg->memory_limit_mb = 64;
    cfg->timeout_ms = 5000;
    cfg->allow_fs = false;
    cfg->allow_net = false;
    cfg->allow_gui = true;
    cos_strlcpy(cfg->lua_version, "5.4", sizeof(cfg->lua_version));
}

static void lua_sanitize_config(cos_lua_config_t* cfg) {
    if (!cfg) {
        return;
    }

    g_pending_script[0] = '\0';
    g_has_pending_script = false;

    char reason[128];
    if (!lua_is_path_safe(cfg->scripts_path, false, reason, sizeof(reason))) {
        lua_normalize_dir(cfg->scripts_path, sizeof(cfg->scripts_path), "/scripts");
    }

    if (cfg->autorun_script[0] != '\0' &&
        !lua_resolve_under_base(cfg->scripts_path, cfg->autorun_script, true,
                                g_pending_script, sizeof(g_pending_script),
                                reason, sizeof(reason))) {
        cfg->autorun_script[0] = '\0';
    }

    if (cfg->memory_limit_mb < 0) cfg->memory_limit_mb = 0;
    if (cfg->memory_limit_mb > 256) cfg->memory_limit_mb = 256;
    if (cfg->timeout_ms < 0) cfg->timeout_ms = 0;
    if (cfg->timeout_ms > 60000) cfg->timeout_ms = 60000;

    if (cfg->sandbox) {
        cfg->allow_fs = false;
        cfg->allow_net = false;
    }
    cfg->allow_gui = cfg->enabled && cfg->allow_gui;
}

void cos_lua_set_config(const cos_lua_config_t* cfg) {
    if (!cfg) {
        return;
    }
    g_lua_cfg = *cfg;
    lua_sanitize_config(&g_lua_cfg);
    g_lua_cfg_inited = true;
    if (g_lua_cfg.autorun_script[0] != '\0') {
        cos_strlcpy(g_pending_script, g_lua_cfg.autorun_script, sizeof(g_pending_script));
        g_has_pending_script = true;
    }
    serial_puts("[LUA] Config updated\n");
}

const cos_lua_config_t* cos_lua_get_config(void) {
    if (!g_lua_cfg_inited) {
        cos_lua_config_defaults(&g_lua_cfg);
        g_lua_cfg_inited = true;
    }
    return &g_lua_cfg;
}

bool cos_lua_backend_available(void) {
    #ifdef COS_LUA_LINKED
    return true;
    #else
    return false;
    #endif
}

bool cos_lua_validate_script_path(const char* path, bool require_lua_suffix,
                                  char* reason, size_t reason_sz) {
    return lua_is_path_safe(path, require_lua_suffix, reason, reason_sz);
}

bool cos_lua_resolve_script_path(const char* requested, bool require_lua_suffix,
                                 char* out, size_t out_sz,
                                 char* reason, size_t reason_sz) {
    const cos_lua_config_t* cfg = cos_lua_get_config();
    return lua_resolve_under_base(cfg->scripts_path, requested, require_lua_suffix,
                                  out, out_sz, reason, reason_sz);
}

const char* cos_lua_get_pending_script(void) {
    return g_has_pending_script ? g_pending_script : NULL;
}

bool cos_lua_take_pending_script(char* out, size_t out_sz) {
    if (!out || out_sz == 0 || !g_has_pending_script) {
        return false;
    }
    cos_strlcpy(out, g_pending_script, out_sz);
    g_pending_script[0] = '\0';
    g_has_pending_script = false;
    return true;
}

bool cos_lua_request_run(const char* script_path, char* reason, size_t reason_sz) {
    if (!cos_lua_get_config()->enabled) {
        lua_copy_reason(reason, reason_sz, "Lua is disabled in settings");
        return false;
    }

    char resolved[256];
    if (!cos_lua_resolve_script_path(script_path, true, resolved, sizeof(resolved), reason, reason_sz)) {
        return false;
    }

    cos_strlcpy(g_pending_script, resolved, sizeof(g_pending_script));
    g_has_pending_script = true;

    if (!cos_lua_backend_available()) {
        lua_copy_reason(reason, reason_sz, "Lua backend is not linked in this build yet; script queued");
        serial_puts("[LUA] Requested script queued but backend is unavailable: ");
        serial_puts(resolved);
        serial_puts("\n");
        return false;
    }

    lua_copy_reason(reason, reason_sz, "Lua script queued");
    serial_puts("[LUA] Requested script: ");
    serial_puts(resolved);
    serial_puts("\n");
    return true;
}
