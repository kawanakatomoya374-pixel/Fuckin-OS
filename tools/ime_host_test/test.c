/* Host test for the C-OS IME: types romaji, converts, prints the bunsetsu and candidates.
 * usage: test <cosime.dic> [romaji...]  ("space" = Space, "right"/"left"/"enter" keys are written as {right} etc.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cosime.h"
int cos_stat(const char *p, cos_stat_t *o) { struct stat st; if (stat(p, &st)) return -1; o->size = (uint64_t)st.st_size; o->is_dir = 0; return 0; }
ssize_t cos_read_file(const char *p, void *b, size_t n) { FILE *f = fopen(p, "rb"); if (!f) return -1; size_t r = fread(b, 1, n, f); fclose(f); return (ssize_t)r; }

static int key(cosime *m, uint8_t sp, char ch, uint8_t mods, char *out, size_t cap) {
    cos_win_event_t ev; memset(&ev, 0, sizeof ev); ev.special = sp; ev.ascii = ch; ev.mods = mods;
    return cosime_key(m, &ev, out, cap);
}
static void show(cosime *m) {
    printf("  [%s]", cosime_text(m));
    if (cosime_state(m) == COSIME_CONVERTING) {
        printf("  segs:"); for (int i = 0; i < cosime_nseg(m); i++) printf(" %s%s", i == cosime_focus(m) ? "*" : "", cosime_seg_text(m, i));
        printf("\n    cands:"); for (int i = 0; i < cosime_ncand(m); i++) printf(" %d.%s", i + 1, cosime_cand(m, i));
    }
    printf("\n");
}
int main(int argc, char **argv) {
    cosime *m = cosime_new();
    clock_t t0 = clock();
    if (cosime_load(m, argv[1]) != 0) { printf("load failed\n"); return 1; }
    printf("loaded in %.2fs\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    cosime_set_enabled(m, true);
    for (int a = 2; a < argc; a++) {
        const char *s = argv[a]; char out[1024];
        printf("%s\n", s);
        t0 = clock();
        for (; *s; s++) {
            if (*s == '{') {
                char name[16]; int n = 0; s++; while (*s && *s != '}' && n < 15) name[n++] = *s++; name[n] = 0;
                uint8_t sp = 0, mods = 0;
                if (!strcmp(name, "enter")) sp = COS_KEY_ENTER; else if (!strcmp(name, "right")) sp = COS_KEY_RIGHT; else if (!strcmp(name, "left")) sp = COS_KEY_LEFT;
                else if (!strcmp(name, "sright")) { sp = COS_KEY_RIGHT; mods = COS_MOD_SHIFT; } else if (!strcmp(name, "sleft")) { sp = COS_KEY_LEFT; mods = COS_MOD_SHIFT; }
                else if (!strcmp(name, "bs")) sp = COS_KEY_BACKSPACE; else if (!strcmp(name, "esc")) sp = COS_KEY_ESC; else if (!strcmp(name, "down")) sp = COS_KEY_DOWN; else if (!strcmp(name, "f7")) sp = COS_KEY_F1 + 6;
                int r = key(m, sp, 0, mods, out, sizeof out);
                if (r & COSIME_COMMIT) printf("  COMMIT: %s\n", out);
                continue;
            }
            int r = key(m, 0, *s, 0, out, sizeof out);
            if (r & COSIME_COMMIT) printf("  COMMIT: %s\n", out);
            if (!(r & COSIME_CONSUMED)) printf("  (passed '%c')\n", *s);
        }
        printf("  (%.0f ms)\n", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
        show(m);
        key(m, COS_KEY_ESC, 0, 0, out, sizeof out); key(m, COS_KEY_ESC, 0, 0, out, sizeof out);
    }
    return 0;
}
