/* Compiler: import / from-import and the export table of user modules. */
#include "compiler_int.h"

static const ExportEnt *user_export(const Comp *c, int mod, const char *name, size_t len) {
    const ModInfo *m = &c->real->mods[mod];
    for (int i = 0; i < m->nexports; i++)
        if (len > 0 && m->exports[i].name[0] == name[0] && strlen(m->exports[i].name) == len && memcmp(m->exports[i].name, name, len) == 0) return &m->exports[i];
    return NULL;
}

/* `m.name` where m is a user-module import: the module slot of the export, or -1 */
int member_user_slot(Comp *c, const Node *obj, const Node *m) {
    if (obj->kind != N_NAME) return -1;
    Res r = resolve_name(c, obj);
    if (r.kind != R_MODULE || r.user_mod < 0) return -1;
    const ExportEnt *e = user_export(c, r.user_mod, m->str, m->slen);
    if (!e) cerr(c, m, "NameError", E_NAME, "module '%s' has no export '%s'", c->real->mods[r.user_mod].name, m->str);
    return e->slot;
}

void export_add(Comp *c, const Node *where, const char *name, size_t len, int slot) {
    for (int i = 0; i < c->nexp; i++)
        if (len > 0 && c->exports[i].name[0] == name[0] && strlen(c->exports[i].name) == len && memcmp(c->exports[i].name, name, len) == 0)
            cerr(c, where, "NameError", E_NAME, "'%.*s' is exported twice", (int)len, name);
    if (c->nexp == c->capexp) {
        c->capexp = c->capexp ? c->capexp * 2 : 8;
        c->exports = xrealloc(c->exports, sizeof(ExportEnt) * (size_t)c->capexp);
    }
    char *nm = malloc(len + 1);
    if (!nm) exit(14);
    memcpy(nm, name, len);
    nm[len] = 0;
    c->exports[c->nexp].name = nm;
    c->exports[c->nexp].slot = slot;
    c->nexp++;
}

static int find_user_module(const Comp *c, const char *name, size_t len) {
    for (int i = 0; i < c->real->nmods; i++)
        if (len > 0 && c->real->mods[i].name[0] == name[0] && strlen(c->real->mods[i].name) == len && memcmp(c->real->mods[i].name, name, len) == 0) return i;
    return -1;
}

void compile_import(Comp *c, Node *n) {
    const ModDesc *md = lib_find_module(c->vm, n->str, n->slen);
    if (!md) {
        int um = find_user_module(c, n->str, n->slen);
        if (um < 0) cerr(c, n, "ImportError", E_IMPORT, "module '%s' not found", n->str);
        int mv = declare_mvar(c, n, n->decl->name, n->decl->len, 1, NULL);
        c->mv[mv].user_mod = um;
        return;
    }
    int idx = add_link(c, LINK_MOD_VALUE, n->str, NULL);
    setpos(c, n);
    op_u16(c, OP_LOAD_LINK, (unsigned)idx, 1);
    int mv = declare_mvar(c, n, n->decl->name, n->decl->len, 1, n->str);
    op_u16(c, OP_STORE_MODULE, (unsigned)mv, -1);
}
void compile_from(Comp *c, Node *n) {
    const ModDesc *md = lib_find_module(c->vm, n->str, n->slen);
    if (!md) {
        int um = find_user_module(c, n->str, n->slen);
        if (um < 0) cerr(c, n, "ImportError", E_IMPORT, "module '%s' not found", n->str);
        for (int i = 0; i < n->n; i++) {
            Node *it = n->list[i];
            const ExportEnt *e = user_export(c, um, it->str, it->slen);
            if (!e) cerr(c, it, "ImportError", E_IMPORT, "module '%s' has no export '%s'", n->str, it->str);
            int mv = declare_mvar(c, it, it->decl->name, it->decl->len, 1, NULL);
            c->mv[mv].alias_slot = e->slot;
        }
        return;
    }
    for (int i = 0; i < n->n; i++) {
        Node *it = n->list[i];
        if (!lib_module_has(md, it->str, it->slen))
            cerr(c, it, "ImportError", E_IMPORT, "module '%s' has no export '%s'", n->str, it->str);
        int idx = add_link(c, LINK_MOD_EXPORT, n->str, it->str);
        setpos(c, it);
        op_u16(c, OP_LOAD_LINK, (unsigned)idx, 1);
        int mv = declare_mvar(c, it, it->decl->name, it->decl->len, 1, NULL);
        op_u16(c, OP_STORE_MODULE, (unsigned)mv, -1);
    }
}
