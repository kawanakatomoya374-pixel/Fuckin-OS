#ifndef PYTHON_CODE_TO_C_GUI_WEB_H
#define PYTHON_CODE_TO_C_GUI_WEB_H

#ifdef __cplusplus
extern "C" {
#endif

/* python_code_to_cのブラウザベースGUIサーバーのエントリポイント。
 * ポート(デフォルト8765)でlocalhost専用のHTTPサーバーを起動し、
 * ブラウザ(xdg-open/open)で自動的に開く。Ctrl-Cで終了するまで戻らない。
 * argv[1..]で --port=N / --host=<addr> / --no-browser が指定できる。 */
int p2c_gui_web_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
