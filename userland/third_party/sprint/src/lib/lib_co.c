/* Module coroutine (SPEC 13); resume/close return 2 = "switch context". */
#include "lib_int.h"

/* ---------------------------------------------------------------- */
/* coroutine (SPEC 13); resume/close return 2 = "switch context"      */
/* ---------------------------------------------------------------- */
static int need_co(VM *vm, SpValue v) {
    if (v.type != T_COROUTINE) return vm_set_error(vm, E_TYPE, "expected a coroutine, not %s", type_name(v));
    return 0;
}
static const char *co_status_name(const SCoroutine *co) {
    switch (co->status) {
    case CO_SUSPENDED: return "suspended";
    case CO_RUNNING: return "running";
    case CO_NORMAL: return "normal";
    case CO_DEAD: return "dead";
    default: return "dead";
    }
}
static int co_create(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    int64_t slots = 4096;
    if (argv[0].type != T_FUNCTION || AS_FUNC(argv[0])->nat)
        return vm_set_error(vm, E_TYPE, "coroutine.create() requires a Sprint function");
    if (AS_FUNC(argv[0])->proto->nrequired != 0)
        return vm_set_error(vm, E_TYPE, "a coroutine body must be callable without arguments");
    if (argc > 1 && need_int(vm, argv[1], "stack_slots", &slots)) return 1;
    if (slots < 64 || slots > (int64_t)vm->lim.max_operand_stack)
        return vm_set_error(vm, E_VALUE, "stack_slots must be between 64 and %u", vm->lim.max_operand_stack);
    *ret = v_obj(T_COROUTINE, vm_new_coroutine(vm, AS_FUNC(argv[0]), slots));
    return 0;
}
static int co_resume(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    if (need_co(vm, argv[0])) return 1;
    SCoroutine *co = AS_CO(argv[0]);
    if (co->status != CO_SUSPENDED)
        return vm_set_error(vm, E_RUNTIME, "cannot resume a %s coroutine", co_status_name(co));
    co->resume_for = 0;
    vm->sw_co = co;
    vm->sw_kind = SW_RESUME;
    return 2;
}
static int co_status(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (need_co(vm, argv[0])) return 1;
    const char *s = co_status_name(AS_CO(argv[0]));
    *ret = vstr(vm, s, strlen(s));
    return 0;
}
static int co_close(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    if (need_co(vm, argv[0])) return 1;
    SCoroutine *co = AS_CO(argv[0]);
    if (co->status == CO_DEAD) return 0;
    if (co->status != CO_SUSPENDED)
        return vm_set_error(vm, E_RUNTIME, "cannot close a %s coroutine", co_status_name(co));
    if (!co->started) {
        co->status = CO_DEAD;
        vm_co_release(vm, co);
        return 0;
    }
    co->closing = 1;
    co->resume_for = 0;
    vm->sw_co = co;
    vm->sw_kind = SW_CLOSE;
    return 2;
}
static int co_current(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv;
    *ret = vm->cur_co ? v_obj(T_COROUTINE, vm->cur_co) : v_nil();
    return 0;
}
const NativeDesc co_fns[] = {
    {"create", 1, 2, co_create}, {"resume", 1, 2, co_resume}, {"status", 1, 1, co_status},
    {"close", 1, 1, co_close}, {"current", 0, 0, co_current}, {NULL, 0, 0, NULL}};
