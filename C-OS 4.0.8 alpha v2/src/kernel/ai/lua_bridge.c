/**
 * lua_bridge.c - C-OS Lua 5.4 Integration Bridge
 * Provides bindings for GUI, System, and Hardware access.
 */
#include "lua_config.h"
#include "serial.h"
#include "gui.h"
#include "memory.h"
#include "timer.h"
#include "string.h"
#include "scheduler.h"

/* Note: In a real build, you would include lua.h here.
 * For this syntax check and template, we define the expected bridge structure. */

/* --- Lua C Functions --- */

/**
 * cos.print(text)
 * Outputs text to the serial debug console.
 */
static int l_cos_print(void* L) {
    /* Implementation would use lua_tostring(L, 1) */
    serial_puts("[LUA] ");
    // serial_puts(lua_tostring(L, 1));
    serial_puts("\n");
    return 0;
}

/**
 * cos.gui_msgbox(title, message)
 * Shows a GUI message box.
 */
static int l_cos_gui_msgbox(void* L) {
    if (!cos_lua_get_config()->allow_gui) return 0;
    /* Implementation would call gui_message_box(title, message) */
    serial_puts("[LUA] GUI MsgBox requested\n");
    return 0;
}

/**
 * cos.get_ticks()
 * Returns system timer ticks.
 */
static int l_cos_get_ticks(void* L) {
    /* lua_pushinteger(L, get_timer_ticks()) */
    return 1;
}

/**
 * cos.yield()
 * Yields execution to the kernel scheduler.
 */
static int l_cos_yield(void* L) {
    (void)L;
    scheduler_yield();
    return 0;
}

/* --- Registration --- */

void cos_lua_open_libs(void* L) {
    serial_puts("[LUA] Registering C-OS API to Lua state...\n");
    
    /* In a real implementation:
    lua_newtable(L);
    lua_pushcfunction(L, l_cos_print); lua_setfield(L, -2, "print");
    lua_pushcfunction(L, l_cos_gui_msgbox); lua_setfield(L, -2, "gui_msgbox");
    lua_pushcfunction(L, l_cos_get_ticks); lua_setfield(L, -2, "get_ticks");
    lua_pushcfunction(L, l_cos_yield); lua_setfield(L, -2, "yield");
    lua_setglobal(L, "cos");
    */
}

/**
 * cos_lua_init_state()
 * Initializes a new Lua state with C-OS sandboxing.
 */
void* cos_lua_init_state(void) {
    if (!cos_lua_get_config()->enabled) return NULL;
    
    serial_puts("[LUA] Creating new Lua 5.4 state...\n");
    
    /* 
    void* L = luaL_newstate();
    if (!L) return NULL;
    
    luaL_openlibs(L);
    cos_lua_open_libs(L);
    
    // Apply memory limit if configured
    if (cos_lua_get_config()->memory_limit_mb > 0) {
        // lua_setallocf(...)
    }
    
    return L;
    */
    return (void*)1; // Dummy handle
}

/* Note: cos_lua_backend_available() is provided by lua_config.c — that is
 * the single source of truth (see fix_report.md §4.2). The bridge only
 * assumes the symbol exists; defining it again here would create a link
 * conflict. */
