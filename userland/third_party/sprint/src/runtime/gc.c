/* GC: precise, non-moving mark & sweep with no C recursion. */
#include "value_int.h"

static void mark_val(VM *vm, SpValue v);

/* ---------------------------------------------------------------- */
/* GC: precise, non-moving mark & sweep (no C recursion)            */
/* ---------------------------------------------------------------- */
static void gray_push(VM *vm, Obj *o) {
    if (vm->ngray == vm->capgray) {
        vm->capgray = vm->capgray ? vm->capgray * 2 : 1024;
        vm->gray = realloc(vm->gray, vm->capgray * sizeof(Obj *));
        if (!vm->gray) { fputs("out of memory during GC\n", stderr); exit(14); }
    }
    vm->gray[vm->ngray++] = o;
}

static void mark_obj(VM *vm, Obj *o) {
    if (!o || o->marked) return;
    o->marked = 1;
    if (o->type == T_STRING) return;
    gray_push(vm, o);
}


static void mark_exec(VM *vm, const ExecState *st) {
    for (const SpValue *p = st->stack; p < st->sp; p++) mark_val(vm, *p);
    for (int i = 0; i < st->nframes; i++) {
        mark_obj(vm, (Obj *)st->frames[i].fn);
        mark_val(vm, st->frames[i].uev);
    }
    for (int i = 0; i < st->ndv; i++) mark_val(vm, st->dvals[i]);
    for (int i = 0; i < st->ndr; i++)
        if (st->drecs[i].method) mark_obj(vm, (Obj *)st->drecs[i].method);
}

static void mark_val(VM *vm, SpValue v) {
    if (v.type >= T_STRING && v.type <= T_CELL) mark_obj(vm, v.as.o);
}

void gc_mark(VM *vm, SpValue v) { mark_val(vm, v); }

static void blacken(VM *vm, Obj *o) {
    switch (o->type) {
    case T_ARRAY: {
        SArray *a = (SArray *)o;
        for (int64_t i = 0; i < a->len; i++) mark_val(vm, a->items[i]);
        break;
    }
    case T_TABLE: {
        STable *t = (STable *)o;
        for (int64_t i = 0; i < t->nent; i++)
            if (t->ents[i].key.type != T_NIL) {
                mark_val(vm, t->ents[i].key);
                mark_val(vm, t->ents[i].val);
            }
        break;
    }
    case T_FUNCTION: {
        SFunc *f = (SFunc *)o;
        for (int i = 0; i < f->nup; i++) mark_obj(vm, (Obj *)f->up[i]);
        break;
    }
    case T_CELL: mark_val(vm, ((SCell *)o)->v); break;
    case T_ITERATOR: mark_val(vm, ((SIterator *)o)->held); break;
    case T_NATIVE: {
        SNativeObj *no = (SNativeObj *)o;
        if (no->cls->trace) no->cls->trace(vm, no);
        break;
    }
    case T_COROUTINE: {
        SCoroutine *co = (SCoroutine *)o;
        mark_obj(vm, (Obj *)co->fn);
        if (co->status == CO_SUSPENDED && co->block) mark_exec(vm, &co->st);
        break;
    }
    case T_CLASS: {
        SClass *c = (SClass *)o;
        mark_obj(vm, (Obj *)c->name);
        mark_obj(vm, (Obj *)c->parent);
        mark_obj(vm, (Obj *)c->fields);
        mark_obj(vm, (Obj *)c->methods);
        mark_obj(vm, (Obj *)c->ctor);
        break;
    }
    case T_INSTANCE: {
        SInstance *in = (SInstance *)o;
        mark_obj(vm, (Obj *)in->cls);
        for (int i = 0; i < in->nslots; i++) mark_val(vm, in->slots[i]);
        break;
    }
    case T_MODULE: {
        SModule *m = (SModule *)o;
        mark_obj(vm, (Obj *)m->name);
        mark_obj(vm, (Obj *)m->exports);
        break;
    }
    case T_ERROR: {
        SError *e = (SError *)o;
        mark_obj(vm, (Obj *)e->kind);
        mark_obj(vm, (Obj *)e->message);
        mark_obj(vm, (Obj *)e->file);
        mark_obj(vm, (Obj *)e->trace);
        mark_val(vm, e->cause);
        mark_val(vm, e->data);
        mark_val(vm, e->suppressed);
        break;
    }
    default: break;
    }
}

static size_t obj_size(Obj *o) {
    switch (o->type) {
    case T_STRING: return offsetof(SString, data) + ((SString *)o)->len + 1;
    case T_ARRAY: return sizeof(SArray) + (size_t)((SArray *)o)->cap * sizeof(SpValue);
    case T_TABLE: {
        STable *t = (STable *)o;
        return sizeof(STable) + (size_t)t->cap * sizeof(TEntry) + (size_t)t->idxcap * sizeof(int32_t);
    }
    case T_FUNCTION: return sizeof(SFunc) + (size_t)((SFunc *)o)->nup * sizeof(SCell *);
    case T_CELL: return sizeof(SCell);
    case T_CLASS: return sizeof(SClass);
    case T_COROUTINE: return sizeof(SCoroutine) + ((SCoroutine *)o)->block_size;
    case T_INSTANCE: return offsetof(SInstance, slots) + (size_t)((SInstance *)o)->nslots * sizeof(SpValue);
    case T_MODULE: return sizeof(SModule);
    case T_ITERATOR: return sizeof(SIterator);
    case T_ERROR: return sizeof(SError);
    case T_NATIVE: return sizeof(SNativeObj) + ((SNativeObj *)o)->size;
    default: return sizeof(Obj);
    }
}

static void free_obj(VM *vm, Obj *o) {
    size_t sz = obj_size(o);
    switch (o->type) {
    case T_ARRAY: free(((SArray *)o)->items); break;
    case T_TABLE: free(((STable *)o)->ents); free(((STable *)o)->idx); break;
    case T_FUNCTION: free(((SFunc *)o)->up); break;
    case T_CLASS: free(((SClass *)o)->dflt); break;
    case T_COROUTINE: free(((SCoroutine *)o)->block); break;
    case T_NATIVE: {
        SNativeObj *no = (SNativeObj *)o;
        if (no->cls->release) no->cls->release(no->data);
        free(no->data);
        break;
    }
    default: break;
    }
    obj_release_block(vm, o);
    vm->bytes -= sz;
}

void vm_gc(VM *vm) {
    vm->gc_runs++;
    vm->ngray = 0;
    memset(vm->ic, 0, sizeof vm->ic);
    memset(vm->mc, 0, sizeof vm->mc);
    {
        ExecState cur;
        exec_save(vm, &cur);
        mark_exec(vm, &cur);
        for (SCoroutine *c = vm->cur_co; c; c = c->resumer) {
            mark_exec(vm, &c->caller);
            mark_obj(vm, (Obj *)c);
        }
    }
    if (vm->prog) {
        for (int i = 0; i < vm->prog->nslots; i++) mark_val(vm, vm->slots[i]);
        for (int i = 0; i < vm->prog->nlinks; i++) mark_val(vm, vm->links[i]);
        for (int i = 0; i < vm->prog->nprotos; i++) {
            Proto *p = vm->prog->protos[i];
            for (int k = 0; k < p->nconsts; k++) mark_val(vm, p->consts[k]);
            jit_roots(p, mark_val, vm);
        }
    }
    for (int i = 0; i < vm->nroots; i++) mark_val(vm, vm->roots[i]);
    for (int i = 0; i < vm->nnroot; i++) mark_val(vm, vm->nroot_stack[i]);
    for (const HostRef *r = vm->hrefs; r; r = r->next) mark_val(vm, r->v);
    mark_val(vm, vm->pend_obj);
    mark_val(vm, vm->uncaught);
    for (int i = 0; i < 17; i++) mark_obj(vm, (Obj *)vm->type_names[i]);
    while (vm->ngray) blacken(vm, vm->gray[--vm->ngray]);

    Obj **pp = &vm->objs;
    while (*pp) {
        Obj *o = *pp;
        if (o->marked) {
            o->marked = 0;
            pp = &o->next;
        } else {
            *pp = o->next;
            if (o->type == T_INSTANCE) {         /* the common garbage: no external storage, so the block goes straight back to its size class */
                size_t hs = offsetof(SInstance, slots) + (size_t)((SInstance *)o)->nslots * sizeof(SpValue);
                vm->bytes -= hs;
                if (hs <= SP_POOL_MAX) {
                    size_t cl = (hs + 15) >> 4;
                    *(void **)(void *)o = vm->pool_free[cl];
                    vm->pool_free[cl] = o;
                } else {
                    free(o);
                }
            } else {
                free_obj(vm, o);
            }
        }
    }
    size_t live = vm->bytes;
    vm->gc_next = live * 2 < (size_t)1 << 20 ? (size_t)1 << 20 : live * 2;
    if (vm->gc_next > vm->lim.max_heap_bytes) vm->gc_next = vm->lim.max_heap_bytes;
    if (vm->gc_next < live + 4096) vm->gc_next = live + 4096;
    if (vm->gc_stress) vm->gc_next = live + 1;   /* SPRINT_GC_STRESS=1: collect at every allocation point */
}

void vm_free(VM *vm) {
    lib_ext_free(vm);
    Obj *o = vm->objs;
    while (o) {
        Obj *n = o->next;
        switch (o->type) {
        case T_ARRAY: free(((SArray *)o)->items); break;
        case T_TABLE: free(((STable *)o)->ents); free(((STable *)o)->idx); break;
        case T_FUNCTION: free(((SFunc *)o)->up); break;
        case T_CLASS: free(((SClass *)o)->dflt); break;
        case T_COROUTINE: free(((SCoroutine *)o)->block); break;
        case T_NATIVE: {
            SNativeObj *no = (SNativeObj *)o;
            if (no->cls->release) no->cls->release(no->data);
            free(no->data);
            break;
        }
        default: break;
        }
        obj_release_block(vm, o);
        o = n;
    }
    for (int i = 0; i < vm->npool_chunks; i++) free(vm->pool_chunks[i]);
    free(vm->pool_chunks);
    free(vm->main_exec.stack);
    free(vm->main_exec.frames);
    free(vm->main_exec.handlers);
    free(vm->main_exec.drecs);
    free(vm->main_exec.dvals);
    free(vm->outbuf);
    free(vm->roots);
    free(vm->nroot_stack);
    while (vm->hrefs) { HostRef *r = vm->hrefs; vm->hrefs = r->next; free(r); }
    for (int i = 0; i < vm->nhmods; i++) {
        free(vm->hmods[i]->name);
        free(vm->hmods[i]->fns);
        free(vm->hmods[i]->hd);
        free(vm->hmods[i]);
    }
    free(vm->hmods);
    free(vm->hdiag);
    free(vm->gray);
    free(vm->slots);
    free(vm->links);
    free(vm);
}
