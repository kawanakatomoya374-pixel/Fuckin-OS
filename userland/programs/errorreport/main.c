/* errorreport.c-os - the "this program has stopped working" window.
 *
 * The kernel starts it (src/kernel/cos_crash.c) when a ring-3 program was ended by a CPU exception, with the facts in the arguments:
 *   ErrorReport NAME CODE EXC_EN EXC_JA PID RIP ADDR NOTE
 * It only shows them: a program of the same name can be started again from wherever it came from; nothing else was touched (the kernel and every other
 * program run on). The window has a short text, the error code (0xC0VVEEEE: V = the CPU exception number, E = its error code) and a details panel whose
 * text can be copied to the clipboard for a bug report. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

static bool in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "?", *code = argc > 2 ? argv[2] : "0xC0000000", *exc_en = argc > 3 ? argv[3] : "error", *exc_ja = argc > 4 ? argv[4] : "エラー";
    const char *pid = argc > 5 ? argv[5] : "?", *rip = argc > 6 ? argv[6] : "?", *addr = argc > 7 ? argv[7] : "?", *note = argc > 8 ? argv[8] : "";
    cos_settings_t st; bool jp = cos_settings_get(&st) == 0 && st.japanese, dark = true;
    if (cos_settings_get(&st) == 0) dark = st.dark_mode != 0;

    const int W = 600, H_LONG = 356;
    cos_win_info_t wi; int64_t h = cos_win2_create(jp ? "プログラムのエラー" : "Program error", W, H_LONG, &wi);
    if (h <= 0) return 1;
    cui_canvas c; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_font *f_title = cui_font_bold(19), *f_body = cui_font_default(14), *f_code = cui_font_bold(15), *f_small = cui_font_default(13), *f_btn = cui_font_default(14);
    uint32_t BG = dark ? 0x1B1F2B : 0xF5F7FB, PANEL = dark ? 0x232838 : 0xFFFFFF, TEXT = dark ? 0xE8ECF5 : 0x1A2030, MUTED = dark ? 0x9AA3B8 : 0x5A6478, RED = 0xE5484D, BTN = dark ? 0x32394D : 0xE1E6F0, ACC = 0x3D7BF0;

    char title[200], l1[200], l2[200], codeline[160], what[160], d1[160], d2[160], d3[160], d4[200];
    if (jp) {
        snprintf(title, sizeof title, "%s は動作を停止しました", name);
        snprintf(l1, sizeof l1, "問題が発生したため、このプログラムを終了しました。");
        snprintf(l2, sizeof l2, "ほかのプログラムとシステムは、そのまま使えます。");
        snprintf(codeline, sizeof codeline, "エラーコード:  %s", code);
        snprintf(what, sizeof what, "内容:  %s", exc_ja);
    } else {
        snprintf(title, sizeof title, "%s has stopped working", name);
        snprintf(l1, sizeof l1, "A problem made C-OS end this program.");
        snprintf(l2, sizeof l2, "Other programs and the system are not affected.");
        snprintf(codeline, sizeof codeline, "Error code:  %s", code);
        snprintf(what, sizeof what, "What happened:  %s", exc_en);
    }
    snprintf(d1, sizeof d1, "%s: %s   PID: %s", jp ? "プログラム" : "Program", name, pid);
    snprintf(d2, sizeof d2, "%s: %s", jp ? "命令の位置 (RIP)" : "Instruction (RIP)", rip);
    snprintf(d3, sizeof d3, "%s: %s", jp ? "アクセスしようとした場所" : "Address touched", addr);
    snprintf(d4, sizeof d4, "%s", jp ? "記録: /system/crash.log (問い合わせのときは、この内容を伝えてください)" : "Logged in /system/crash.log (quote this when you report a bug)");
    char copy[900]; snprintf(copy, sizeof copy, "%s\n%s\n%s\nPID %s  RIP %s  ADDR %s\n%s\n", title, codeline, what, pid, rip, addr, note);

    bool idk = ((cos_time_ms() * 2654435761ull) >> 12) % 20 == 0;                      /* a joke: about one error window in twenty has "IDK :/" on its close button */
    int hover = -1; bool copied = false; uint64_t copied_t = 0;
    const int bw = 130, bh = 36;
    for (;;) {
        int btn_y = H_LONG - 58;
        int bx[2] = { W - 2 * bw - 28, W - bw - 16 };           /* copy, close */
        const char *bl[2] = { copied ? (jp ? "コピーしました" : "Copied") : (jp ? "内容をコピー" : "Copy text"), idk ? "IDK :/" : (jp ? "閉じる" : "Close") };

        cui_fill(&c, 0, 0, W, H_LONG, BG);
        cui_circle(&c, 52.0f, 62.0f, 26.0f, RED);
        cui_line(&c, 41.0f, 51.0f, 63.0f, 73.0f, 4.0f, 0xFFFFFF); cui_line(&c, 63.0f, 51.0f, 41.0f, 73.0f, 4.0f, 0xFFFFFF);
        cui_text(&c, f_title, 98, 34, title, TEXT);
        cui_text(&c, f_body, 98, 70, l1, TEXT); cui_text(&c, f_body, 98, 92, l2, MUTED);
        cui_round_rect(&c, 24, 126, W - 48, 62, 8, PANEL);
        cui_text(&c, f_code, 40, 134, codeline, TEXT); cui_text(&c, f_small, 40, 160, what, MUTED);
        {
            cui_round_rect(&c, 24, 198, W - 48, 94, 8, PANEL);
            cui_text(&c, f_small, 40, 208, d1, TEXT); cui_text(&c, f_small, 40, 230, d2, MUTED); cui_text(&c, f_small, 40, 252, d3, MUTED);
            cui_text_fit(&c, f_small, 40, 276, W - 80, d4, MUTED);
        }
        for (int i = 0; i < 2; ++i) {
            bool primary = i == 1;
            uint32_t col = primary ? (hover == i ? cui_mix(ACC, 0xFFFFFF, 40) : ACC) : (hover == i ? cui_mix(BTN, TEXT, 30) : BTN);
            cui_round_rect(&c, bx[i], btn_y, bw, bh, 8, col);
            cui_text_center(&c, f_btn, bx[i], btn_y, bw, bh, bl[i], primary ? 0xFFFFFF : TEXT);
        }
        cos_win2_present(h);

        cos_win_event_t ev; int r = cos_win2_wait(h, &ev, 250);
        if (r > 0) {
            if (ev.type == COS_EV_CLOSE) break;
            if (ev.type == COS_EV_KEY && (ev.special == COS_KEY_ESC || ev.special == COS_KEY_ENTER)) break;
            if (ev.type == COS_EV_MOUSE_MOVE) { hover = -1; for (int i = 0; i < 2; ++i) if (in(ev.x, ev.y, bx[i], btn_y, bw, bh)) hover = i; }
            if (ev.type == COS_EV_MOUSE_DOWN) {
                for (int i = 0; i < 2; ++i) if (in(ev.x, ev.y, bx[i], btn_y, bw, bh)) {
                    if (i == 0) { cos_clip_set(copy, strlen(copy)); copied = true; copied_t = cos_time_ms(); }
                    else goto done;
                }
            }
        }
        if (copied && cos_time_ms() - copied_t > 2500) copied = false;
    }
done:
    cos_win2_close(h);
    return 0;
}
