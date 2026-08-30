/**
 * notification_center.h - Notification Center System
 * C-OS 4.0.0 - システム全体で利用可能な通知プッシュ機能
 */

#ifndef NOTIFICATION_CENTER_H
#define NOTIFICATION_CENTER_H

#include <stdint.h>
#include <stdbool.h>

/* 通知の優先度 */
typedef enum {
    NOTIFY_PRIORITY_LOW = 0,
    NOTIFY_PRIORITY_NORMAL = 1,
    NOTIFY_PRIORITY_HIGH = 2,
    NOTIFY_PRIORITY_CRITICAL = 3,
} notify_priority_t;

/* 通知のタイプ */
typedef enum {
    NOTIFY_TYPE_INFO = 0,
    NOTIFY_TYPE_WARNING = 1,
    NOTIFY_TYPE_ERROR = 2,
    NOTIFY_TYPE_SUCCESS = 3,
    NOTIFY_TYPE_SYSTEM = 4,
} notify_type_t;

/* 通知構造体 */
typedef struct {
    uint32_t id;
    char title[128];
    char message[512];
    notify_type_t type;
    notify_priority_t priority;
    uint64_t timestamp;
    uint32_t duration_ms;  /* 表示時間（0 = 永続） */
    bool read;
    uint32_t source_pid;   /* 送信元プロセスID */
} notification_t;

/* 初期化 */
int notification_center_init(void);

/* 通知の送信 */
uint32_t notification_send(const char* title, const char* message, notify_type_t type, notify_priority_t priority);
uint32_t notification_send_with_duration(const char* title, const char* message, notify_type_t type, notify_priority_t priority, uint32_t duration_ms);

/* 通知の管理 */
int notification_dismiss(uint32_t notification_id);
int notification_dismiss_all(void);
int notification_mark_as_read(uint32_t notification_id);

/* 通知の取得 */
notification_t* notification_get(uint32_t notification_id);
notification_t* notification_get_all(int* count);
int notification_get_unread_count(void);

/* Bracket any multi-instruction iteration over the pointer returned by
 * notification_get_all() with these two calls - see notification_center.c
 * for why (background GC thread can mutate the array concurrently). */
void notification_center_begin_read(void);
void notification_center_end_read(void);

/* Starts the background thread that expires timed-out notifications
 * on its own schedule instead of piggybacking on GUI redraws. Call
 * once, after notification_center_init() and after the scheduler is
 * running (kernel_main does this right after gui_init()). */
int notification_center_start_gc_thread(void);

/* 通知リスナー */
typedef void (*notification_callback_t)(notification_t* notif);
int notification_register_listener(notification_callback_t callback);
int notification_unregister_listener(notification_callback_t callback);

#endif /* NOTIFICATION_CENTER_H */
