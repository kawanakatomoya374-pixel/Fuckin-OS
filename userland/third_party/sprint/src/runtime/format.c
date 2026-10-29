/* Value formatting (SPEC 6.5, 6.6) and the buffered stdout. */
#include "value_int.h"

/* ---------------------------------------------------------------- */
/* formatting (SPEC 6.5, 6.6)                                        */
/* ---------------------------------------------------------------- */
void out_put(Out *o, const char *s, size_t n) {
    if (o->sink) o->sink(o->ud, s, n);
    else if (o->dst && o->len + n <= o->cap) memcpy(o->dst + o->len, s, n);
    o->len += n;
}

void fmt_float(double d, char *buf) {
    if (isnan(d)) { strcpy(buf, "nan"); return; }
    if (isinf(d)) { strcpy(buf, d < 0 ? "-inf" : "inf"); return; }
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, 40, "%.*g", p, d);
        if (sp_feq(strtod(buf, NULL), d)) break;
    }
    /* "%g" switches to exponent form as soon as the shortest digit string is
     * shorter than the exponent (100.0 -> "1e+02"); SPEC 6.6 wants 100.0, so
     * integral values below 1e16 are written in full. */
    const char *ex = strchr(buf, 'e');
    if (ex) {
        long x = strtol(ex + 1, NULL, 10);
        if (x >= 0 && x < 16) snprintf(buf, 40, "%lld", (long long)d);
    }
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n')) strcat(buf, ".0");
}

static void put_repr_string(Out *o, SString *s) {
    out_put(o, "\"", 1);
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        char tmp[8];
        if (c == '"') out_put(o, "\\\"", 2);
        else if (c == '\\') out_put(o, "\\\\", 2);
        else if (c == '\n') out_put(o, "\\n", 2);
        else if (c == '\r') out_put(o, "\\r", 2);
        else if (c == '\t') out_put(o, "\\t", 2);
        else if (c < 0x20 || c == 0x7F) {
            snprintf(tmp, sizeof tmp, "\\x%02X", c);
            out_put(o, tmp, 4);
        } else out_put(o, (const char *)&c, 1);
    }
    out_put(o, "\"", 1);
}

/* Decimal formatting of an int64 into buf (>= 21 bytes, no terminator). Returns the length. */
size_t sp_fmt_int(char *buf, int64_t v) {
    static const char dig2[201] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    char tmp[20];
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    size_t pos = sizeof tmp;
    while (u >= 100) {
        size_t r = (size_t)(u % 100);
        u /= 100;
        pos -= 2;
        tmp[pos] = dig2[2 * r];
        tmp[pos + 1] = dig2[2 * r + 1];
    }
    if (u >= 10) {
        pos -= 2;
        tmp[pos] = dig2[2 * u];
        tmp[pos + 1] = dig2[2 * u + 1];
    } else {
        tmp[--pos] = (char)('0' + (int)u);
    }
    size_t k = 0, n = sizeof tmp - pos;
    if (v < 0) buf[k++] = '-';
    memcpy(buf + k, tmp + pos, n);
    return k + n;
}

void fmt_value(VM *vm, SpValue v, int depth, int repr, Out *o) {
    char buf[96];
    switch (v.type) {
    case T_NIL: out_put(o, "nil", 3); break;
    case T_BOOL: v.as.b ? out_put(o, "true", 4) : out_put(o, "false", 5); break;
    case T_INT: { size_t n = sp_fmt_int(buf, v.as.i); out_put(o, buf, n); break; }
    case T_FLOAT: fmt_float(v.as.f, buf); out_put(o, buf, strlen(buf)); break;
    case T_STRING:
        if (repr) put_repr_string(o, AS_STR(v));
        else out_put(o, AS_STR(v)->data, AS_STR(v)->len);
        break;
    case T_ARRAY: {
        SArray *a = AS_ARRAY(v);
        if (depth >= 8) { out_put(o, "[...]", 5); break; }
        out_put(o, "[", 1);
        for (int64_t i = 0; i < a->len; i++) {
            if (i) out_put(o, ", ", 2);
            fmt_value(vm, a->items[i], depth + 1, 1, o);
        }
        out_put(o, "]", 1);
        break;
    }
    case T_TABLE: {
        STable *t = AS_TABLE(v);
        if (depth >= 8) { out_put(o, "{...}", 5); break; }
        out_put(o, "{", 1);
        int64_t c = 0;
        SpValue k, val;
        int first = 1;
        while ((c = table_next(t, c, &k, &val)) >= 0) {
            if (!first) out_put(o, ", ", 2);
            first = 0;
            fmt_value(vm, k, depth + 1, 1, o);
            out_put(o, ": ", 2);
            fmt_value(vm, val, depth + 1, 1, o);
        }
        out_put(o, "}", 1);
        break;
    }
    case T_FUNCTION: {
        SFunc *f = AS_FUNC(v);
        const char *nm = f->nat ? f->nat->name : (f->proto && f->proto->name ? f->proto->name : NULL);
        int n = nm ? snprintf(buf, sizeof buf, "<function %s #%llu>", nm, (unsigned long long)f->h.id)
                   : snprintf(buf, sizeof buf, "<function #%llu>", (unsigned long long)f->h.id);
        out_put(o, buf, (size_t)n);
        break;
    }
    case T_ITERATOR: {
        int n = snprintf(buf, sizeof buf, "<iterator %s #%llu>", AS_ITER(v)->cls->kind, (unsigned long long)AS_ITER(v)->h.id);
        out_put(o, buf, (size_t)n);
        break;
    }
    case T_NATIVE: {
        int n = snprintf(buf, sizeof buf, "<native %s #%llu>", AS_NATIVE(v)->cls->name, (unsigned long long)AS_NATIVE(v)->h.id);
        out_put(o, buf, (size_t)n);
        break;
    }
    case T_COROUTINE: {
        int n = snprintf(buf, sizeof buf, "<coroutine #%llu>", (unsigned long long)AS_CO(v)->h.id);
        out_put(o, buf, (size_t)n);
        break;
    }
    case T_CLASS: {
        out_put(o, "<class ", 7);
        out_put(o, AS_CLASS(v)->name->data, AS_CLASS(v)->name->len);
        out_put(o, ">", 1);
        break;
    }
    case T_INSTANCE: {
        SInstance *in = AS_INSTANCE(v);
        int n = snprintf(buf, sizeof buf, " #%llu>", (unsigned long long)in->h.id);
        out_put(o, "<", 1);
        out_put(o, in->cls->name->data, in->cls->name->len);
        out_put(o, buf, (size_t)n);
        break;
    }
    case T_MODULE: {
        SModule *m = AS_MODULE(v);
        out_put(o, "<module ", 8);
        out_put(o, m->name->data, m->name->len);
        out_put(o, ">", 1);
        break;
    }
    case T_ERROR: {
        SError *e = AS_ERROR(v);
        out_put(o, "<", 1);
        out_put(o, e->kind->data, e->kind->len);
        out_put(o, ": ", 2);
        out_put(o, e->message->data, e->message->len);
        out_put(o, ">", 1);
        break;
    }
    default: {
        int n = snprintf(buf, sizeof buf, "<%s #%llu>", type_name(v),
                         (unsigned long long)(v.as.o ? v.as.o->id : 0));
        out_put(o, buf, (size_t)n);
    }
    }
}

SString *vm_to_string(VM *vm, SpValue v) {
    if (v.type == T_STRING) return AS_STR(v);
    Out m = {0};
    fmt_value(vm, v, 0, 0, &m);
    SString *s = vm_alloc_string(vm, m.len);
    Out w = {0};
    w.dst = s->data;
    w.cap = m.len;
    fmt_value(vm, v, 0, 0, &w);
    string_finish(s);
    return s;
}

/* ---------------------------------------------------------------- */
/* stdout buffer                                                     */
/* ---------------------------------------------------------------- */
void vm_out_flush(VM *vm) {
    if (vm->outlen) {
        if (vm->hwrite) (void)vm->hwrite(vm->hio_ud, 1, vm->outbuf, vm->outlen);
        else (void)fwrite(vm->outbuf, 1, vm->outlen, stdout);
        vm->outlen = 0;
    }
    if (!vm->hwrite) (void)fflush(stdout);
}

void vm_out_write(VM *vm, const char *s, size_t n) {
    while (n) {
        size_t room = vm->outcap - vm->outlen;
        if (room == 0) { vm_out_flush(vm); room = vm->outcap; }
        size_t k = n < room ? n : room;
        memcpy(vm->outbuf + vm->outlen, s, k);
        vm->outlen += k;
        s += k;
        n -= k;
    }
}
