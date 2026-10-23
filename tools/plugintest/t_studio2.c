#include "mock.h"
#include "cos_plugin_studio.h"
static char doc[20000]; static size_t dlen;
static int has_doc(void) { return 1; }
static const char *doc_path(void) { return "/tmp/x.c"; }
static size_t doc_length(void) { return dlen; }
static size_t doc_read(size_t pos, char *out, size_t n) { if (pos >= dlen) return 0; if (n > dlen - pos) n = dlen - pos; memcpy(out, doc + pos, n); return n; }
static void doc_replace(size_t pos, size_t del, const char *ins, size_t n) { memmove(doc + pos + n, doc + pos + del, dlen - pos - del); memcpy(doc + pos, ins, n); dlen = dlen - del + n; doc[dlen] = 0; }
static void status(const char *m) { snprintf(g_status, sizeof g_status, "%s", m); }
static void output(const char *l) { strcat(g_out, l); strcat(g_out, "\n"); }
static void (*cmds[32])(void); static const char *labels[32]; static int ncmd;
static int add_command(const char *l, void (*fn)(void)) { labels[ncmd] = l; cmds[ncmd++] = fn; return 0; }
static studio_api_t api;
#include "../../userland/plugins/studio/cformat.c"
void run_cformat_test(void) {
    memset(&api, 0, sizeof api); api.base.abi = COS_PBASE_ABI; api.base.status = status; api.base.snprintf = m_snprintf; api.abi = STUDIOP_ABI; api.add_command = add_command;
    api.has_doc = has_doc; api.doc_path = doc_path; api.doc_length = doc_length; api.doc_read = doc_read; api.doc_replace = doc_replace; api.output = output;
    cos_plugin_host_t h = { 1, sizeof h, "studio", &api }; ncmd = 0; CHECK(init(&h) == 0, "cformat init");
    const char *src = "int main(void) {\nint x = 1;\nif (x) {\nprintf(\"a{b\");\n}\n  return call(x,\n  y);\n}\n";
    dlen = strlen(src); memcpy(doc, src, dlen + 1); cmds[0]();
    const char *want = "int main(void) {\n    int x = 1;\n    if (x) {\n        printf(\"a{b\");\n    }\n    return call(x,\n        y);\n}\n";
    CHECK(!strcmp(doc, want), "cformat:\n%s", doc);
    cmds[0](); CHECK(!strcmp(g_status, "Already formatted"), "idempotent: %s", g_status);
}
