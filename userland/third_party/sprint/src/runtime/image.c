/* Program image (.spc): byte buffers, encoder and decoder (SPEC 16). */
#include "image_int.h"

/* ================================================================ */
/* byte buffers                                                      */
/* ================================================================ */
typedef struct Buf {
    uint8_t *d;
    size_t len, cap;
} Buf;

void *xalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) exit(14);
    return p;
}

static void b_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n) nc *= 2;
        uint8_t *nd = realloc(b->d, nc);
        if (!nd) exit(14);
        b->d = nd;
        b->cap = nc;
    }
    if (n) memcpy(b->d + b->len, p, n);
    b->len += n;
}
static void b_u8(Buf *b, unsigned v) { uint8_t x = (uint8_t)v; b_put(b, &x, 1); }
static void b_u16(Buf *b, unsigned v) { b_u8(b, v & 0xFFu); b_u8(b, (v >> 8) & 0xFFu); }
static void b_u32(Buf *b, uint32_t v) { b_u16(b, v & 0xFFFFu); b_u16(b, v >> 16); }
static void b_u64(Buf *b, uint64_t v) { b_u32(b, (uint32_t)v); b_u32(b, (uint32_t)(v >> 32)); }

/* string table: first-appearance order, no duplicates */
typedef struct StrTab {
    char **s;
    size_t *len;
    int n, cap;
} StrTab;

static uint32_t st_intern(StrTab *t, const char *s, size_t len) {
    for (int i = 0; i < t->n; i++)
        if (t->len[i] == len && memcmp(t->s[i], s, len) == 0) return (uint32_t)i;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->s = realloc(t->s, sizeof(char *) * (size_t)t->cap);
        t->len = realloc(t->len, sizeof(size_t) * (size_t)t->cap);
        if (!t->s || !t->len) exit(14);
    }
    char *c = xalloc(len + 1);
    memcpy(c, s, len);
    c[len] = 0;
    t->s[t->n] = c;
    t->len[t->n] = len;
    return (uint32_t)t->n++;
}
static uint32_t st_cstr(StrTab *t, const char *s) { return st_intern(t, s, strlen(s)); }

#define NOSTR 0xFFFFFFFFu
#define IMG_MAJOR 2u
#define IMG_MINOR 0u

static void section(Buf *out, unsigned tag, const Buf *body) {
    b_u8(out, tag);
    b_u32(out, (uint32_t)body->len);
    b_put(out, body->d, body->len);
}

uint8_t *image_encode(const Program *pg, size_t *out_len) {
    StrTab st;
    memset(&st, 0, sizeof st);
    Buf mods = {0}, protos = {0}, classes = {0}, links = {0}, debug = {0};

    /* MODULES */
    b_u32(&mods, (uint32_t)pg->nslots);
    for (int i = 0; i < pg->nslots; i++) b_u32(&mods, st_cstr(&st, pg->slot_names[i]));
    b_u16(&mods, (unsigned)pg->nmods);
    for (int i = 0; i < pg->nmods; i++) {
        const ModInfo *m = &pg->mods[i];
        b_u32(&mods, st_cstr(&st, m->name));
        b_u32(&mods, st_cstr(&st, m->rel));
        uint8_t dg[32];
        mod_sha(m, dg);
        b_put(&mods, dg, 32);
        b_u32(&mods, (uint32_t)m->entry_proto);
        b_u16(&mods, (unsigned)m->nexports);
        for (int k = 0; k < m->nexports; k++) {
            b_u32(&mods, st_cstr(&st, m->exports[k].name));
            b_u32(&mods, (uint32_t)m->exports[k].slot);
        }
    }
    /* PROTOS and DEBUG */
    b_u32(&protos, (uint32_t)pg->nprotos);
    for (int i = 0; i < pg->nprotos; i++) {
        const Proto *p = pg->protos[i];
        b_u32(&protos, p->name ? st_cstr(&st, p->name) : NOSTR);
        b_u16(&protos, (unsigned)p->mod);
        b_u8(&protos, (unsigned)(p->has_rest ? 1 : 0) | (unsigned)(p->is_module ? 2 : 0));
        b_u8(&protos, (unsigned)p->nparams);
        b_u8(&protos, (unsigned)p->nrequired);
        b_u16(&protos, (unsigned)p->nlocals);
        b_u16(&protos, (unsigned)p->max_stack);
        b_u16(&protos, (unsigned)p->nupvals);
        for (int k = 0; k < p->nupvals; k++) {
            b_u8(&protos, p->upvals[k].from_local);
            b_u16(&protos, p->upvals[k].index);
        }
        b_u16(&protos, (unsigned)p->nconsts);
        for (int k = 0; k < p->nconsts; k++) {
            SpValue v = p->consts[k];
            switch (v.type) {
            case T_BOOL: b_u8(&protos, v.as.b ? 2u : 1u); break;
            case T_INT: b_u8(&protos, 3); b_u64(&protos, (uint64_t)v.as.i); break;
            case T_FLOAT: {
                uint64_t bits;
                memcpy(&bits, &v.as.f, sizeof bits);
                b_u8(&protos, 4);
                b_u64(&protos, bits);
                break;
            }
            case T_STRING: b_u8(&protos, 5); b_u32(&protos, st_intern(&st, AS_STR(v)->data, AS_STR(v)->len)); break;
            default: b_u8(&protos, 0); break;
            }
        }
        b_u8(&protos, (unsigned)p->ndefaults);
        for (int k = 0; k <= p->ndefaults; k++) b_u32(&protos, p->default_pcs ? p->default_pcs[k] : 0u);
        b_u32(&protos, p->code_len);
        b_put(&protos, p->code, p->code_len);
        b_u32(&debug, (uint32_t)p->nlines);
        for (int k = 0; k < p->nlines; k++) {
            b_u32(&debug, p->lines[k].pc);
            b_u32(&debug, (uint32_t)p->lines[k].line);
            b_u32(&debug, (uint32_t)p->lines[k].col);
        }
    }
    /* CLASSES */
    b_u32(&classes, (uint32_t)pg->nclasses);
    for (int i = 0; i < pg->nclasses; i++) {
        const ClassInfo *c = &pg->classes[i];
        b_u32(&classes, st_cstr(&st, c->name));
        b_u16(&classes, (unsigned)c->nmethods);
        for (int k = 0; k < c->nmethods; k++) b_u32(&classes, st_cstr(&st, c->method_names[k]));
        b_u16(&classes, (unsigned)c->nfields);
        for (int k = 0; k < c->nfields; k++) b_u32(&classes, st_cstr(&st, c->field_names[k]));
        b_u32(&classes, (uint32_t)c->ctor_proto);
    }
    /* LINKS */
    b_u32(&links, (uint32_t)pg->nlinks);
    for (int i = 0; i < pg->nlinks; i++) {
        b_u8(&links, (unsigned)pg->links[i].kind);
        b_u32(&links, pg->links[i].module ? st_cstr(&st, pg->links[i].module) : NOSTR);
        b_u32(&links, pg->links[i].member ? st_cstr(&st, pg->links[i].member) : NOSTR);
    }
    /* STRINGS */
    Buf strs = {0};
    b_u32(&strs, (uint32_t)st.n);
    for (int i = 0; i < st.n; i++) {
        b_u32(&strs, (uint32_t)st.len[i]);
        b_put(&strs, st.s[i], st.len[i]);
    }
    Buf payload = {0};
    section(&payload, 1, &strs);
    section(&payload, 2, &mods);
    section(&payload, 3, &protos);
    section(&payload, 4, &classes);
    section(&payload, 5, &links);
    section(&payload, 6, &debug);

    Buf img = {0};
    b_put(&img, "SPRC", 4);
    b_u16(&img, IMG_MAJOR);
    b_u16(&img, IMG_MINOR);
    b_u32(&img, 1); /* flags: debug info present */
    b_u32(&img, 0); /* required features */
    b_u32(&img, 64);
    b_u32(&img, 0);
    b_u64(&img, payload.len);
    uint8_t h[32];
    sp_source_hash(pg, h);
    b_put(&img, h, 32);
    b_put(&img, payload.d, payload.len);
    sp_sha256(payload.d, payload.len, h);
    b_put(&img, h, 32);

    for (int i = 0; i < st.n; i++) free(st.s[i]);
    free(st.s);
    free(st.len);
    free(mods.d); free(protos.d); free(classes.d); free(links.d); free(debug.d); free(strs.d); free(payload.d);
    *out_len = img.len;
    return img.d;
}


/* ================================================================ */
/* decoder                                                           */
/* ================================================================ */
typedef struct Rd {
    const uint8_t *p;
    size_t n, pos;
    int bad;
} Rd;

static uint32_t r_u8(Rd *r) {
    if (r->pos + 1 > r->n) { r->bad = 1; return 0; }
    return r->p[r->pos++];
}
static uint32_t r_u16(Rd *r) { uint32_t a = r_u8(r); return a | (r_u8(r) << 8); }
static uint32_t r_u32(Rd *r) { uint32_t a = r_u16(r); return a | (r_u16(r) << 16); }
static uint64_t r_u64(Rd *r) { uint64_t a = r_u32(r); return a | ((uint64_t)r_u32(r) << 32); }
static const uint8_t *r_bytes(Rd *r, size_t n) {
    if (n > r->n - r->pos) { r->bad = 1; return NULL; }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}

static char *dupn(const char *s, size_t n) {
    char *c = xalloc(n + 1);
    memcpy(c, s, n);
    c[n] = 0;
    return c;
}

typedef struct Dec {
    Program *pg;
    VM *vm;
    char **strs;
    size_t *slen;
    uint32_t nstr;
    uint32_t nfree; /* entries of strs that are allocated */
    char *err;
    size_t errsz;
} Dec;

static int derr(Dec *d, const char *fmt, ...) SP_PRINTF(2, 3);

static int derr(Dec *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(d->err, d->errsz, fmt, ap);
    va_end(ap);
    return 1;
}

/* string by index, or NULL with an error recorded */
static const char *dstr(Dec *d, uint32_t i) {
    if (i >= d->nstr) {
        (void)derr(d, "string index %u out of range", i);
        return NULL;
    }
    return d->strs[i];
}

static int dec_strings(Dec *d, Rd *s) {
    uint32_t n = r_u32(s);
    if (n > s->n) return derr(d, "bad string count");
    d->strs = calloc(n ? n : 1u, sizeof(char *));
    d->slen = calloc(n ? n : 1u, sizeof(size_t));
    if (!d->strs || !d->slen) exit(14);
    d->nstr = n;
    for (uint32_t i = 0; i < d->nstr; i++) {
        uint32_t l = r_u32(s);
        const uint8_t *b = r_bytes(s, l);
        if (s->bad) return derr(d, "truncated string table");
        size_t bad;
        if (!utf8_validate((const char *)b, l, &bad)) return derr(d, "string %u is not valid UTF-8", i);
        d->strs[i] = dupn((const char *)b, l);
        d->slen[i] = l;
        d->nfree = i + 1;
    }
    return s->pos != s->n ? derr(d, "string section size mismatch") : 0;
}

static int dec_modules(Dec *d, Rd *s) {
    Program *pg = d->pg;
    uint32_t nslots = r_u32(s);
    if (nslots > 65535u) return derr(d, "too many module slots");
    pg->slot_names = calloc((size_t)(nslots ? nslots : 1u), sizeof(char *));
    if (!pg->slot_names) exit(14);
    for (int i = 0; i < (int)nslots; i++) {
        pg->nslots = i + 1;
        const char *nm = dstr(d, r_u32(s));
        if (s->bad) return derr(d, "truncated module section");
        if (!nm) return 1;
        pg->slot_names[i] = dupn(nm, strlen(nm));
    }
    pg->nslots = (int)nslots;
    int nm = (int)r_u16(s);
    pg->mods = calloc((size_t)(nm ? nm : 1), sizeof(ModInfo));
    if (!pg->mods) exit(14);
    pg->capmods = nm ? nm : 1;
    for (int i = 0; i < nm; i++) {
        ModInfo *m = &pg->mods[i];
        pg->nmods = i + 1;
        const char *name = dstr(d, r_u32(s));
        const char *path = dstr(d, r_u32(s));
        if (s->bad) return derr(d, "truncated module section");
        if (!name || !path) return 1;
        m->name = dupn(name, strlen(name));
        (void)snprintf(m->rel, sizeof m->rel, "%s", path);
        (void)snprintf(m->file, sizeof m->file, "%s", path);
        const uint8_t *sh = r_bytes(s, 32);
        if (!sh) return derr(d, "truncated module section");
        memcpy(m->sha, sh, 32);
        m->entry_proto = (int)r_u32(s);
        m->nexports = (int)r_u16(s);
        m->exports = calloc((size_t)(m->nexports ? m->nexports : 1), sizeof(ExportEnt));
        if (!m->exports) exit(14);
        for (int k = 0; k < m->nexports; k++) {
            const char *en = dstr(d, r_u32(s));
            uint32_t slot = r_u32(s);
            if (s->bad) return derr(d, "truncated module section");
            if (!en) return 1;
            m->exports[k].name = dupn(en, strlen(en));
            m->exports[k].slot = (int)slot;
        }
    }
    return s->bad || s->pos != s->n ? derr(d, "module section size mismatch") : 0;
}

static int dec_protos(Dec *d, Rd *s) {
    Program *pg = d->pg;
    uint32_t np = r_u32(s);
    if (np > s->n) return derr(d, "bad prototype count");
    pg->protos = calloc(np ? np : 1u, sizeof(Proto *));
    if (!pg->protos) exit(14);
    pg->capprotos = (int)(np ? np : 1u);
    for (uint32_t i = 0; i < np; i++) {
        Proto *p = calloc(1, sizeof *p);
        if (!p) exit(14);
        pg->protos[i] = p;
        pg->nprotos = (int)i + 1;
        uint32_t ni = r_u32(s);
        if (ni != NOSTR) {
            const char *nm = dstr(d, ni);
            if (!nm) return 1;
            p->name = dupn(nm, strlen(nm));
        }
        p->mod = (int)r_u16(s);
        unsigned fl = r_u8(s);
        if (fl & ~3u) return derr(d, "bad prototype flags");
        p->has_rest = (int)(fl & 1u);
        p->is_module = (int)((fl >> 1) & 1u);
        p->nparams = (int)r_u8(s);
        p->nrequired = (int)r_u8(s);
        p->nlocals = (int)r_u16(s);
        p->max_stack = (int)r_u16(s);
        p->nupvals = (int)r_u16(s);
        p->upvals = calloc((size_t)(p->nupvals ? p->nupvals : 1), sizeof(UpDesc));
        if (!p->upvals) exit(14);
        for (int k = 0; k < p->nupvals; k++) {
            p->upvals[k].from_local = (uint8_t)r_u8(s);
            p->upvals[k].index = (uint16_t)r_u16(s);
        }
        p->nconsts = (int)r_u16(s);
        p->capconsts = p->nconsts;
        p->consts = calloc((size_t)(p->nconsts ? p->nconsts : 1), sizeof(SpValue));
        if (!p->consts) exit(14);
        for (int k = 0; k < p->nconsts; k++) {
            unsigned t = r_u8(s);
            switch (t) {
            case 0: p->consts[k] = v_nil(); break;
            case 1: p->consts[k] = v_bool(0); break;
            case 2: p->consts[k] = v_bool(1); break;
            case 3: p->consts[k] = v_int((int64_t)r_u64(s)); break;
            case 4: {
                uint64_t bits = r_u64(s);
                double dv;
                memcpy(&dv, &bits, sizeof dv);
                p->consts[k] = v_float(dv);
                break;
            }
            case 5: {
                uint32_t si = r_u32(s);
                if (s->bad) return derr(d, "truncated prototype");
                if (!dstr(d, si)) return 1;
                p->consts[k] = vm_perm_string(d->vm, d->strs[si], d->slen[si]);
                break;
            }
            default: return derr(d, "bad constant tag %u", t);
            }
        }
        p->ndefaults = (int)r_u8(s);
        p->default_pcs = malloc(sizeof(uint32_t) * (size_t)(p->ndefaults + 1));
        if (!p->default_pcs) exit(14);
        for (int k = 0; k <= p->ndefaults; k++) p->default_pcs[k] = r_u32(s);
        p->code_len = r_u32(s);
        const uint8_t *code = r_bytes(s, p->code_len);
        if (!code || s->bad) return derr(d, "truncated prototype");
        p->code_cap = p->code_len;
        p->code = xalloc(p->code_len);
        memcpy(p->code, code, p->code_len);
    }
    return s->bad || s->pos != s->n ? derr(d, "prototype section size mismatch") : 0;
}

static int dec_classes(Dec *d, Rd *s) {
    Program *pg = d->pg;
    uint32_t nc = r_u32(s);
    if (nc > s->n) return derr(d, "bad class count");
    pg->classes = calloc(nc ? nc : 1u, sizeof(ClassInfo));
    if (!pg->classes) exit(14);
    pg->capclasses = (int)(nc ? nc : 1u);
    for (uint32_t i = 0; i < nc; i++) {
        ClassInfo *c = &pg->classes[i];
        pg->nclasses = (int)i + 1;
        const char *nm = dstr(d, r_u32(s));
        if (s->bad) return derr(d, "truncated class section");
        if (!nm) return 1;
        c->name = dupn(nm, strlen(nm));
        c->nmethods = (int)r_u16(s);
        c->method_names = calloc((size_t)(c->nmethods ? c->nmethods : 1), sizeof(char *));
        if (!c->method_names) exit(14);
        for (int k = 0; k < c->nmethods; k++) {
            const char *mn = dstr(d, r_u32(s));
            if (s->bad) return derr(d, "truncated class section");
            if (!mn) return 1;
            c->method_names[k] = dupn(mn, strlen(mn));
        }
        c->nfields = (int)r_u16(s);
        c->field_names = calloc((size_t)(c->nfields ? c->nfields : 1), sizeof(char *));
        if (!c->field_names) exit(14);
        for (int k = 0; k < c->nfields; k++) {
            const char *fnm = dstr(d, r_u32(s));
            if (s->bad) return derr(d, "truncated class section");
            if (!fnm) return 1;
            c->field_names[k] = dupn(fnm, strlen(fnm));
        }
        c->ctor_proto = (int)r_u32(s);
    }
    return s->bad || s->pos != s->n ? derr(d, "class section size mismatch") : 0;
}

static int dec_links(Dec *d, Rd *s) {
    Program *pg = d->pg;
    uint32_t nl = r_u32(s);
    if (nl > s->n) return derr(d, "bad link count");
    pg->links = calloc(nl ? nl : 1u, sizeof(Link));
    if (!pg->links) exit(14);
    pg->caplinks = (int)(nl ? nl : 1u);
    for (uint32_t i = 0; i < nl; i++) {
        Link *l = &pg->links[i];
        pg->nlinks = (int)i + 1;
        l->kind = (int)r_u8(s);
        uint32_t mi = r_u32(s), bi = r_u32(s);
        if (s->bad) return derr(d, "truncated link section");
        if (mi != NOSTR) {
            const char *x = dstr(d, mi);
            if (!x) return 1;
            l->module = dupn(x, strlen(x));
        }
        if (bi != NOSTR) {
            const char *x = dstr(d, bi);
            if (!x) return 1;
            l->member = dupn(x, strlen(x));
        }
    }
    return s->bad || s->pos != s->n ? derr(d, "link section size mismatch") : 0;
}

static int dec_debug(Dec *d, Rd *s) {
    Program *pg = d->pg;
    for (int i = 0; i < pg->nprotos; i++) {
        Proto *p = pg->protos[i];
        uint32_t cnt = r_u32(s);
        if (s->bad || cnt > s->n / 12u + 1u) return derr(d, "bad debug table");
        p->nlines = (int)cnt;
        p->caplines = (int)cnt;
        p->lines = calloc(cnt ? cnt : 1u, sizeof(LineEnt));
        if (!p->lines) exit(14);
        for (uint32_t k = 0; k < cnt; k++) {
            p->lines[k].pc = r_u32(s);
            p->lines[k].line = (int)r_u32(s);
            p->lines[k].col = (int)r_u32(s);
        }
    }
    return s->bad || s->pos != s->n ? derr(d, "debug section size mismatch") : 0;
}

static int dec_container(const uint8_t *buf, size_t len, Rd sec[6], int *nsec, uint8_t want_src[32], char *err, size_t errsz) {
    Dec tmp = {NULL, NULL, NULL, NULL, 0, 0, err, errsz};
    if (len < 96) return derr(&tmp, "image too short");
    if (memcmp(buf, "SPRC", 4) != 0) return derr(&tmp, "bad magic");
    Rd h = {buf, 64, 4, 0};
    uint32_t major = r_u16(&h), minor = r_u16(&h), flags = r_u32(&h), feats = r_u32(&h), hsize = r_u32(&h), resv = r_u32(&h);
    uint64_t psize = r_u64(&h);
    if (major != IMG_MAJOR) return derr(&tmp, "unsupported format major %u", major);
    if (minor > IMG_MINOR) return derr(&tmp, "unsupported format minor %u", minor);
    if (hsize != 64 || resv != 0 || (flags & ~1u) != 0) return derr(&tmp, "bad header");
    if (feats != 0) return derr(&tmp, "unsupported required features 0x%x", feats);
    if (psize > (uint64_t)len || (uint64_t)len != 64u + psize + 32u) return derr(&tmp, "file length does not match payload size");
    uint8_t digest[32];
    sp_sha256(buf + 64, (size_t)psize, digest);
    if (memcmp(digest, buf + 64 + psize, 32) != 0) return derr(&tmp, "checksum mismatch");
    memcpy(want_src, buf + 32, 32);
    Rd r = {buf + 64, (size_t)psize, 0, 0};
    *nsec = (flags & 1u) ? 6 : 5;
    for (int i = 0; i < *nsec; i++) {
        unsigned tag = r_u8(&r);
        uint32_t sz = r_u32(&r);
        const uint8_t *body = r_bytes(&r, sz);
        if (r.bad || tag != (unsigned)i + 1u) return derr(&tmp, "bad section structure");
        sec[i].p = body;
        sec[i].n = sz;
        sec[i].pos = 0;
        sec[i].bad = 0;
    }
    return r.pos != r.n ? derr(&tmp, "trailing bytes after sections") : 0;
}

Program *image_decode(VM *vm, const uint8_t *buf, size_t len, char *err, size_t errsz) {
    Dec d = {program_new(), vm, NULL, NULL, 0, 0, err, errsz};
    Rd sec[6];
    d.pg->hostvm = vm;
    int nsec = 0;
    uint8_t want_src[32], got[32];
    err[0] = 0;
    int bad = dec_container(buf, len, sec, &nsec, want_src, err, errsz) || dec_strings(&d, &sec[0]) ||
              dec_modules(&d, &sec[1]) || dec_protos(&d, &sec[2]) || dec_classes(&d, &sec[3]) ||
              dec_links(&d, &sec[4]) || (nsec == 6 && dec_debug(&d, &sec[5]));
    if (!bad) {
        sp_source_hash(d.pg, got);
        if (memcmp(got, want_src, 32) != 0) bad = derr(&d, "source hash mismatch");
    }
    if (!bad) bad = verify_program(d.pg, err, errsz);
    for (uint32_t i = 0; i < d.nfree; i++) free(d.strs[i]);
    free(d.strs);
    free(d.slen);
    if (bad) {
        if (!err[0]) (void)snprintf(err, errsz, "malformed image");
        program_free(d.pg);
        return NULL;
    }
    return d.pg;
}
