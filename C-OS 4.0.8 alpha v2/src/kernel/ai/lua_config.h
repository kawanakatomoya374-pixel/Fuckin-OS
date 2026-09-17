#ifndef COS_LUA_CONFIG_H
#define COS_LUA_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "../../include/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    char scripts_path[256];
    char autorun_script[256];
    bool sandbox;
    bool debug;
    int memory_limit_mb;  /* 0 = unlimited */
    int timeout_ms;       /* 0 = unlimited */
    bool allow_fs;
    bool allow_net;
    bool allow_gui;
    char lua_version[16];
} cos_lua_config_t;

void cos_lua_config_defaults(cos_lua_config_t* cfg);
void cos_lua_set_config(const cos_lua_config_t* cfg);
const cos_lua_config_t* cos_lua_get_config(void);
bool cos_lua_backend_available(void);

bool cos_lua_validate_script_path(const char* path, bool require_lua_suffix,
                                  char* reason, size_t reason_sz);
bool cos_lua_resolve_script_path(const char* requested, bool require_lua_suffix,
                                 char* out, size_t out_sz,
                                 char* reason, size_t reason_sz);
const char* cos_lua_get_pending_script(void);
bool cos_lua_take_pending_script(char* out, size_t out_sz);
bool cos_lua_request_run(const char* script_path, char* reason, size_t reason_sz);

#ifdef __cplusplus
}
#endif

#endif
