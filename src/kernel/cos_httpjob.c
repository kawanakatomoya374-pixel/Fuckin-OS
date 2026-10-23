/* cos_httpjob.c - asynchronous HTTP downloads for ring-3 programs.
 *
 * A blocking HTTP syscall holds the syscall path for as long as the transfer takes, which freezes every ring-3 window.
 * Instead a program starts a job (SYS_NET_HTTP_START), the transfer runs on its own kernel thread, and the program polls
 * for the result and collects the body (SYS_NET_HTTP_POLL / _TAKE). The body stays byte-exact, and the HTTP status code and
 * Content-Type are reported, which the text-only SYS_NET_HTTP_GET cannot do. */
#include "serial.h"
#include "task.h"
#include "string.h"
#include "memory.h"
#include "cos_sysinfo.h"
#include "drivers/http.h"

#define HJ_MAX 4
enum { HJ_FREE = 0, HJ_STARTING, HJ_RUNNING, HJ_DONE, HJ_ABANDONED };

typedef struct {
    volatile int state;
    uint32_t owner;
    char url[HTTP_MAX_URL];
    http_client_t *hc;
    cos_http_result_t res;
    int ret;
} hj_t;

static hj_t g_jobs[HJ_MAX];

static void hj_release(hj_t *j) {
    if (j->hc) { http_destroy(j->hc); j->hc = NULL; }
    __atomic_store_n(&j->state, HJ_FREE, __ATOMIC_RELEASE);
}

static void hj_worker(void *arg) {
    hj_t *j = (hj_t *)arg;
    http_client_t *hc = http_create();
    j->ret = -1;
    memset(&j->res, 0, sizeof j->res);
    if (hc) {
        j->hc = hc;
        j->ret = http_get(hc, j->url);
        j->res.status = http_status_code(hc);
        j->res.error = j->ret != 0;
        j->res.body_len = http_response_length(hc);
        const char *ct = http_get_header(hc, "Content-Type");
        if (ct) {                           /* the header value is not NUL-terminated at the end of its line */
            size_t k = 0;
            while (ct[k] && ct[k] != '\r' && ct[k] != '\n' && k < sizeof(j->res.content_type) - 1) { j->res.content_type[k] = ct[k]; k++; }
        }
    } else {
        j->res.error = 1;
    }
    int expected = HJ_RUNNING;
    if (!__atomic_compare_exchange_n(&j->state, &expected, HJ_DONE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        hj_release(j);                      /* the owner gave up while we were running */
    }
    thread_exit(thread_get_current(), 0);
    for (;;) thread_yield();
}

static hj_t *hj_get(int id, uint32_t owner) {
    if (id < 0 || id >= HJ_MAX) return NULL;
    hj_t *j = &g_jobs[id];
    int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (st == HJ_FREE || st == HJ_STARTING || st == HJ_ABANDONED || j->owner != owner) return NULL;
    return j;
}

int cos_httpjob_start(const char *url, uint32_t owner) {
    for (int i = 0; i < HJ_MAX; i++) {
        hj_t *j = &g_jobs[i];
        int expected = HJ_FREE;
        if (!__atomic_compare_exchange_n(&j->state, &expected, HJ_STARTING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* a job whose owner exited can never be collected: reclaim it */
            process_t *p = process_get_by_pid(j->owner);
            int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
            if (st == HJ_DONE && (!p || p->state == TASK_ZOMBIE || p->state == TASK_UNUSED)) hj_release(j);
            continue;
        }
        j->owner = owner; j->hc = NULL; j->ret = -1;
        strncpy(j->url, url, sizeof j->url - 1); j->url[sizeof j->url - 1] = 0;
        __atomic_store_n(&j->state, HJ_RUNNING, __ATOMIC_RELEASE);
        if (!thread_create_kernel("http_job", (void *)hj_worker, j)) { hj_release(j); return -1; }
        return i;
    }
    return -1;
}

/* 0 = still running, 1 = finished (res filled), -1 = no such job */
int cos_httpjob_poll(int id, uint32_t owner, cos_http_result_t *res) {
    hj_t *j = hj_get(id, owner);
    if (!j) return -1;
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) != HJ_DONE) return 0;
    *res = j->res;
    return 1;
}

/* Copies up to cap body bytes into dst (a kernel-validated user pointer) and frees the job. Returns the byte count, or -1. */
int64_t cos_httpjob_take(int id, uint32_t owner, void *dst, uint64_t cap) {
    hj_t *j = hj_get(id, owner);
    if (!j || __atomic_load_n(&j->state, __ATOMIC_ACQUIRE) != HJ_DONE) return -1;
    int64_t n = -1;
    if (j->ret == 0 && j->hc) {
        const char *body = http_response_body(j->hc);
        uint64_t len = http_response_length(j->hc);
        n = (int64_t)(len < cap ? len : cap);
        if (body && n > 0) memcpy(dst, body, (size_t)n);
    }
    hj_release(j);
    return n;
}

void cos_httpjob_cancel(int id, uint32_t owner) {
    if (id < 0 || id >= HJ_MAX || g_jobs[id].owner != owner) return;
    hj_t *j = &g_jobs[id];
    int expected = HJ_RUNNING;
    if (__atomic_compare_exchange_n(&j->state, &expected, HJ_ABANDONED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == HJ_DONE) hj_release(j);
}
