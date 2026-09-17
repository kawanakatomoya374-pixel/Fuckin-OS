/**
 * notification_center.h - Notification Center
 * 
 * C-OS 4.0.8 alpha 通知センター
 */

#ifndef NOTIFICATION_CENTER_H
#define NOTIFICATION_CENTER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NOTIFY_INFO,
    NOTIFY_SUCCESS,
    NOTIFY_WARNING,
    NOTIFY_ERROR,
} notify_type_t;

typedef struct {
    char title[64];
    char message[256];
    notify_type_t type;
    uint64_t timestamp;
    bool read;
} notification_t;

typedef struct {
    notification_t notifications[32];
    int count;
    bool visible;
} notification_center_t;

int notify_init(void);
void notify_push(const char* title, const char* message, notify_type_t type);
void notify_draw(void);

#endif
