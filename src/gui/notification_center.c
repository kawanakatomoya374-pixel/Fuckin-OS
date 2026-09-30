/**
 * notification_center.c - Notification Center Implementation
 * C-OS 4.0.0 - システム全体で利用可能な通知プッシュ機能
 */

#include "notification_center.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/serial.h"
#include "sync.h"
#include "task.h"

#define MAX_NOTIFICATIONS 256
#define MAX_LISTENERS 16

static notification_t g_notifications[MAX_NOTIFICATIONS];
static notification_callback_t g_listeners[MAX_LISTENERS];
static int g_notification_count = 0;
static int g_listener_count = 0;
static uint32_t g_next_notification_id = 1;
static bool g_initialized = false;

/* Guards g_notifications/g_notification_count. Before this, the array
 * was mutated both from whatever thread called notification_send()/
 * notification_dismiss() *and* from gui.c's draw_notifications(), which
 * ran on the GUI thread's own cadence and dismissed expired entries by
 * swapping in the last element mid-iteration. Now that expiry runs on
 * its own independent thread (notification_gc_thread below) instead of
 * being tied to frame draws, two threads can legitimately touch this
 * array at the same timer tick, so every access needs this lock. */
static mutex_t g_notif_mutex;
static bool g_gc_thread_started = false;

extern uint64_t get_timer_ticks(void);

/* ============================================================
 * 初期化
 * ============================================================ */

int notification_center_init(void) {
    if (g_initialized) return 0;
    
    serial_puts("[NOTIF] Initializing notification center...\n");
    
    memset(g_notifications, 0, sizeof(g_notifications));
    memset(g_listeners, 0, sizeof(g_listeners));
    
    g_notification_count = 0;
    g_listener_count = 0;
    g_next_notification_id = 1;
    mutex_init(&g_notif_mutex);
    g_initialized = true;
    
    serial_puts("[NOTIF] Notification center initialized\n");
    return 0;
}

/* ============================================================
 * 通知の送信
 * ============================================================ */

uint32_t notification_send(const char* title, const char* message, notify_type_t type, notify_priority_t priority) {
    return notification_send_with_duration(title, message, type, priority, 5000);
}

uint32_t notification_send_with_duration(const char* title, const char* message, notify_type_t type, notify_priority_t priority, uint32_t duration_ms) {
    if (!g_initialized) return 0;

    mutex_lock(&g_notif_mutex);

    if (g_notification_count >= MAX_NOTIFICATIONS) {
        mutex_unlock(&g_notif_mutex);
        return 0;
    }

    notification_t* notif = &g_notifications[g_notification_count++];
    notif->id = g_next_notification_id++;
    
    if (title) strncpy(notif->title, title, sizeof(notif->title) - 1);
    if (message) strncpy(notif->message, message, sizeof(notif->message) - 1);
    
    notif->type = type;
    notif->priority = priority;
    notif->timestamp = get_timer_ticks();
    notif->duration_ms = duration_ms;
    notif->read = false;
    process_t* current = process_get_current();
    notif->source_pid = current ? current->pid : 0;
    uint32_t id = notif->id;

    mutex_unlock(&g_notif_mutex);

    /* Listeners are invoked outside the lock so a listener that turns
     * around and calls back into notification_center (dismiss, send,
     * etc.) can't deadlock against a mutex we're still holding. */
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i]) {
            g_listeners[i](notif);
        }
    }
    
    return id;
}

/* ============================================================
 * 通知の管理
 * ============================================================ */

int notification_dismiss(uint32_t notification_id) {
    if (!g_initialized) return -1;

    mutex_lock(&g_notif_mutex);
    for (int i = 0; i < g_notification_count; i++) {
        if (g_notifications[i].id == notification_id) {
            /* 通知を削除（最後の要素と入れ替え） */
            g_notifications[i] = g_notifications[g_notification_count - 1];
            g_notification_count--;
            mutex_unlock(&g_notif_mutex);
            return 0;
        }
    }
    mutex_unlock(&g_notif_mutex);
    return -1;
}

int notification_dismiss_all(void) {
    if (!g_initialized) return -1;

    mutex_lock(&g_notif_mutex);
    g_notification_count = 0;
    mutex_unlock(&g_notif_mutex);
    return 0;
}

int notification_mark_as_read(uint32_t notification_id) {
    if (!g_initialized) return -1;

    mutex_lock(&g_notif_mutex);
    for (int i = 0; i < g_notification_count; i++) {
        if (g_notifications[i].id == notification_id) {
            g_notifications[i].read = true;
            mutex_unlock(&g_notif_mutex);
            return 0;
        }
    }
    mutex_unlock(&g_notif_mutex);
    return -1;
}

/* ============================================================
 * 通知の取得
 * ============================================================ */

notification_t* notification_get(uint32_t notification_id) {
    if (!g_initialized) return NULL;
    
    for (int i = 0; i < g_notification_count; i++) {
        if (g_notifications[i].id == notification_id) {
            return &g_notifications[i];
        }
    }
    return NULL;
}

notification_t* notification_get_all(int* count) {
    if (!g_initialized || !count) return NULL;
    
    *count = g_notification_count;
    return g_notifications;
}

/* notification_get_all() hands back a raw pointer into the live array
 * rather than a copy, so any caller that iterates the result across
 * more than a single instruction (e.g. gui.c's draw_notifications(),
 * which walks the list while drawing each entry) is reading memory that
 * notification_gc_thread can concurrently rewrite via
 * notification_dismiss()'s swap-with-last. Bracket such iteration with
 * these two calls so the GC thread can't touch the array mid-draw; keep
 * the bracketed section short (no thread_sleep, no blocking calls)
 * since it runs with the notification lock held. */
void notification_center_begin_read(void) {
    if (g_initialized) mutex_lock(&g_notif_mutex);
}

void notification_center_end_read(void) {
    if (g_initialized) mutex_unlock(&g_notif_mutex);
}

int notification_get_unread_count(void) {
    if (!g_initialized) return 0;
    
    int count = 0;
    for (int i = 0; i < g_notification_count; i++) {
        if (!g_notifications[i].read) {
            count++;
        }
    }
    return count;
}

/* ============================================================
 * 通知リスナー
 * ============================================================ */

int notification_register_listener(notification_callback_t callback) {
    if (!g_initialized || !callback || g_listener_count >= MAX_LISTENERS) return -1;
    
    g_listeners[g_listener_count++] = callback;
    return 0;
}

int notification_unregister_listener(notification_callback_t callback) {
    if (!g_initialized || !callback) return -1;
    
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i] == callback) {
            g_listeners[i] = g_listeners[g_listener_count - 1];
            g_listener_count--;
            return 0;
        }
    }
    return -1;
}

/* ============================================================
 * バックグラウンド有効期限管理スレッド
 *
 * Previously the duration_ms >= expiry check lived inline inside
 * gui.c's draw_notifications(), so a notification only ever expired
 * on a GUI redraw, and dismissal (an array mutation) happened *during*
 * the very iteration that was reading the array - a classic
 * modify-while-iterating bug, and one entirely tied to render cadence
 * rather than wall-clock time. This thread makes expiry an independent
 * scheduled activity: it wakes on its own cadence via thread_sleep
 * (genuinely descheduled, not spinning/polling every tick) and is
 * completely decoupled from whether the GUI thread is busy, blocked,
 * or not yet scheduled at all.
 * ============================================================ */
static void notification_gc_thread(void* arg) {
    (void)arg;
    for (;;) {
        thread_sleep(200);

        if (!g_initialized) continue;

        mutex_lock(&g_notif_mutex);
        uint64_t now = get_timer_ticks();
        /* Walk backwards so the swap-with-last removal never skips the
         * element that got swapped into the current slot. */
        for (int i = g_notification_count - 1; i >= 0; i--) {
            notification_t* n = &g_notifications[i];
            if (n->duration_ms > 0 && (now - n->timestamp) > (n->duration_ms / 10)) {
                g_notifications[i] = g_notifications[g_notification_count - 1];
                g_notification_count--;
            }
        }
        mutex_unlock(&g_notif_mutex);
    }
}

int notification_center_start_gc_thread(void) {
    if (!g_initialized) return -1;
    if (g_gc_thread_started) return 0;

    if (!thread_create_kernel("notif_gc", (void*)notification_gc_thread, NULL)) {
        serial_puts("[NOTIF] WARNING: failed to start GC thread\n");
        return -1;
    }

    g_gc_thread_started = true;
    serial_puts("[NOTIF] Background expiry thread started\n");
    return 0;
}
