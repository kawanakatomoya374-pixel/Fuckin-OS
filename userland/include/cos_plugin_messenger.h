/* cos_plugin_messenger.h - what a plugin of C-OS Message (app "messenger", directory /system/plugins/messenger) can do.
 *
 *   add_command      a slash command: typing "/name text" in the message box calls fn("text"). Built-in commands win; two plugins cannot share a name.
 *   add_outgoing_filter   every message the user sends (typed, or sent by a command or plugin) goes through fn(text, cap) first: change the text in place, or return
 *                    non-zero to keep it from being sent at all. Runs for a message to a person and to a group, before it is encrypted.
 *   add_incoming_hook     fn(chat, text) is called for every message that arrives (chat = the contact's or group's name).
 *   note             a card only the user sees (never sent, never saved): a title line, then lines of text.
 *   send             sends `text` to the chat that is open, as if it had been typed (outgoing filters apply). 0, or -1 when no chat is open.
 * Everything a plugin registers is removed when it is unloaded. The functions are only valid between init and fini. */
#ifndef COS_PLUGIN_MESSENGER_H
#define COS_PLUGIN_MESSENGER_H
#include <stdint.h>
#include <stddef.h>
#define MSGRP_ABI 1u
typedef struct {
    uint32_t abi, size;
    int  (*add_command)(const char *name, const char *help, void (*fn)(const char *arg));
    int  (*add_outgoing_filter)(int (*fn)(char *text, size_t cap));
    int  (*add_incoming_hook)(void (*fn)(const char *chat, const char *text));
    void (*note)(const char *title, const char *body);
    void (*status)(const char *text);
    int  (*send)(const char *text);
    const char *(*chat_name)(void);                 /* the open chat, or NULL */
    uint64_t (*now_ms)(void);
    uint32_t (*random32)(void);
    int  (*snprintf)(char *buf, size_t size, const char *fmt, ...);
} msgr_api_t;
#endif
