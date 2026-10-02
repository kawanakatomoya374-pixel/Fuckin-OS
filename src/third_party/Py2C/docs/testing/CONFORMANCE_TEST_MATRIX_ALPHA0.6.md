# Python Code to C Alpha0.6 — 意味論コンフォーマンステスト・マトリクス

## 目的と判定方法

このマトリクスは、**383件の意味論回帰**を含み、件数を満たすための例ではなく、コンパイラ・生成C・ランタイムの**失敗モードを検出する契約テスト**を定義する。`C01`から`C60`は `tests/complex_alpha06.py` の実値差分コーパス、`C61`から`C80`は評価順序・制御移譲・アンパック・可変コンテナ・集合を深掘りする補強コーパスに属する。通常ケースはCPython出力との完全一致、順序非保証の集合は`sorted()`正規化後の一致、診断ケースは期待エラー文字列の一致で判定する。

| 範囲 | ケース数 | 主な検証層 | 合格条件 |
|---|---:|---|---|
| C01–C15 | 15 | 式・数値・文字列・list | CPython差分一致 |
| C16–C30 | 15 | 内包表記・tuple・dict・組込み関数 | CPython差分一致 |
| C31–C45 | 15 | 制御・関数呼出し・例外・クラス | CPython差分一致 |
| C46–C60 | 15 | 書式化・代入・変換・コンテナメソッド | CPython差分一致 |
| C61–C80 | 20 | 高リスク意味論・set・副作用・反復・スコープ | CPython差分一致、期待診断、または期待ビルド |
| C81–C83 | 3 | dict内包表記 | CPython差分一致 |
| C84–C85 | 2 | 複数コンテキストwith | CPython差分一致 |
| C86–C92 | 7 | `dict()`組込み構築 | CPython差分一致 |
| C93–C97 | 5 | 空コンテナ・逆順反復・文字列join | CPython差分一致 |
| C98–C100 | 3 | `enumerate`開始値 | CPython差分一致 |
| C101–C108 | 8 | 辞書順序・更新・削除・clear後再利用 | CPython差分一致 |
| C109–C117 | 9 | setメソッド・set/listコピー | CPython差分一致 |
| C118–C126 | 9 | 集合演算・等値比較・拡張代入 | CPython差分一致 |
| C127–C128 | 2 | 辞書値等値比較 | CPython差分一致 |
| C129–C132 | 4 | 数値ハッシュ等値契約 | CPython差分一致 |
| C133–C144 | 12 | 非ハッシュ可能キー・要素 | CPython差分一致 |
| C145–C148 | 4 | tuple値ハッシュ契約 | CPython差分一致 |
| C149–C161 | 13 | slice、負添字、添字型 | CPython差分一致 |
| C162–C167 | 6 | 固定幅整数の境界内算術 | CPython差分一致 |
| C168–C173 | 6 | f-string・二項式の評価順 | CPython差分一致 |
| C174 | 1 | 型注釈の実行時無視 | CPython差分一致 |
| C175–C178 | 4 | 代入式`:=` | CPython差分一致 |
| C179–C189 | 11 | list/dict/set拡張メソッド | CPython差分一致 |
| C190–C194 | 5 | match/case、or-pattern、guard | CPython差分一致 |
| C195–C197 | 3 | 辞書マージ`|`・`|=` | CPython差分一致 |
| C198–C199 | 2 | bare raiseとnested try | CPython差分一致 |
| C200–C203 | 4 | 文字列拡張メソッド | CPython差分一致 |
| C204–C207 | 4 | `for`ターゲットのstarred unpack | CPython差分一致 |
| C208–C211 | 4 | `print(sep=..., end=...)` | CPython差分一致 |
| C212–C215 | 4 | `str.index()`・`str.rindex()` | CPython差分一致 |
| C216–C220 | 5 | 回数指定`str.replace()`・空文字列置換 | CPython差分一致 |
| C221–C224 | 4 | 開始・終了境界付き`list.index()` | CPython差分一致 |
| C225–C234 | 10 | `str.find`/`index`/`rfind`/`rindex`/`count`のstart/stop境界 | CPython差分一致 |
| C235–C240 | 6 | `str.split(sep, maxsplit)` | CPython差分一致 |
| C241–C250 | 10 | `strip(chars)`、prefix/suffix境界、文字列タプル | CPython差分一致 |
| C251–C256 | 6 | `str.partition()`・`str.rpartition()` | CPython差分一致 |
| C257–C268 | 12 | ASCII文字種判定、`splitlines(keepends)`、制御文字リテラル・repr | CPython差分一致 |
| C269–C274 | 6 | `str.expandtabs(tabsize)` | CPython差分一致 |
| C275–C282 | 8 | `dict.fromkeys`、pair iterable `dict.update`、set包含関係・`pop` | CPython差分一致 |
| C283–C287 | 5 | generator再開、generator for反復、`async def`、`await`、`asyncio.run` | CPython差分一致 |
| C288–C299 | 12 | UTF-8文字コード・基数変換・開始値付き`sum` | CPython差分一致 |
| C300–C302 | 3 | 位置専用引数`/`、通常呼出し、キーワード誤用の`TypeError` | CPython差分一致 |
| C303 | 1 | `yield from`のlist/tuple委譲、終了後のyield、収集型組込み反復 | CPython差分一致 |
| C304 | 1 | `raise exc from cause`の例外伝播・cause連鎖生成 | CPython差分一致 |
| C305–C314 | 10 | sequence/mapping/as/star/OR/guard、mapping `**rest` | CPython差分一致 |
| C315–C322 | 8 | bare/keyword属性class pattern、nested pattern、as/guard、組込み型、属性不在fallback | CPython差分一致 |
| C323–C328 | 6 | `__match_args__` class positional pattern、位置/keyword混在、組込み型、fallback | CPython差分一致 |
| C329–C331 | 3 | dotted value pattern、属性連鎖、非一致fallback | CPython差分一致 |
| C332–C334 | 3 | dotted class pattern、動的class object照合、位置/keyword属性、fallback | CPython差分一致 |
| C335–C339 | 5 | generator expression、単一for、filter、反復状態 | CPython差分一致 |
| C340 | 1 | 比較・短絡論理を含むawait代入式 | CPython差分一致 |
| C341–C342 | 2 | async forの自然終了else、break・continue | CPython差分一致 |
| C343–C346 | 4 | nonlocal共有cell、複数closure間の再束縛可視性 | CPython差分一致 |
| C347 | 1 | 関数ローカル自由変数をcaptureするlambda | CPython差分一致 |
| C348 | 1 | 関数ローカル自由変数をcaptureするgenerator expressionと唯一の関数引数での括弧省略 | CPython差分一致 |
| C349 | 1 | generator expression唯一引数parserのtoken位置安全性 | CPython差分一致、ASan/UBSan診断なし |
| C350–C353 | 4 | `sorted`/`min`/`max`の`key=`、`reverse=`、未知keyword拒否 | CPython差分一致、サイレント無視なし |
| C354 | 1 | `raise ... from ...`後のexception `__cause__`参照 | CPython差分一致 |
| C355 | 1 | 括弧付き複数context managerと末尾カンマ | CPython差分一致 |
| C356–C357 | 2 | `with ... as`のtuple/listアンパックtarget | CPython差分一致 |
| C358–C362 | 5 | `async with`の正常終了、例外抑止、例外再送出、protocol method内await | CPython差分一致 |
| C363–C365 | 3 | set comprehensionの重複排除、filter、複数`for` | CPython差分一致。`--c11`では期待診断 |
| C366–C372 | 7 | function/class decorator、評価順、callable再束縛、default引数、async function | CPython差分一致。未対応組合せは期待診断 |
| C373–C377 | 5 | 複数itemの`async with`正常経路、左から右のenter、`as`束縛、右から左のexit | CPython差分一致 |
| C378–C383 | 6 | 複数itemの`async with`例外cleanup、内側から外側のexit、外側truthyによる抑止 | CPython差分一致 |
| P01–P23 | 23 | strict C11・freestanding・GC/GUI・単一ヘッダー・整数境界・プラットフォーム出力・make起動・決定的ファジング・ジェネレータ・ベアメタル・Python 3.13型構文・GC lifecycle | 期待ビルドまたは期待診断 |

## C01–C60: 横断コーパス

| ID | 主張する契約 | 代表入力 | 主な故障モード |
|---|---|---|---|
| C01 | 乗算は加算より先に評価される | `1 + 2 * 3` | 演算子優先順位 |
| C02 | 整数除算はPythonの床除算になる | `(10 - 3) // 2` | 除算丸め |
| C03 | 整数累乗の値が保持される | `2 ** 8` | 累乗実装 |
| C04 | 剰余演算の基本値 | `17 % 5` | 算術変換 |
| C05 | 単項負号と加算 | `-7 + 12` | 単項式 |
| C06 | 比較値を使った論理式 | `3 < 4 and 5 >= 5` | 比較・truthiness |
| C07 | `not`の真偽反転 | `not (2 == 3)` | 単項論理 |
| C08 | 文字列連結 | `"Py" + "thon"` | 文字列所有権 |
| C09 | 文字列大文字化 | `"alpha".upper()` | 組込みメソッド分派 |
| C10 | 分割結果の添字 | `"alpha beta".split()[1]` | 一時list・添字 |
| C11 | 正の文字列添字 | `"abc"[1]` | 文字列境界 |
| C12 | ステップ付き文字列slice | `"abcdef"[1:5:2]` | slice境界 |
| C13 | list長さ | `len([1, 2, 3])` | コンテナ長 |
| C14 | list連結 | `[1, 2] + [3, 4]` | list新規確保 |
| C15 | 負のlist添字 | `[1, 2, 3][-1]` | 負インデックス |
| C16 | 基本list内包表記 | `[x*x for x in ...]` | 生成器コード生成 |
| C17 | 条件付き内包表記 | `[x for x in ... if ...]` | フィルタ評価 |
| C18 | iterableからtupleへ変換 | `tuple([1,2,3])` | iterable変換 |
| C19 | tuple長さ | `len((...))` | tupleランタイム |
| C20 | 辞書添字取得 | `{"a":1,"b":2}["b"]` | ハッシュ検索 |
| C21 | 辞書更新後の長さ | `d["b"] = 2` | dict更新 |
| C22 | 辞書のキー包含 | `"x" in d` | dict包含 |
| C23 | listの要素包含 | `4 in [...]` | 線形包含 |
| C24 | rangeからlist化 | `list(range(3))` | range・反復 |
| C25 | iterable合計 | `sum([...])` | 反復集約 |
| C26 | iterable最大値 | `max([...])` | 比較集約 |
| C27 | iterable最小値 | `min([...])` | 比較集約 |
| C28 | 整列結果 | `sorted([...])` | sort・比較 |
| C29 | 丸め | `round(2.5)` | 数値丸め |
| C30 | 絶対値 | `abs(-42)` | 数値変換 |
| C31 | for累積 | `for`と`+=` | ループ変数・複合代入 |
| C32 | while反復 | `while value < ...` | 条件再評価 |
| C33 | for中断 | `break` | ループ脱出 |
| C34 | for継続 | `continue` | 次反復制御 |
| C35 | 関数デフォルト引数 | `f(a=3,b=4)` | デフォルト補完 |
| C36 | `*args`束縛 | `sum(values)` | 可変長引数 |
| C37 | `**kwargs`束縛 | `values.get(...)` | キーワード辞書 |
| C38 | 呼出し側`*`アンパック | `f(*[...])` | 引数展開 |
| C39 | キーワード呼出し | `f(x=..., y=...)` | 引数対応付け |
| C40 | ゼロ除算捕捉 | `try/except` | 例外伝播 |
| C41 | 明示raiseと束縛 | `raise ValueError` | 例外型一致 |
| C42 | finally実行保証 | `try/finally` | cleanup制御 |
| C43 | メソッド呼出し | `Counter(...).inc(...)` | `self`・属性 |
| C44 | `__str__`優先 | `str(Counter(...))` | 特殊メソッド |
| C45 | 可変インスタンス属性 | 複数回`inc` | 属性更新 |
| C46 | f-string書式 | `:02d` | 書式変換 |
| C47 | `str.format` | `"{}:{}".format(...)` | format分派 |
| C48 | タプルアンパック | `a,b = [...]` | 代入展開 |
| C49 | list slice | `values[1:]` | slice終端 |
| C50 | 辞書デフォルト取得 | `d.get(k, default)` | dictメソッド |
| C51 | bool変換 | `bool(1)` | truthiness |
| C52 | floatからint | `int(3.9)` | 数値変換 |
| C53 | intからfloat | `float(4)` | 数値変換 |
| C54 | 文字列長 | `len("portable")` | 文字列長 |
| C55 | 接頭辞判定 | `startswith` | 文字列メソッド |
| C56 | 接尾辞判定 | `endswith` | 文字列メソッド |
| C57 | 文字列置換 | `replace` | 文字列生成 |
| C58 | list末尾pop | `pop()` | list変異 |
| C59 | 辞書キー列 | `list(d.keys())` | キー反復 |
| C60 | `all`/`any` | 混在truthiness | 集約短絡 |

## C61–C80: 高リスク意味論コーパス

| ID | 主張する契約 | 重点理由 |
|---|---|---|
| C61 | `and`は左の偽オペランド自身を返し、右を評価しない | Cの`&&`への誤変換を防ぐ |
| C62 | `or`は左の真オペランド自身を返し、右を評価しない | Cの`||`への誤変換を防ぐ |
| C63 | `for ... else`はbreak時にelseを実行しない | 制御フラグの逆転を防ぐ |
| C64 | `while ... else`は自然終了時のみelseを実行する | break追跡を検証する |
| C65 | 属性複合代入の受け手評価は一度だけ | 副作用の二重評価を防ぐ |
| C66 | 添字複合代入のコンテナ・キー評価は各一度だけ | 左辺展開の順序を固定する |
| C67 | starred unpackは先頭・中間・末尾を正しく分配する | 可変長代入境界を検証する |
| C68 | dict `**` unpackは左から右で後勝ちになる | 挿入順・上書き規則を検証する |
| C69 | `del`属性と`del`添字は対象だけを削除する | 変異・例外経路を検証する |
| C70 | bit演算とシフトは符号付き整数でPython値を返す | 演算子コード生成を検証する |
| C71 | setリテラルは重複を除外し、包含・長さを提供する | hashコンテナの基本契約 |
| C72 | `set(iterable)`は任意のシーケンスを重複排除する | iterable変換を検証する |
| C73 | set内包表記はfilter・複数for・重複排除を組み合わせる | statement expression生成を検証する |
| C74 | `frozenset`フォールバックは読み取り操作を継続できる | 非停止互換経路を検証する |
| C75 | `with ... as value`はenter/body/exitを順に一度ずつ実行する | リソースcleanupとenter値束縛を検証する |
| C76 | as句なしの`with`でenter結果を破棄し、truthyな`__exit__`は本体例外を抑止する | C生成の未使用変数警告と例外抑止の意味論を検証する |
| C77 | `finally`のreturn/break/continueは、finally本体を実行したうえで制御を移す（CPythonと出力一致） | 危険な制御移譲を正しくlowerする |
| C78 | strict ISO C11可能コードは`-pedantic-errors`でコンパイルする | GCC依存の境界を検証する |
| C79 | strict ISO C11非対応式は理由付きで拒否する | サイレントな不正生成を防ぐ |
| C80 | freestandingコアは標準ライブラリ依存なしでアーカイブ化される | 自作OS組込み契約を検証する |
| C81 | dict内包表記はキー・値式を各反復で評価する | 辞書構築コード生成を検証する |
| C82 | dict内包表記はifフィルタを適用する | 生成器条件を検証する |
| C83 | dict内包表記は複数forと同一キー後勝ちを保持する | ネスト反復・dict更新順序を検証する |
| C84 | 複数コンテキストwithは左からenterし右からexitする | ネスト展開と値束縛を検証する |
| C85 | 内側コンテキストの例外抑止後にも外側exitが実行される | 例外フレームとcleanup順を検証する |
| C86 | `dict()`は空辞書を構築する | C標準名との衝突回避を検証する |
| C87 | `dict(mapping)`は挿入順を保つ辞書コピーを構築する | 辞書反復・更新を検証する |
| C88 | `dict(pair_iterable)`は2要素シーケンスから構築する | 反復ペア展開を検証する |
| C89 | `dict(pair_iterable)`は重複キーを後勝ちで更新する | 辞書更新意味論を検証する |
| C90 | `dict(key=value)`はキーワード項目を構築する | GNU通常生成の式位置辞書構築を検証する |
| C91 | `dict(mapping, key=value)`は左から右に更新する | 既存マッピングとキーワードの優先順を検証する |
| C92 | `dict(**mapping, key=value)`はアンパック更新を処理する | `**mapping`とキーワード更新の統合を検証する |

| C93 | `list()`は空リストを構築する | 空コンテナ組込み経路を検証する |
| C94 | `tuple()`は空タプルを構築する | 空コンテナ組込み経路を検証する |
| C95 | `reversed(list)`は逆順の反復可能値を返す | 逆順materializationを検証する |
| C96 | `reversed(str)`は文字単位で逆順化する | 文字列反復の実装を検証する |
| C97 | `str.join(reversed(str))`が文字列反復を保持する | 逆順・join統合を検証する |
| C98 | `enumerate(iterable)`は0始まりである | 既定開始値を検証する |
| C99 | `enumerate(iterable, positive)`は任意の正開始値を使う | 第2位置引数を検証する |
| C100 | `enumerate(str, negative)`は負開始値を使う | 文字列反復と開始値を検証する |
| C101–C103 | dictの`keys`、`values`、`items`は挿入順を維持する | ハッシュバケット順への退行を防ぐ |
| C104 | `dict.update`はソースの挿入順で更新する | 辞書更新順を検証する |
| C105–C106 | `dict.pop`後の値とキー順が正しい | 削除時の順序リンク解除を検証する |
| C107–C108 | `dict.clear`後の長さと再利用が正しい | dangling順序リンクを防ぐ |
| C109–C110 | `set(set)`は元集合から独立する | コピー独立性を検証する |
| C111 | `discard`と`remove`が正しく要素を削除する | 集合削除を検証する |
| C112–C115 | `copy`、`clear`、`add`が独立した集合状態を保つ | 集合メソッドを検証する |
| C116–C117 | `list(list)`は元リストから独立する | リストコピー独立性を検証する |
| C118–C121 | setの和・積・差・対称差が正しい | 型別演算子ディスパッチを検証する |
| C122 | 順序が異なる同要素setは等しい | set値等値比較を検証する |
| C123–C126 | 集合拡張代入が演算結果を再束縛する | `|=`、`&=`、`-=`、`^=`を検証する |
| C127–C128 | dictは挿入順と異なる同値辞書を等しいと判定し、値差を検出する | dict値等値比較を検証する |
| C129–C132 | `1`、`True`、`1.0`はdict/setで同一キー・要素になる | 数値等値とハッシュの契約を検証する |
| C133–C144 | list/dict/setとそれらを含むtupleはキー・set要素・参照・包含で拒否される | 非ハッシュ可能値の全主要経路を検証する |
| C145–C148 | 内容が等しい別tupleはdict/setで同一の値キーとなる | tuple値ハッシュと数値等値の合成を検証する |
| C149–C155 | 負添字、範囲外、正負stepのsliceがlist/tuple/strでCPythonと一致する | 添字正規化と境界クランプを検証する |
| C156–C161 | float/str添字、float slice step、添字代入・削除が`TypeError`となる | 暗黙の0変換を防止する |
| C162–C167 | 床除算、剰余、負指数、符号付きシフトが64ビット範囲内でCPythonと一致する | 固定幅算術の境界内意味論を検証する |
| C168–C173 | 複数補間、書式指定、単一補間内の複数副作用は左から右へ評価される | Cの未規定引数評価順への退行を検出する |
| C174 | 変数・引数・戻り値・関数内ローカルの型注釈は実行時意味を変えない | 注釈受理と無視を検証する |

## P01–P08: プラットフォーム契約

| ID | 契約 | 実行場所 |
|---|---|---|
| P01 | Hosted CLIがC11警告ゼロでビルドできる | `make` |
| P02 | ローカルGUIフロントエンドがビルドできる | `make gui` |
| P03 | freestandingコアが静的ライブラリとして生成できる | `make freestanding` |
| P04 | GCルート・容量境界・GUIコマンドバッファがC APIで正しい | `make test-gc-gui` と専用境界ビルド |
| P05 | 生成Cの通常経路がCPythonと一致する | `make test` |
| P06 | Sanitizerが追加コンテナ・内包表記経路で不正アクセス/UBを報告しない | ASan/UBSan実行 |
| P07 | 単一ヘッダーだけを含む翻訳単位がGCC/Clangで警告即エラーによりビルド・実行でき、sequence・mapping・star・as・OR・guard・`**rest`・bare/keyword属性class patternを含む`match/case`、辞書マージ、for starred unpack、`print`キーワード引数を生成できる | `make test-single-header` |
| P08 | 64ビット境界の加減乗、除算、累乗、シフトが未定義動作ではなく安全な例外または定義済み値となる | `make test-integer-overflow` |
| P09 | GCCを使わず、Clangだけで単一ヘッダーを`-std=c11 -pedantic-errors`・警告即エラーでビルド・実行できる | `make CC=clang test-single-header-c11` |
| P10 | `PYTHON_CODE_TO_C_NO_STDLIB`の単一ヘッダー実装部がHostedアダプタ込みで翻訳でき、Hosted allocator・出力・時刻シンボルを要求しない | `make test-single-header-freestanding` |
| P11 | 登録済み`P2C_Platform.write`が`print(sep=..., end=...)`の出力を完全に受け取る | `make test-platform-adapter` |
| P12 | `make check-tools`、`make help`、`make run INPUT=...`がHosted起動契約を提供する | Makefile起動テスト |
| P13 | 汎用`freestanding-c11.mk`が任意の`CC`、`AR`、ABI追加フラグを受け取り構成を表示する | `print-config`とfreestandingアーカイブビルド |
| P14 | 決定的seedのdict/set操作列をCPythonと生成Cで比較し、失敗時には最小prefixと再現コマンドを保存する | `make test-container-fuzz` |
| P15 | 状態機械generatorの値・停止・再開、ネストawait、FIFO協調実行、GC到達性がC APIで正しい | `make test-generator-async-runtime` |
| P16 | 静的ヒープ、platform write、GC基点、tick、協調awaitを`PYTHON_CODE_TO_C_NO_STDLIB`で実行する | `make test-baremetal-runtime` |
| P17 | ベアメタルPython例を実際に変換し、起動・アダプタ例とともにfreestanding C11警告即エラーでオブジェクト化する | `make test-baremetal-build` と `make test-baremetal-generated` |
| P18 | `ord`/`chr`のUTF-8単一Unicodeスカラー値、`bin`/`oct`/`hex`の符号・接頭辞・小文字、開始値付き`sum`がCPythonと一致する | C288–C299、単一ヘッダー自己テスト |
| P19 | Python 3.13の`type`文、型パラメータ既定値、TypeVarTuple、ParamSpecを型消去で変換し、生成Cを厳格C11で実行できる | `make test-py313-syntax` |
| P20 | `yield from`の委譲値・StopIteration終了、位置専用引数のTypeError、`raise from`のcause参照が生成C・単一ヘッダー・GC契約と整合する | C300–C304、`make test-single-header` |
| P21 | 複数runtime epochでmodule/class registry、async queue、root table、pygame static slotが解放済みobjectを保持しない | `make test-gc-lifecycle` |
| P22 | constructor後段の`malloc`/`calloc`失敗がGC追跡リストをダングリング参照にしない | `make test-gc-allocation-failure` |
| P23 | shutdown後にruntime所有heapが残らない | `make CC=clang test-gc-leaks` |
| P24 | 保守的スタックスキャンが「現在のフレームから宣言上端まで」だけを走査し、スタック上の生存ローカルが収集されない（走査語数がスタック全長へ比例しない） | `make test-gc-stack-scan-scope` |
| P25 | 自作libcスタブ構成（libcなし）で`raise`/`except`が実際に機能し、プラットフォームのアロケータがランタイムと変換器の共有ヒープとして使われる | `make test-baremetal-exceptions` |
| P26 | カーネルのアロケータ（`p2c_platform_set_allocator`）と`P2C_Allocator`（`p2c_set_default_allocator`）の注入が、変換器コアとランタイムの両方へ効き、注入が確保に失敗する場合だけ静的フォールバック（既定64KiB、`P2C_COMPILER_FALLBACK_HEAP_SIZE`）へ落ちる | `make test-allocator-injection` |
| P27 | `P2C_SETJMP`/`P2C_LONGJMP`を差し替えた状態で、ランタイム本体（`p2c_raise`）と生成Cの例外機構がフックを通り、生の`setjmp`を出力しない | `make test-setjmp-hook` |
| P28 | NO_STDLIB構成で、カーネルアロケータの注入時にランタイム同梱libcスタブの`malloc/calloc/realloc/free`も共有ヒープへ委譲され（別ヒープを確保しない）、解除後はスタブの線形ヒープが唯一のヒープとして`realloc`で旧サイズを正しく扱い、ヒープ未設定では確保がNULLを返す | `make test-heap-unification` |
| P29 | 二項演算の左オペランドと処理中の例外が収集のルートに入り、式の途中の自動GCでも解放済みポインタを演算関数へ渡さない。右オペランドの評価中に例外が脱出しても深さが漏れず、200回の反復でネスト上限を誤発火しない（f-stringビルダも解放される） | `make test-gc-temp-roots`、`tests/audit_regression.sh`（GCH-010、CPython差分） |

> C01–C76およびC81–C383はCPython差分で実行する。C77–C80とA01は「通ること」ではなく、移植性と安全性の契約どおりに通過または拒否することを検証する。加えてP14は固定seedに基づく生成操作列をCPythonと差分比較し、P15–P17は状態機械・静的ヒープ・生成済みfreestanding Cを、P18は文字コード・基数変換・開始値付き集計を、P19–P20はPython 3.13型構文と新規制御・例外意味論を検証する。

## C175–C383: Alpha0.6拡張コーパス

| 範囲 | 主張する契約 | 主な故障モード |
|---|---|---|
| C175–C178 | `name := value`は値を返し、if、while、内包表記、入れ子式で一度だけ評価・束縛される | C未宣言変数、値を返さない代入、評価回数の差 |
| C179–C189 | `list.copy`、`dict.copy`、`dict.popitem`、setの非破壊・破壊的演算、`isdisjoint`はCPythonと同じ値・変異・順序を持つ | aliasing、順序破壊、集合更新の誤差 |
| C190–C194 | match subjectは一度だけ評価され、literal、`None`、or-pattern、capture、wildcard、guardが先頭一致規則で動く | subject再評価、capture前guard、複数case実行 |
| C195–C197 | `dict | dict`は左の順序を保ち、同一キーは右の値で更新され、`|=`は左dictを更新する | 値の優先順位、挿入順、非破壊性 |
| C198–C199 | ハンドラ内bare raiseは同一例外を外側へ再送出し、ハンドラ外bare raiseは`RuntimeError`となる | stale active exception、例外フレーム名衝突 |
| C200–C203 | 文字列case変換、後方検索、prefix/suffix除去が期待値を返す | ASCII変換、逆方向走査、部分文字列境界 |
| C204–C207 | `for first, *middle, last in iterable`が先頭・可変残余・末尾を正しく束縛し、各反復で残余listを独立して構築する | 反復長判定、残余境界、入れ子ループの一時変数衝突 |
| C208–C211 | `print`が`sep`/`end`の指定値および`None`の既定化を守り、非文字列指定に`TypeError`を送出する | 区切り・終端の欠落、None既定値、型検査の欠落 |
| C212–C215 | `str.index`と`str.rindex`が先頭・末尾側の一致位置を返し、未検出時は`ValueError`を送出する | findとの例外契約混同、後方探索の境界 |
| C216–C220 | `str.replace(old, new, count)`が0・負・正の回数制限と空文字列の挿入位置をCPythonと同じく扱う | 回数制限無視、空文字列置換の境界欠落 |
| C221–C224 | `list.index(value, start, stop)`が正負の開始・終了境界を正規化し、範囲外の一致を無視し未検出時に`ValueError`を送出する | 負境界のクランプ、stop境界、検索範囲漏れ |
| C225–C234 | `str.find`/`index`/`rfind`/`rindex`/`count`がstart/stopの正負境界、空部分文字列、未検出時の例外契約を守る | 範囲外検索、空文字列位置、ValueError欠落 |
| C235–C240 | `str.split(sep, maxsplit)`が明示区切り・空白区切りで0・負・正の回数制限を守る | 残余文字列、連続空白、回数制限無視 |
| C241–C250 | `strip(chars)`、`startswith`/`endswith`のstart/stop、文字列タプルが正しい範囲・型契約を守る | 片側strip、範囲のずれ、prefix/suffixタプルの誤判定 |
| C251–C256 | `partition`/`rpartition`が最初/最後の区切り、未検出時の3要素tupleを返す | 区切り位置、未検出tuple、後方探索 |
| C257–C268 | ASCII文字種判定、identifier、`splitlines(keepends)`、制御文字のリテラル・reprがCPythonと一致する | 空文字列、改行境界、C文字列エスケープ、repr制御文字漏れ |
| C269–C274 | `expandtabs`が既定幅、列位置、改行後の列リセット、0/負幅を正しく扱う | タブ幅、列カウンタ、改行リセット |
| C275–C282 | `dict.fromkeys`が順序・共有値・既定Noneを守り、`dict.update`がpair iterableの後勝ち更新と不正pairのValueErrorを守る。setの`issubset`、`issuperset`、`pop`が値・残余集合・空集合KeyErrorを守る | 重複キー順、共有mutable値、pair arity、包含関係、削除後の集合不変条件 |
| C283–C284 | `next()`でgeneratorが順次`yield`値を返し、for文の汎用反復経路が同じgeneratorをStopIterationまで反復する | 再開状態の消失、forのシーケンス依存、StopIteration捕捉漏れ |
| C285–C287 | `async def`、子コルーチンの`await`、`asyncio.run`が親の完了値をFIFO協調実行で返す | await結果の喪失、待機子の再実行、実行器終了値の誤り |
| C288–C299 | UTF-8単一Unicodeスカラー値の`ord`/`chr`、負値を含む`bin`/`oct`/`hex`、開始値を持つ`sum(iterable, start)`がCPython出力と一致する | UTF-8幅・符号位置・接頭辞・十六進小文字・開始値の欠落 |
| C300–C302 | `/`で区切られた位置専用引数が位置呼出し・既定値・後続keyword-only引数を守り、位置専用名のキーワード指定を`TypeError`にする | keyword再束縛、既定値順序、例外欠落 |
| C303 | `yield from`がlistとtupleの各値を順に委譲し、委譲完了後の通常yieldを継続し、`list(generator)`のmaterializationでも値を失わない | 状態再開、delegate iteratorのGC到達性、StopIteration捕捉、iterator収集 |
| C304 | `raise RuntimeError(...) from ValueError(...)`が外側ハンドラへ伝播し、生成Cがcause設定APIを通る | causeの脱落、GC未走査、例外フレーム破損 |
| C305 | `[1, middle, *tail, 9] as original`が固定要素・中間残余・as subjectを正しく束縛する | subject再評価、star境界、as捕捉順 |
| C306 | mappingのliteral keyとcaptureが挿入順dictから正しく抽出される | key存在確認、値取得、capture漏れ |
| C307 | literal OR patternが最初の成功caseを選ぶ | OR枝の評価順、複数case実行 |
| C308 | 非sequence subjectがsequence patternを通過しない | 型判定の欠落、誤った添字アクセス |
| C309 | guardがcapture後かつ成功patternだけに対して評価される | guard先行評価、失敗caseの束縛 |
| C310 | `{"x": x, **rest}`が要求済みキーを除く残余dictを返す | キー除外、残余値欠落、入力dict変異 |
| C311 | `{"x": x, **rest}`の残余が空dictになり得る | 空残余、ゼロ要素dict生成 |
| C312 | 指定キー不在のmappingがfallback caseへ進む | 非送出lookup、case停止 |
| C313 | `{**rest}`がdict全体の独立した残余dictを返す | key_count=0経路、順序・aliasing |
| C314 | 非mapping subjectが`{**rest}`を通過しない | 型判定欠落、誤capture |
| C315 | `Point(x=0, y=value)`がcustom class型とkeyword属性を照合する | 型判定漏れ、属性値取得不良 |
| C316 | class属性にnested sequence/star patternを適用する | 属性subjectの誤評価、star境界 |
| C317 | class pattern成功後のas captureとguardが正しい順序で動く | capture前guard、subject再評価 |
| C318 | 同型で値条件が外れたinstanceが後続bare class caseへ進む | 失敗後のcase停止、属性pattern誤判定 |
| C319 | 非instance subjectがcustom class patternを通過しない | 無条件一致、instance判定欠落 |
| C320 | keyword属性が不在のinstanceは送出せず後続caseへ進む | AttributeError送出、fallback欠落 |
| C321 | `int()`と`str()`が対応組込み型を照合する | object tag型名分派不良 |
| C322 | `object()`が任意の対応objectを照合する | 最上位型照合の欠落 |
| C335–C339 | generator expressionが反復子状態、単一for、filterを保持する | 生成時評価、再開状態、filter逸脱 |
| C340 | `(await value(3)) == 3`と`(await value(1)) and 7`が中断再開後もCPythonと同じ値を返す | await探索漏れ、短絡値喪失 |
| C341 | async forが`__aiter__`、`__anext__`、`StopAsyncIteration`、elseを正しい順序で処理する | await状態、終了例外、else遷移 |
| C342 | async forの`continue`が次のanextへ戻り、`break`がelseを抑止する | ループ遷移、breakフラグ、else誤実行 |
| C343–C346 | 外側変数を読むclosureと`nonlocal`で更新するclosureが同じcellを共有する | 値コピー、再束縛不可視、cell GC漏れ |
| C347 | lambdaが外側関数引数をclosure環境から取得する | GNU依存、capture漏れ、引数束縛破損 |
| C348 | `list(scale * item + 1 for item in range(4) if item >= 1)`が外側`scale`を生成時に保持し、Python 3.13の唯一の関数引数generator expression構文を受理する | free variable喪失、遅延評価時のdangling参照、`for`の構文受理漏れ |
| C349 | `sum(x*x for x in range(10) if x % 2 == 0)`を唯一の関数引数として安全に解析する | parser token UAF、解放済み位置情報の参照 |
| C350–C353 | `sorted`/`min`/`max`の`key=`・`reverse=`と未知keyword拒否 | key無視、降順条件誤り、未知引数のサイレント無視 |
| C354 | `raise RuntimeError(...) from e`後に`outer.__cause__`を参照する | cause属性欠落、例外オブジェクトのGC不整合 |
| C355 | `with (A() as a, B() as b,)`を受理して左enter・右exitする | 括弧内改行/末尾カンマのparse誤り、cleanup順逆転 |
| C356–C357 | `with Scope() as (a, b)`および`as [a, b]`を束縛する | 複合targetの未束縛、生成C左辺誤変換 |
| C358 | `async with Guard() as value`が`__aenter__`、body、`__aexit__`を順に待機する | generator local喪失、await state遷移誤り |
| C359 | truthyな`__aexit__`がbodyの例外を抑止し、後続文を実行する | 例外フレーム復帰漏れ、抑止値の無視 |
| C360 | falseな`__aexit__`の後も`__aexit__`へ元例外を渡す | 元例外の消失、exit未実行 |
| C361 | falseな`__aexit__`の後、元例外を外側`try/except`へ再送出する | 再送出漏れ、例外型/メッセージの不一致 |
| C362 | class内async protocol methodが内部`await`を実行してから結果を返す | class methodの同期誤lowering、awaitの黙殺 |
| C363 | `{x % 3 for x in range(10)}`が重複を除いたsetを構築する | `p2c_set_add`漏れ、重複排除破損 |
| C364 | set comprehensionの`if` filterが各要素を正しく選別する | filterの未評価、条件逆転 |
| C365 | 複数`for`を持つset comprehensionがnested loopの評価順を維持する | inner iterable評価・nested loop・target束縛の破損 |
| C366–C369 | 複数decorator expressionを上から下へ一度ずつ評価し、innerからouterへ適用する。default引数を持つdecorated functionは位置呼出しで正しい値を返す | decorator評価・適用順逆転、callable再束縛漏れ、adapterのdefault補完破損 |
| C370 | bare-name decoratorが通常関数をP2C callable adapterとして受け取り、戻された関数を再束縛する | direct C function pointerの誤用、未定義adapter、GC root漏れ |
| C371 | decorated async functionがadapter経由でcoroutineを返し、`asyncio.run`で完了値を返す | async state machineの同期化、awaitable喪失 |
| C372 | decorated classがclass objectを再束縛し、constructor・method呼出しを維持する | class object再束縛漏れ、constructor call分派破損 |
| C373–C377 | `async with A() as a, B() as b:`がA/Bをこの順にenterし、body後にB/Aを逆順exitする | item state番号衝突、enter/exit順の反転、`as` value喪失 |
| C378–C383 | body例外時にB/Aを逆順exitし、Aのtruthy結果が例外を抑止して後続文を実行する | 最初のexitへの例外情報未渡し、cleanup中断、抑止結果無視 |
| C384–C402 | `try`本体・`except`節・`else`節から`return`/`break`/`continue`で脱出するとき`finally`を内側から外側へ実行し、例外フレームを復元してから脱出する。戻り値式は`finally`より先に評価し、for/whileの`else`やモジュールレベルループ、入れ子`def`、メソッドでも同じ順序を保つ | finallyの取りこぼし、保留中例外の誤破棄と死んだ例外フレーム参照、戻り値評価順の逆転、脱出範囲の取り違え（ループ外の`try`再実行） |
| C403–C428 | `divmod`（int/float・両符号・タプル分解）、3引数`pow`（正/零/負指数・負の底と法・大きい法）、`format`（`<`/`>`/`,`/`%`を含む指定）、`callable`（関数・lambda・クラス・`__call__`付きインスタンス・値・ビルトイン名）、`str.isascii`/`str.isprintable` | 変換は受理するが未定義のC関数呼び出しを生成する（`divmod`/`p2c_user_pow`）、床除算・剰余の符号規則、負指数の逆元、`format`仕様の取りこぼし |
| C429–C443 | `...`（Ellipsis）と`Ellipsis`：単一値の同一性、`repr`、`type()`、真偽、コンテナ/dict値、`def f(): ...`のスタブ本体、`-> ...`注釈 | 式文が`-Wunused-value`になる、単一値がNone扱いになる、`Ellipsis`名の未解決 |
| C444–C460 | 文字列エスケープ：8進`\ooo`、16進`\xHH`、`\uXXXX`/`\UXXXXXXXX`（UTF-8化）、行継続、未知エスケープの保持、raw文字列、f-string内のエスケープとエスケープされた波括弧、空白を含む識別情報 | バックスラッシュの消失（`"caf\u00e9"`→`"cafu00e9"`）、サロゲートや範囲外コードポイントの扱い、f-stringと通常文字列での挙動差 |
| C461–C479 | ネストしたクラス定義（`class Outer: class Inner:`）：`Outer.Inner`経由の構築とメソッド呼出し、クラス本体での`alias = Inner`束縛と`[First, Second]`内での参照、クラス本体の文の上から下への評価順、ネストクラスの継承と明示的基底呼出し、`isinstance(x, Outer.Inner)`、同じ単純名を持つ別々の外側クラスの衝突回避（`Left.Node`と`Right.Node`）、二段ネスト（`Deep.Mid.Core`）、インスタンス経由の`self.Inner`、ネストクラス版`__str__` | C名の衝突（`Outer__Inner`前置漏れ）、クラス本体での名前解決漏れ、メソッド本体からのクラススコープ参照（PythonではNameError）、外側クラスへの属性登録漏れ、`__classobj()`の再帰初期化漏れ |

| C480–C486 | Cのライブラリ名と衝突しうる名前をパラメータ・ローカル・モジュール関数名に使う回帰（`index`/`round`/`abs`/`pow`/`sqrt`/`log`/`main`、クラスメソッドの引数、キーワード呼出し） | パラメータ宣言・`(void)`キャスト・本体参照・前方宣言でマングル名が食い違い、生成Cが「undeclared identifier」でコンパイル不能になる（クラスメソッドの`(void)`キャストだけ生名だった） |
| C487–C493 | 多重継承のC3線形化MRO（ダイヤモンド継承で、サブクラスより先に兄弟基底のメソッドが解決されること）、isinstance の全基底判定、基底クラスのクラス属性の継承、基底 __init__ の継承、明示的な基底メソッド呼出し | 基底探索順（従来の深さ優先近似ではCPythonと異なるメソッドを選んでいた）、MROキャッシュ、クラス属性探索の欠落 |
| C494–C499 | 束縛メソッド（m = obj.method）の取り出し・状態変更・複数回呼出し、hasattr、map/filter/コールバックへの受け渡し | メソッド属性の解決漏れ、map/filter が関数ポインタを直接呼びクロージャを黙ってスキップしていた不具合 |
| C500–C505 | finally内の return/break/continue（finally本体の実行、進行中returnの上書き、例外の抑制、入れ子finally、ループ脱出） | 実行中のfinallyの再実行、例外フレームの復元漏れ |
| C506–C515 | ジェネレータ式の複数for節（〜3節の積）、タプルターゲット、ifフィルタ、外側ローカルcapture、最外iterableの即時評価、sortedへの受け渡し | 1節のみの制限、ターゲット束縛漏れ、レベル変数（state machine）の誤再開 |
| C516–C525 | タプル・リストの辞書式比較（sorted/min/max）、安定性（key=同値の順序保持）、reverse=、混在キー | タプルを整数として比較していたことによる未ソート出力、挿入ソートのO(n^2) |

> C175–C486は通常のCPython出力差分に加え、対応する生成Cを警告即エラーの条件でコンパイルする。`match/case`、bare raise、dictマージ、文字列メソッド、for starred unpack、`print`キーワード引数、async for、async with、closure、ネストしたクラス定義はstrict ISO C11経路または単一ヘッダー契約でも確認する。単一ヘッダーはClangだけでも`-pedantic-errors`で自己完結ビルドを確認し、freestanding構成ではHosted allocator・出力・時刻シンボルの不在を検査する。ネストしたクラス定義とC識別子衝突の回帰は`examples/embed/embed_boot.py`（H04）と`examples/baremetal/baremetal_hello.py`（`make test-baremetal-generated`）でも実際に変換され、カーネル側ヒープだけを与えた組込み構成でCPython差分を取る。

### 自作OS統合ゲート（H01–H06）

CPython差分とは別に、組込み統合の契約を実行時検証する。

| ID | コマンド | 検証内容 |
|---|---|---|
| H01 | `make test-embed-runtime` | 組込みヒープ（整列・分割・隣接合体・realloc移動・破損検出・二重解放耐性）、ライフサイクル、タスク再起動、スタック境界未登録時の収集停止と登録後の有効化、OOM通知、MemoryError化、panic経路 |
| H02 | `make test-freestanding-setjmp` | カーネル提供setjmp/longjmpでのraise/except、`except as`の束縛読み出し、深いフレームからのlongjmp、入れ子フレームと再送出 |
| H03 | `make test-embed-compile` | カーネル相当環境での変換器コア実行、`--embed-entry`相当APIの生成物、不正エントリ名の拒否 |
| H04 | `make test-embed-generated` | `--embed-entry`生成モジュールを、静的ヒープ・UARTシンク・スタック境界・panicフックだけを与えて実行し、CPythonと出力差分比較 |
| H05 | `make test-hobby-os-template` | 組み込みテンプレート（推奨構成・フルコントロール構成・Makefile断片）の警告即エラー・コンパイルとライブラリ化 |
| H06 | `make test-crlf` | LF/CRLF/CRの各ソースから生成されるCが完全一致し、classのメソッドがクラス所属として生成されること |
| H07 | `make test-allocator-injection` | カーネルアロケータの1回注入（`p2c_platform_set_allocator`）と`P2C_Allocator`注入（`p2c_set_default_allocator`）が変換器コア・共有ヒープの両方へ効くこと、注入が失敗する場合のみ静的フォールバックを使い`p2c_core_static_allocator_active()`で観測できること、停止順序（ランタイム停止→プラットフォーム解除）でヒープが壊れないこと |
| H08 | `make test-setjmp-hook` | ランタイム本体と生成Cの例外機構が`P2C_SETJMP`/`P2C_LONGJMP`だけを通ること（オーバーライドしたフックの呼び出し回数で検証） |
| H09 | `make test-heap-unification` | NO_STDLIB構成（ランタイム同梱スタブ）で、スタブの`malloc/realloc/calloc/free`がカーネルアロケータへ委譲されること、`p2c_heap_alloc()`のブロックとraw mallocのブロックが相互に解放できること、プラットフォーム未設定時はスタブの線形ヒープが唯一のヒープになること、ヒープ未設定では確保がNULLを返すこと |
| C526–C528 | super()のMRO解決：ダイヤモンド継承（class D(B, C)、B(A)、C(A)）でBのsuper()が実体の型のMROに従ってCを選ぶこと、単一継承でのsuper()、super().__init__()による基底初期化 | コード生成時に基底チェーンを静的に辿っていたことによる誤解決 |
| C529–C538 | メソッドデコレータ：@staticmethod（クラス経由・インスタンス経由の呼び出し）、@classmethod（cls を受け取りクラス属性を更新／生成）、@property（読み出しでゲッター実行）、propertyのMRO継承とオーバーライド、propertyへの代入がAttributeErrorになること、束縛したpropertyの取り出し | メソッド種別のディスパッチ漏れ、propertyの優先順位（インスタンス属性より先）、setter未実装時の属性隠蔽 |
| C539–C549 | math モジュール拡充：floor/ceil/trunc/fabs/fmod/hypot/copysign/ldexp/degrees/radians/log/log2/log10/log(2引数)/exp/expm1/log1p/tan/isnan/isinf/isfinite/fsum/prod/factorial/gcd/isqrt/comb/perm/cbrt/erf/gamma | freestandingでのlibm宣言漏れ、判定マクロ依存（isnan/isinf）、整数オーバーフロー検査 |
| C550–C555 | @property の setter（`@x.setter`）：プロパティへの代入で setter が呼ばれること、setter 無しのプロパティへの代入が AttributeError になること、継承したプロパティの setter が動くこと、setter が副作用（trace リスト等）を残すこと | ゲッターと setter の C シンボル衝突、名前引きで setter がゲッターを隠すこと |
| C556–C576 | 文字列の `%` 書式（剰余演算子）：`"%s(%.2f)" % (name, area)`、`%d`/`%i`/`%u`/`%x`/`%X`/`%o`、フラグ（`-` `+` 空白 `0`）と幅・精度、`*`（幅と精度を引数から取得）、`%r`/`%a`/`%c`、`%%`、タプル右辺の位置引数、`%(key)` 辞書マッピング、非タプルの単一値、`str % int` の TypeError（not all arguments converted）、引数不足の TypeError、辞書キー欠落の KeyError、`%(key)` に対する非マッピングの TypeError | 左辺が文字列でも数値の剰余として扱われていたこと（`ZeroDivisionError`）、`p2c_obj_as_str` が文字列専用で数値・コンテナを空文字にしていたこと |
| C577–C588 | 実践的な複合プログラム：継承（`super()`連鎖・`@property`・`__init__`オーバーライド）＋`round`／コレクション内包（`if`付きdict内包）／`sorted(key=len, reverse=)`／`dict.get`集計／`zip`／クロージャ／`try/except/finally`／`enumerate`付きジェネレータ式／`join`／`%`書式／`format`／f-string | 3段継承で `super().__init__(a, b)` が基底の `__init__` を呼ばず、祖先のメソッドが `AttributeError` になっていたこと |
| C589–C593 | クラス名の隠蔽規則：ランタイム／モジュール側の同名クラス（pygame の `Rect`/`Surface`/`Sprite`/`Group`/`Clock`）より**後から定義されたユーザークラスが優先**されること。3段継承で各段の `__init__` が順に走り、`super().__init__(a, b)` が基底のアダプタへ正しく渡り、祖先のメソッドが解決できること | クラス名レジストリの名前引きが**先着**を返していたため、ユーザーの `class Rect(Shape)` ではなく pygame の `Rect`（基底なし・無言の `__init__`）が継承元として解決され、MROが `[Rect]` で打ち切られていたこと |
| H10 | make test-stack-usage | freestandingランタイムと共通層のすべてのフレームが STACK_USAGE_LIMIT（既定4096バイト）以下であること（-Wstack-usage と -Werror の併用） |
| H11 | make test-analyzer | GCC -fanalyzer でコンパイラコアとランタイムの欠陥（解放後利用・NULL経路・確保失敗の取り違え）がないこと。実行時間が長いため任意実行 |

> H01–H09は`make test`に含まれる。テンプレート・例・回帰テストは実装と同じ警告基準
> （`-Werror`、`-Wconversion`、`-Wcast-qual`、`-Wvla`、`-Wfloat-equal`等）でビルドされる。
