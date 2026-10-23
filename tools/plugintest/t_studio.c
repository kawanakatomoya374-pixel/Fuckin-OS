/* studio plugins against a fake document */
#include "mock.h"
#include "cos_plugin_studio.h"
static char doc[20000]; static size_t dlen, sel_a, sel_b, caretp;
static int has_doc(void) { return 1; }
static const char *doc_path(void) { return "/tmp/x.c"; }
static size_t doc_length(void) { return dlen; }
static size_t doc_read(size_t pos, char *out, size_t n) { if (pos >= dlen) return 0; if (n > dlen - pos) n = dlen - pos; memcpy(out, doc + pos, n); return n; }
static void doc_replace(size_t pos, size_t del, const char *ins, size_t n) { memmove(doc + pos + n, doc + pos + del, dlen - pos - del); memcpy(doc + pos, ins, n); dlen = dlen - del + n; doc[dlen] = 0; }
static size_t caret(void) { return caretp; }
static size_t sel_start(void) { return sel_a; }
static size_t sel_end(void) { return sel_b; }
static void set_sel(size_t a, size_t b) { sel_a = a; sel_b = b; caretp = b; }
static size_t line_count(void) { size_t n = 0; for (size_t i = 0; i < dlen; i++) if (doc[i] == '\n') n++; if (dlen && doc[dlen - 1] != '\n') n++; return n ? n : 1; }
static size_t line_start(size_t l) { size_t s = 0; for (size_t i = 0; i < dlen && l; i++) if (doc[i] == '\n') { s = i + 1; l--; } return s; }
static size_t line_of(size_t pos) { size_t l = 0; for (size_t i = 0; i < pos && i < dlen; i++) if (doc[i] == '\n') l++; return l; }
static void output(const char *l) { strcat(g_out, l); strcat(g_out, "\n"); }
static void status(const char *m) { snprintf(g_status, sizeof g_status, "%s", m); }
static void (*cmds[32])(void); static const char *labels[32]; static int ncmd;
static int add_command(const char *l, void (*fn)(void)) { labels[ncmd] = l; cmds[ncmd++] = fn; return 0; }
static studio_api_t api;
static void setdoc(const char *t) { dlen = strlen(t); memcpy(doc, t, dlen + 1); sel_a = sel_b = 0; }
static void run(const char *label) { for (int i = 0; i < ncmd; i++) if (!strcmp(labels[i], label)) { cmds[i](); return; } CHECK(0, "no command %s", label); }
static void base_init(cos_pbase_t *b) {
    memset(b, 0, sizeof *b); b->abi = COS_PBASE_ABI; b->status = status; b->snprintf = m_snprintf; b->log = (void (*)(const char *))puts;
    b->datetime = NULL;
}
static void host_setup(void) {
    memset(&api, 0, sizeof api); base_init(&api.base); api.abi = STUDIOP_ABI; api.size = sizeof api; api.add_command = add_command; api.has_doc = has_doc; api.doc_path = doc_path; api.doc_length = doc_length;
    api.doc_read = doc_read; api.doc_replace = doc_replace; api.caret = caret; api.sel_start = sel_start; api.sel_end = sel_end; api.set_selection = set_sel; api.line_count = line_count;
    api.line_start = line_start; api.line_of = line_of; api.output = output; api.open_file = NULL; api.project_root = NULL; ncmd = 0;
}
/* the plugin sources, each in its own scope via renamed statics */
#define PLUGIN_UNDER_TEST(file) 
#include "../../userland/plugins/studio/texttools.c"
static void test_texttools(void) {
    host_setup(); cos_plugin_host_t h = { 1, sizeof h, "studio", &api };
    CHECK(init(&h) == 0, "texttools init");
    setdoc("b  \nc\t\na\n"); run("Text: Trim trailing spaces"); CHECK(!strcmp(doc, "b\nc\na\n"), "trim: %s", doc);
    setdoc("\tx\n  \ty\n"); run("Text: Tabs to spaces"); CHECK(!strcmp(doc, "    x\n    y\n"), "untab: [%s]", doc);
    setdoc("pear\napple\nfig\n"); run("Text: Sort lines"); CHECK(!strcmp(doc, "apple\nfig\npear\n"), "sort: %s", doc);
    setdoc("a\nb\na\nc\nb\n"); run("Text: Remove duplicate lines"); CHECK(!strcmp(doc, "a\nb\nc\n"), "dedupe: %s", doc);
    setdoc("1\n2\n3"); run("Text: Reverse lines"); CHECK(!strcmp(doc, "3\n2\n1"), "reverse: %s", doc);
    setdoc("Hello World"); run("Text: UPPERCASE"); CHECK(!strcmp(doc, "HELLO WORLD"), "upper: %s", doc);
    setdoc("Hello World"); sel_a = 0; sel_b = 5; run("Text: lowercase"); CHECK(!strcmp(doc, "hello World"), "lower sel: %s", doc);
    setdoc("z\ny\nx\nw\n"); sel_a = 2; sel_b = 5; run("Text: Sort lines"); CHECK(!strcmp(doc, "z\nx\ny\nw\n"), "sort selection (whole lines): %s", doc);
    fini();
}
void run_studio_tests(void) { test_texttools(); }
