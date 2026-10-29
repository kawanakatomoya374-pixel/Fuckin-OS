/* Bytecode verifier (SPEC 17.9): a loaded image is checked before it may run. */
#include "image_int.h"

/* ================================================================ */
/* verifier (SPEC 17.9)                                              */
/* ================================================================ */
typedef struct VErr {
    char *msg;
    size_t cap;
} VErr;

static int verr(VErr *e, const char *fmt, ...) SP_PRINTF(2, 3);

static int verr(VErr *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(e->msg, e->cap, fmt, ap);
    va_end(ap);
    return 1;
}

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static int32_t rd32s(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)v;
}

static int is_terminator(int op) {
    return op == OP_RETURN || op == OP_RETURN_NIL || op == OP_THROW || op == OP_JUMP || op == OP_PANIC ||
           op == OP_ASSERT_FAIL || op == OP_MATCH_FAIL || op == OP_RAISE_ERR;
}

/* const must be a string */
static int const_is_str(const Proto *p, unsigned k) { return (int)k < p->nconsts && p->consts[k].type == T_STRING; }

typedef struct Flow {
    int *height;
    int *tdepth;
    uint32_t *work;
    size_t nwork;
} Flow;

static int flow_push(VErr *e, const Proto *p, Flow *fl, uint32_t pc, int h, int td, const char *why) {
    if (h < 0 || h > p->max_stack)
        return verr(e, "%s: operand stack height %d out of range (max_stack %d) at pc %u", p->name ? p->name : "<anon>", h, p->max_stack, pc);
    if (fl->height[pc] < 0) {
        fl->height[pc] = h;
        fl->tdepth[pc] = td;
        fl->work[fl->nwork++] = pc;
        return 0;
    }
    if (fl->height[pc] != h || fl->tdepth[pc] != td)
        return verr(e, "%s: inconsistent stack/handler state at pc %u (%s)", p->name ? p->name : "<anon>", pc, why);
    return 0;
}

static int verify_proto(const Program *pg, const Proto *p, VErr *e) {
    const char *nm = p->name ? p->name : "<anon>";
    if (p->nrequired > p->nparams || p->nparams < 0 || p->nrequired < 0) return verr(e, "%s: bad parameter counts", nm);
    if (p->ndefaults != p->nparams - p->nrequired) return verr(e, "%s: ndefaults mismatch", nm);
    if (p->code_len == 0) return verr(e, "%s: empty code", nm);
    if (p->mod < 0 || p->mod >= pg->nmods) return verr(e, "%s: bad module index", nm);
    if (p->nlocals < p->nparams + (p->has_rest ? 1 : 0)) return verr(e, "%s: too few locals", nm);
    if (p->max_stack > 65535) return verr(e, "%s: max_stack too large", nm);

    uint32_t n = p->code_len;
    uint8_t *start = calloc(n + 1u, 1);
    int *height = xalloc((size_t)(n + 1u) * sizeof(int));
    int *tdepth = xalloc((size_t)(n + 1u) * sizeof(int));
    uint32_t *work = xalloc((size_t)(n + 1u) * sizeof(uint32_t) * 2u);
    if (!start) exit(14);
    int rc = 1;
    /* pass 1: instruction boundaries */
    uint32_t pc = 0;
    int last_op = -1;
    while (pc < n) {
        int op = p->code[pc];
        int len = op_len(op);
        if (len == 0) { (void)verr(e, "%s: invalid opcode 0x%02X at pc %u", nm, (unsigned)op, pc); goto done; }
        if (pc + (uint32_t)len > n) { (void)verr(e, "%s: truncated instruction at pc %u", nm, pc); goto done; }
        start[pc] = 1;
        last_op = op;
        pc += (uint32_t)len;
    }
    if (!is_terminator(last_op)) { (void)verr(e, "%s: code falls off the end", nm); goto done; }
    for (int k = 0; k <= p->ndefaults; k++) {
        uint32_t d = p->default_pcs ? p->default_pcs[k] : 0u;
        if (d >= n || !start[d]) { (void)verr(e, "%s: default entry %d is not an instruction boundary", nm, k); goto done; }
    }
    /* pass 2: operand ranges and jump targets */
    for (pc = 0; pc < n; pc += (uint32_t)op_len(p->code[pc])) {
        int op = p->code[pc];
        const uint8_t *a = p->code + pc + 1;
        uint32_t end = pc + (uint32_t)op_len(op);
        unsigned u = 0;
        switch (op) {
        case OP_LOAD_LOCAL: case OP_STORE_LOCAL: case OP_MAKE_CELL: case OP_LOAD_CELL: case OP_STORE_CELL:
        case OP_DEFER_SCOPE: case OP_RUN_DEFERS: case OP_CTOR_CHECK:
            u = rd16(a);
            if ((int)u >= p->nlocals) { (void)verr(e, "%s: local %u out of range at pc %u", nm, u, pc); goto done; }
            break;
        case OP_FOR_PREP: case OP_FOR_NEXT: case OP_RANGE_PREP: case OP_RANGE_NEXT:
            u = rd16(a);
            if ((int)u + 2 >= p->nlocals) { (void)verr(e, "%s: iteration slots out of range at pc %u", nm, pc); goto done; }
            break;
        case OP_LOAD_UPVAL: case OP_STORE_UPVAL:
            if ((int)rd16(a) >= p->nupvals) { (void)verr(e, "%s: upvalue out of range at pc %u", nm, pc); goto done; }
            break;
        case OP_LOAD_MODULE: case OP_STORE_MODULE:
            if ((int)rd16(a) >= pg->nslots) { (void)verr(e, "%s: module slot out of range at pc %u", nm, pc); goto done; }
            break;
        case OP_LOAD_LINK:
            if ((int)rd16(a) >= pg->nlinks) { (void)verr(e, "%s: link out of range at pc %u", nm, pc); goto done; }
            break;
        case OP_CONST:
            if ((int)rd16(a) >= p->nconsts) { (void)verr(e, "%s: constant out of range at pc %u", nm, pc); goto done; }
            break;
        case OP_GET_MEMBER: case OP_SET_MEMBER: case OP_TEST_HAS:
            if (!const_is_str(p, rd16(a))) { (void)verr(e, "%s: name constant at pc %u is not a string", nm, pc); goto done; }
            break;
        case OP_INVOKE: case OP_INVOKE_SPREAD: case OP_DEFER_INVOKE:
            if (!const_is_str(p, rd16(a))) { (void)verr(e, "%s: method name at pc %u is not a string", nm, pc); goto done; }
            break;
        case OP_RAISE_ERR:
            if (!const_is_str(p, rd16(a + 1))) { (void)verr(e, "%s: message at pc %u is not a string", nm, pc); goto done; }
            break;
        case OP_CONCAT:
            if (a[0] < 2) { (void)verr(e, "%s: CONCAT with fewer than 2 operands at pc %u", nm, pc); goto done; }
            break;
        case OP_CLOSURE: {
            if ((uint32_t)rd32s(a) >= (uint32_t)pg->nprotos) { (void)verr(e, "%s: prototype out of range at pc %u", nm, pc); goto done; }
            const Proto *child = pg->protos[rd32s(a)];
            for (int k = 0; k < child->nupvals; k++) {
                int idx = child->upvals[k].index;
                if (child->upvals[k].from_local ? idx >= p->nlocals : idx >= p->nupvals) {
                    (void)verr(e, "%s: closure capture out of range at pc %u", nm, pc);
                    goto done;
                }
            }
            break;
        }
        case OP_CLASS:
            if ((int)rd16(a) >= pg->nclasses) { (void)verr(e, "%s: class out of range at pc %u", nm, pc); goto done; }
            break;
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: case OP_JUMP_IF_FALSE_OR_POP:
        case OP_JUMP_IF_TRUE_OR_POP: case OP_TRY_BEGIN: break;
        default: break;
        }
        {
            int64_t tgt = INT64_MIN;
            if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_JUMP_IF_TRUE || op == OP_JUMP_IF_FALSE_OR_POP ||
                op == OP_JUMP_IF_TRUE_OR_POP || op == OP_TRY_BEGIN)
                tgt = (int64_t)end + rd32s(a);
            else if (op == OP_FOR_NEXT) tgt = (int64_t)end + rd32s(a + 3);
            else if (op == OP_RANGE_NEXT) tgt = (int64_t)end + rd32s(a + 2);
            if (tgt != INT64_MIN) {
                if (tgt < 0 || tgt >= (int64_t)n || !start[tgt]) { (void)verr(e, "%s: bad jump target at pc %u", nm, pc); goto done; }
            }
        }
    }
    /* pass 3: stack height and handler depth by abstract interpretation */
    {
        Flow fl = {height, tdepth, work, 0};
        for (uint32_t i = 0; i <= n; i++) height[i] = -1;
        if (flow_push(e, p, &fl, 0, 0, 0, "entry")) goto done;
        for (int k = 0; k <= p->ndefaults; k++)
            if (flow_push(e, p, &fl, p->default_pcs ? p->default_pcs[k] : 0u, 0, 0, "default entry")) goto done;
        while (fl.nwork > 0) {
            pc = fl.work[--fl.nwork];
            int op = p->code[pc];
            const uint8_t *a = p->code + pc + 1;
            uint32_t end = pc + (uint32_t)op_len(op);
            int h = height[pc], td = tdepth[pc];
            int pop = 0, push = 0;
            switch (op) {
            case OP_NOP: case OP_MAKE_CELL: case OP_DEFER_SCOPE: case OP_RUN_DEFERS: case OP_CTOR_CHECK:
            case OP_JUMP: case OP_TRY_BEGIN: case OP_TRY_END: case OP_RAISE_ERR: case OP_RETURN_NIL: break;
            case OP_CONST: case OP_NIL: case OP_TRUE: case OP_FALSE: case OP_SMALLINT: case OP_LOAD_LOCAL:
            case OP_LOAD_CELL: case OP_LOAD_UPVAL: case OP_LOAD_MODULE: case OP_LOAD_LINK: case OP_CLOSURE:
                push = 1; break;
            case OP_DUP: pop = 1; push = 2; break;
            case OP_DUP2: pop = 2; push = 4; break;
            case OP_POP: case OP_STORE_LOCAL: case OP_STORE_CELL: case OP_STORE_UPVAL: case OP_STORE_MODULE:
            case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: case OP_RETURN: case OP_THROW: case OP_PANIC:
            case OP_ASSERT_FAIL: case OP_MATCH_FAIL: case OP_FOR_PREP: case OP_YIELD:
                pop = 1; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_FLOOR_DIV: case OP_MOD: case OP_POW:
            case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_EQ: case OP_NE:
            case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_GET_INDEX:
                pop = 2; push = 1; break;
            case OP_NEG: case OP_BNOT: case OP_NOT: case OP_GET_MEMBER: case OP_TEST_ARRAY: case OP_TEST_HAS:
            case OP_NEW_INSTANCE: case OP_JUMP_IF_FALSE_OR_POP: case OP_JUMP_IF_TRUE_OR_POP:
                pop = 1; push = 1; break;
            case OP_CONCAT: pop = a[0]; push = 1; break;
            case OP_SET_INDEX: pop = 3; break;
            case OP_SET_MEMBER: case OP_SET_SLOT: pop = 2; break;
            case OP_NEW_ARRAY: pop = (int)rd16(a); push = 1; break;
            case OP_NEW_TABLE: pop = 2 * (int)rd16(a); push = 1; break;
            case OP_CALL: pop = a[0] + 1; push = 1; break;
            case OP_CALL_SPREAD: pop = a[0] + 2; push = 1; break;
            case OP_INVOKE: pop = a[2] + 1; push = 1; break;
            case OP_INVOKE_SPREAD: pop = a[2] + 2; push = 1; break;
            case OP_DEFER: pop = a[0] + 1; break;
            case OP_DEFER_INVOKE: pop = a[2] + 1; break;
            case OP_RANGE_PREP: pop = 3; break;
            case OP_FOR_NEXT: case OP_RANGE_NEXT: break; /* handled below */
            case OP_CLASS: pop = pg->classes[rd16(a)].nmethods + 1; push = 1; break;
            default: (void)verr(e, "%s: unhandled opcode 0x%02X", nm, (unsigned)op); goto done;
            }
            if (h < pop) { (void)verr(e, "%s: operand stack underflow at pc %u", nm, pc); goto done; }
            int nh = h - pop + push;
            switch (op) {
            case OP_JUMP:
                if (flow_push(e, p, &fl, (uint32_t)((int64_t)end + rd32s(a)), h, td, "jump")) goto done;
                break;
            case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
                if (flow_push(e, p, &fl, (uint32_t)((int64_t)end + rd32s(a)), nh, td, "branch")) goto done;
                if (flow_push(e, p, &fl, end, nh, td, "fallthrough")) goto done;
                break;
            case OP_JUMP_IF_FALSE_OR_POP: case OP_JUMP_IF_TRUE_OR_POP:
                if (flow_push(e, p, &fl, (uint32_t)((int64_t)end + rd32s(a)), h, td, "short-circuit")) goto done;
                if (flow_push(e, p, &fl, end, h - 1, td, "fallthrough")) goto done;
                break;
            case OP_TRY_BEGIN:
                if (flow_push(e, p, &fl, (uint32_t)((int64_t)end + rd32s(a)), h + 1, td, "handler")) goto done;
                if (flow_push(e, p, &fl, end, h, td + 1, "try body")) goto done;
                break;
            case OP_TRY_END:
                if (td == 0) { (void)verr(e, "%s: TRY_END without TRY_BEGIN at pc %u", nm, pc); goto done; }
                if (flow_push(e, p, &fl, end, h, td - 1, "try end")) goto done;
                break;
            case OP_FOR_NEXT:
                if (flow_push(e, p, &fl, (uint32_t)((int64_t)end + rd32s(a + 3)), h, td, "loop exit")) goto done;
                if (a[2] < 1 || a[2] > 2) { (void)verr(e, "%s: FOR_NEXT arity at pc %u", nm, pc); goto done; }
                if (flow_push(e, p, &fl, end, h + a[2], td, "loop body")) goto done;
                break;
            case OP_RANGE_NEXT:
                if (flow_push(e, p, &fl, (uint32_t)((int64_t)end + rd32s(a + 2)), h, td, "range exit")) goto done;
                if (flow_push(e, p, &fl, end, h + 1, td, "range body")) goto done;
                break;
            default:
                if (is_terminator(op)) break;
                if (end >= n) { (void)verr(e, "%s: falls off the end", nm); goto done; }
                if (flow_push(e, p, &fl, end, nh, td, "next")) goto done;
                break;
            }
        }
    }
    rc = 0;
done:
    free(start);
    free(height);
    free(tdepth);
    free(work);
    return rc;
}

int verify_program(const Program *pg, char *errbuf, size_t errsz) {
    VErr e = {errbuf, errsz};
    errbuf[0] = 0;
    if (pg->nmods < 1) return verr(&e, "no modules");
    for (int i = 0; i < pg->nmods; i++) {
        const ModInfo *m = &pg->mods[i];
        if (m->entry_proto < 0 || m->entry_proto >= pg->nprotos) return verr(&e, "module '%s': bad entry prototype", m->name);
        if (pg->protos[m->entry_proto]->nupvals != 0) return verr(&e, "module '%s': entry prototype has upvalues", m->name);
        for (int k = 0; k < m->nexports; k++) {
            if (m->exports[k].slot < 0 || m->exports[k].slot >= pg->nslots) return verr(&e, "module '%s': export slot out of range", m->name);
            for (int j = 0; j < k; j++)
                if (strcmp(m->exports[j].name, m->exports[k].name) == 0) return verr(&e, "module '%s': duplicate export", m->name);
        }
    }
    for (int i = 0; i < pg->nclasses; i++) {
        const ClassInfo *c = &pg->classes[i];
        if (c->ctor_proto < 0 || c->ctor_proto >= pg->nprotos) return verr(&e, "class '%s': bad constructor", c->name);
        if (pg->protos[c->ctor_proto]->nupvals != 0) return verr(&e, "class '%s': constructor has upvalues", c->name);
        for (int k = 0; k < c->nmethods; k++)
            for (int j = 0; j < k; j++)
                if (strcmp(c->method_names[j], c->method_names[k]) == 0) return verr(&e, "class '%s': duplicate method '%s'", c->name, c->method_names[k]);
        for (int k = 0; k < c->nfields; k++)
            for (int j = 0; j < k; j++)
                if (strcmp(c->field_names[j], c->field_names[k]) == 0) return verr(&e, "class '%s': duplicate field '%s'", c->name, c->field_names[k]);
    }
    for (int i = 0; i < pg->nlinks; i++) {
        const Link *l = &pg->links[i];
        if (l->kind == LINK_PRELUDE) {
            if (!l->member || lib_prelude_index(l->member, strlen(l->member)) < 0) return verr(&e, "link %d: unknown prelude function", i);
        } else if (l->kind == LINK_MOD_EXPORT || l->kind == LINK_MOD_VALUE) {
            const ModDesc *d = l->module ? lib_find_module(pg->hostvm, l->module, strlen(l->module)) : NULL;
            if (!d) return verr(&e, "link %d: unknown module", i);
            if (l->kind == LINK_MOD_EXPORT && (!l->member || !lib_module_has(d, l->member, strlen(l->member))))
                return verr(&e, "link %d: module '%s' has no such export", i, l->module);
        } else {
            return verr(&e, "link %d: bad kind", i);
        }
    }
    for (int i = 0; i < pg->nprotos; i++) {
        const Proto *p = pg->protos[i];
        if (verify_proto(pg, p, &e)) return 1;
        for (int k = 0; k < p->nlines; k++) {
            if (p->lines[k].pc >= p->code_len) return verr(&e, "debug pc out of range");
            if (k > 0 && p->lines[k].pc < p->lines[k - 1].pc) return verr(&e, "debug table not sorted");
        }
    }
    return 0;
}
