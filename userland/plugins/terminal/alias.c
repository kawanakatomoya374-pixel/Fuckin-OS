/* alias.c - plugin of the Terminal: alias NAME=TEXT (kept in the plugin's settings, so it survives restarts), unalias NAME. A line whose first word is an alias is rewritten. */
#include "cos_plugin.h"
#include "cos_plugin_terminal.h"
#include "cos_plugin_util.h"
static const term_api_t *A;
static int rewrite(const char *line, char *out, size_t cap) {
    char name[40]; size_t i = 0; while (line[i] && line[i] != ' ' && i < sizeof name - 1) { name[i] = line[i]; i++; } name[i] = 0;
    char key[48], val[160]; A->base.snprintf(key, sizeof key, "a_%s", name);
    if (!name[0] || A->base.cfg_get("alias", key, val, sizeof val) != 0 || !val[0]) return 0;
    A->base.snprintf(out, cap, "%s%s", val, line + i); return 1;
}
static int cmd_alias(int argc, char **argv, const term_io_t *io) {
    if (argc < 2) { io->print("usage: alias NAME=TEXT   (the line  NAME args  then runs  TEXT args)"); return 1; }
    char all[200] = ""; for (int i = 1; i < argc; i++) { if (i > 1) pu_cat(all, sizeof all, " "); pu_cat(all, sizeof all, argv[i]); }
    char *eq = all; while (*eq && *eq != '=') eq++;
    if (!*eq || eq == all) { io->print("alias: NAME=TEXT expected"); return 1; }
    *eq = 0; char key[60]; A->base.snprintf(key, sizeof key, "a_%s", all);
    if (A->base.cfg_set("alias", key, eq + 1) != 0) { io->print("alias: cannot save"); return 1; }
    char m[260]; A->base.snprintf(m, sizeof m, "alias %s = %s", all, eq + 1); io->print(m); return 0;
}
static int cmd_unalias(int argc, char **argv, const term_io_t *io) {
    if (argc < 2) { io->print("usage: unalias NAME"); return 1; }
    char key[60]; A->base.snprintf(key, sizeof key, "a_%s", argv[1]);
    if (A->base.cfg_set("alias", key, "") != 0) return 1;      /* an empty value is an empty rewrite: it is dropped below */
    io->print("alias removed"); return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const term_api_t *)h->api;
    if (!A || A->abi != TERMP_ABI) return -1;
    if (A->add_command("alias", "alias NAME=TEXT   a short name for a command line", cmd_alias)) return -1;
    A->add_command("unalias", "unalias NAME", cmd_unalias);
    return A->add_rewriter(rewrite) ? -1 : 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("terminal", "alias", "1.0", init, fini);
