/* cos_js_scheduler.h - owner-thread pump for bounded QuickJS web timers. */
#ifndef COS_JS_SCHEDULER_H
#define COS_JS_SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Execute at most a bounded batch of due JavaScript timer/animation callbacks.
 * This must be called from the GUI/NetSurf owner thread, never an IRQ or AP
 * worker. */
void cos_js_pump_timers(void);

/* Complete at most one queued browser fetch/XMLHttpRequest operation. This
 * shares the GUI owner thread with DOM mutation and must never run in IRQ/AP
 * context. */
void cos_js_pump_web_requests(void);

#ifdef __cplusplus
}
#endif

#endif /* COS_JS_SCHEDULER_H */
