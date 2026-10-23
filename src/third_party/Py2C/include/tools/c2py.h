#ifndef C2PY_H
#define C2PY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * c2py: 簡易 C -> Python 変換器
 * ------------------------------------------------------------
 * python_code_to_cとは非対称な設計です。python_code_to_cはPythonの動的意味論を保った
 * フルAST変換ですが、c2pyは「型情報を捨てて読みやすいPythonの
 * スケッチを作る」ことを目的にした軽量なトークンベース変換器です。
 *
 * 対応する範囲（意図的に絞っています）:
 *   - 関数定義: 戻り値型・引数型を捨てて def name(args): に変換
 *   - 変数宣言 (int/float/double/char/long/short/unsigned等) の破棄
 *     と、初期化式がある場合の代入への変換
 *   - if / else if / else, while, 単純な for(初期化;条件;増分)
 *   - break / continue / return
 *   - printf の簡易的な print への変換（書式指定子の解釈は簡易）
 *   - 論理演算子 && || ! の and/or/not への変換
 *   - true/false/NULL の True/False/None への変換
 *
 * 対応しない範囲（コメントとして出力し、手動対応を促します）:
 *   - ポインタ演算・アドレス取得（&）・deref（*p）の正確な意味
 *   - 構造体 / union / typedef / enum
 *   - 配列の宣言サイズやスタック確保
 *   - マクロ (#define) の展開
 *   - 複雑な書式指定を伴う printf/scanf
 *
 * 戻り値: 0=成功, 非0=失敗（out_pythonにエラー概要が入る）
 */
int c_to_python(const char *c_code, char **out_python_code);

#ifdef __cplusplus
}
#endif

#endif /* C2PY_H */
