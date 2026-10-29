/* VM creation and error construction (the interpreter loop is in vm.c). */
#include "vm_int.h"

/* ---------------------------------------------------------------- */
/* creation                                                          */
/* ---------------------------------------------------------------- */
VM *vm_new(const Limits *lim, int caps) {
    VM *vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;
    vm->lim = *lim;
    jit_configure(vm);
    {
        const char *gs = getenv("SPRINT_GC_STRESS");
        vm->gc_stress = gs && gs[0] == '1';
        if (vm->gc_stress) vm->gc_next = 1;
    }
    vm->caps = caps;
    vm->gc_next = (size_t)1 << 20;
    vm->h_append = str_hash_calc("append", 6);
    vm->stack = malloc(sizeof(SpValue) * (size_t)vm->lim.max_operand_stack);
    vm->frames = malloc(sizeof(Frame) * (size_t)vm->lim.max_call_depth);
    vm->handlers = malloc(sizeof(Handler) * (size_t)vm->lim.max_handlers);
    vm->drecs = malloc(sizeof(DeferRec) * (size_t)vm->lim.max_defer_records);
    vm->dvals = malloc(sizeof(SpValue) * (size_t)vm->lim.max_defer_values);
    vm->outcap = vm->lim.stdout_buffer_bytes < 256 ? 256 : vm->lim.stdout_buffer_bytes;
    vm->outbuf = malloc(vm->outcap);
    if (!vm->stack || !vm->frames || !vm->handlers || !vm->drecs || !vm->dvals || !vm->outbuf) {
        exec_save(vm, &vm->main_exec);
        vm_free(vm);
        return NULL;
    }
    vm->stack_end = vm->stack + vm->lim.max_operand_stack;
    vm->sp = vm->stack;
    vm->max_frames = (int)vm->lim.max_call_depth;
    vm->hcap = (int)vm->lim.max_handlers;
    vm->drcap = (int)vm->lim.max_defer_records;
    vm->dvcap = (int)vm->lim.max_defer_values;
    exec_save(vm, &vm->main_exec);
    vm->pend_obj = v_nil();
    vm->uncaught = v_nil();
    vm->fatal_armed = 0;
    vm->max_reentry = 32;
    for (int i = 0; i <= T_UNINIT; i++) {
        SpValue tmp = v_obj(i, NULL);
        const char *nm = type_name(tmp);
        vm->type_names[i] = vm_new_string(vm, nm, strlen(nm));
    }
    lib_init(vm);
    return vm;
}

/* ---------------------------------------------------------------- */
/* errors                                                            */
/* ---------------------------------------------------------------- */
int vm_set_error(VM *vm, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(vm->pend_msg, sizeof vm->pend_msg, fmt, ap);
    va_end(ap);
    vm->pend_code = code;
    vm->pend_obj = v_nil();
    return 1;
}

void vm_fatal(VM *vm, int status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(vm->fatal_msg, sizeof vm->fatal_msg, fmt, ap);
    va_end(ap);
    vm->fatal_status = status;
    if (vm->fatal_armed) longjmp(vm->fatal_jb, 1);
    (void)fprintf(stderr, "fatal: %s\n", vm->fatal_msg);
    exit(status == RS_MEMORY ? 14 : status);
}

static void line_for(const Proto *p, const uint8_t *ipc, int *line, int *col) {
    uint32_t off = (uint32_t)(ipc - p->code);
    *line = 0;
    *col = 0;
    for (int i = 0; i < p->nlines; i++) {
        if (p->lines[i].pc > off) break;
        *line = p->lines[i].line;
        *col = p->lines[i].col;
    }
}

static const char *proto_file(const VM *vm, const Proto *p) {
    return vm->prog->mods[p->mod].file;
}

static const char *frame_name(const Frame *f) {
    if (f->fn->nat) return f->fn->nat->name;
    if (f->fn->proto->is_module) return "<module>";
    return f->fn->proto->name ? f->fn->proto->name : "<anonymous>";
}

static void trace_fmt(VM *vm, Out *o) {
    int n = vm->nframes;
    int maxf = (int)vm->lim.max_trace_frames;
    int head = n > maxf ? maxf / 2 : n;
    int tail = n > maxf ? maxf - head : 0;
    char buf[640];
    for (int i = 0; i < n; i++) {
        if (n > maxf && i >= head && i < n - tail) {
            if (i == head) {
                int k = snprintf(buf, sizeof buf, "  ... %d frames omitted ...\n", n - head - tail);
                out_put(o, buf, (size_t)k);
            }
            continue;
        }
        const Frame *f = &vm->frames[i];
        int line, col;
        const uint8_t *ip = i == n - 1 ? f->pc : f->pc - 1;
        line_for(f->fn->proto, ip, &line, &col);
        int k = snprintf(buf, sizeof buf, "  %s:%d:%d in %s\n", proto_file(vm, f->fn->proto), line, col, frame_name(f));
        if (k < 0) k = 0;
        if ((size_t)k >= sizeof buf) k = (int)sizeof buf - 1;
        out_put(o, buf, (size_t)k);
    }
}

SError *vm_make_error(VM *vm, int code, SString *kind, SString *msg) {
    SError *e = vm_new_error_obj(vm);
    e->kind = kind;
    e->message = msg;
    e->code = code;
    e->cause = v_nil();
    e->data = v_nil();
    e->suppressed = v_nil();
    if (vm->nframes > 0 && vm->prog) {
        const Frame *top = &vm->frames[vm->nframes - 1];
        int line = 0, col = 0;
        line_for(top->fn->proto, top->pc, &line, &col);
        e->line = line;
        e->col = col;
        e->file = vm_new_string(vm, proto_file(vm, top->fn->proto), strlen(proto_file(vm, top->fn->proto)));
        Out m = {0};
        trace_fmt(vm, &m);
        SString *t = vm_alloc_string(vm, m.len);
        Out w = {0};
        w.dst = t->data;
        w.cap = m.len;
        trace_fmt(vm, &w);
        string_finish(t);
        e->trace = t;
    } else {
        e->file = vm_new_string(vm, "", 0);
        e->trace = vm_new_string(vm, "", 0);
    }
    return e;
}

SError *vm_new_error(VM *vm, int code, const char *kind, size_t klen, const char *msg, size_t mlen) {
    SString *k = vm_new_string(vm, kind, klen);
    SString *m = vm_new_string(vm, msg, mlen);
    return vm_make_error(vm, code, k, m);
}

const uint8_t *vm_error_ipc(const VM *vm) { return vm->nframes ? vm->frames[vm->nframes - 1].pc : NULL; }
