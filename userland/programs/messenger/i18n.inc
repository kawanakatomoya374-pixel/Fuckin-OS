/* i18n.inc - the interface language follows the system setting (Settings > Language). Every user-visible English literal is written T("...") and
 * is looked up here; with the system in Japanese the Japanese text is used, otherwise (and for anything not in the list) the text as written. */
/* The Japanese text lives in a language pack, a shared library of data (build/langpacks/ja.c-osll, installed as /system/lang/ja.c-osll) that is loaded
 * with cos_dlopen() the first time the language is Japanese. Without the pack (or with a broken one) the program simply shows its English text. */
struct lang_entry { const char *src, *dst; };
struct lang_pack { uint32_t magic, version, count, reserved; const char *id; const struct lang_entry *tab; };
static bool ui_ja;
static const struct lang_pack *lang_pk;
static bool lang_tried;
static bool lang_ptr_ok(const void *p) { return p && (uint64_t)p >= 0x10000 && (uint64_t)p < 0x00007FFFFFFFF000ULL; }
static void lang_load(void) {                                     /* only from /system/lang: the pack is code that runs inside this program */
    lang_tried = true;
    int64_t h = cos_dlopen("/system/lang/ja.c-osll"); if (h < 0) { cos_puts("[MESSENGER] i18n: no Japanese language pack\n"); return; }
    const struct lang_pack *p = (const struct lang_pack *)cos_dlsym(h, "cos_lang_pack");
    if (!lang_ptr_ok(p) || p->magic != 0x474E4C43u || p->version != 1 || p->count == 0 || p->count > 8192 || !lang_ptr_ok(p->tab)) { cos_puts("[MESSENGER] i18n: the language pack is not valid\n"); return; }
    for (uint32_t i = 0; i < p->count; i++) if (!lang_ptr_ok(p->tab[i].src) || !lang_ptr_ok(p->tab[i].dst)) { cos_puts("[MESSENGER] i18n: the language pack is not valid\n"); return; }
    lang_pk = p; cos_puts("[MESSENGER] i18n: Japanese language pack loaded\n");
}
static const char *T(const char *en) {
    if (!ui_ja) return en;
    if (!lang_tried) lang_load();
    if (!lang_pk) return en;
    for (uint32_t i = 0; i < lang_pk->count; i++) if (lang_pk->tab[i].src[0] == en[0] && strcmp(lang_pk->tab[i].src, en) == 0) return lang_pk->tab[i].dst;
    return en;
}
