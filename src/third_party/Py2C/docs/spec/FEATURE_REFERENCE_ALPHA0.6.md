# Python Code to C Alpha0.6 — 全機能リファレンス

## 1. 目的と構成

**Python Code to C Alpha0.6** は、Python 3の実用的サブセットをCコードへ変換するコンパイラである。生成Cは同梱ランタイムをリンクして実行する。構成は、開発ホスト用のCLI、ホスト用ローカルGUI、標準Cライブラリを必要としないfreestandingコンパイラコアの三つに分かれる。

| 構成 | 目的 | ビルド成果物 |
|---|---|---|
| Hosted CLI | PythonソースをCへ変換し、開発ホストで利用する | `build/python-code-to-c` |
| Hosted GUI | OS依存の描画を抽象化したローカルGUIフロントエンド | `build/python-code-to-c-gui` |
| Freestanding core | 自作OSへ組み込むための変換器コア | `build/freestanding/libpython-code-to-c-core.a` |

## 2. CLI

| 操作 | 用途 |
|---|---|
| `python-code-to-c input.py -o output.c` | PythonをCへ変換する |
| `python-code-to-c run input.py` | 変換・ホストCコンパイル・実行をまとめて行う |
| `python-code-to-c --dump-ast input.py` | ASTをデバッグ表示する |
| `python-code-to-c --supported` | 実装側が公開する対応状況を表示する |
| `python-code-to-c --self-test` | 組込み自己診断を行う |
| `-v` / `--verbose` | 字句解析、構文解析、意味解析、コード生成の統計を標準エラー出力へ出す |
| `--comments` | 生成Cへ対応Python行のコメントを埋め込む |
| `--c11` | GNU拡張を使う生成を拒否し、ISO C11対象だけを明示的に受理する |
| `--embed-entry NAME` | `int main()` の代わりにカーネルから呼べる `P2C_Object *NAME(void)` を生成する（自作OS組み込み用。名前はC識別子のみ受理） |
| `--fallback` | 未対応構文を、実行時に `NotImplementedError` を送出するスタブへ置き換えて変換を続行する（到達しなければそのまま動く）。既定は位置付きの明確な診断で停止 |

通常のHosted生成Cは、`-I./include`、`src/runtime/python_code_to_c_runtime.c`、`src/common/python_code_to_c_common.c`、`src/platform/python_code_to_c_platform.c`、Hostedプラットフォーム実装、必要ならGUI・pygameモジュールとリンクする。`make run INPUT=path/to/file.py`はこの変換・コンパイル・実行を一括化し、`make check-tools`と`make help`はツールチェーン確認と起動ターゲット一覧を提供する。詳細な手順は [`../build/BUILD_AND_LAUNCH_ALPHA0.6.md`](../build/BUILD_AND_LAUNCH_ALPHA0.6.md) を参照する。

## 3. Python言語機能

### 3.1 値、演算子、式

| 分類 | 対応する機能 |
|---|---|
| 基本値 | `None`、`bool`、64ビット符号付き整数、浮動小数点、文字列。整数の範囲外は安全に`OverflowError` |
| 算術 | `+`、`-`、`*`、`/`、`//`、`%`、`**`、単項`+`、単項`-` |
| 比較 | `<`、`<=`、`==`、`!=`、`>`、`>=`、`is`、`is not`、`in`、`not in`、比較連鎖の基本経路 |
| 論理 | `not`、`and`、`or`。Pythonと同様に短絡し、boolではなくオペランド値を返す |
| ビット・集合・辞書演算 | 整数の`&`、`|`、`^`、`~`、`<<`、`>>`。set同士の`&`、`|`、`-`、`^`と拡張代入。dict同士の`|`と`|=`は右優先・挿入順保持でマージ |
| 条件式 | `value_if_true if condition else value_if_false` |
| 代入式 | `name := expression`。nameだけをターゲットにし、if、while、内包表記、入れ子式で値を返しながら束縛する |
| 書式化 | 基本f-string、書式指定の主要部分、`!r`/`!s`変換、`#`代替形式、符号フラグ、`str.format()`。f-string補間と二項式は左から右へ評価 |
| 呼出し | 通常呼出し、検証済み組込みのキーワード引数、`sorted`/`min`/`max`の`key=` callableと`reverse=`、位置専用引数の`/`区切り、`*args`、`**kwargs`アンパック、属性メソッド呼出し、`print`の`sep`/`end`キーワード引数。未知keywordはサイレントに無視せず診断し、位置専用引数をキーワードで渡すと`TypeError` |
| 単一値`...` | `...`（Ellipsis）とビルトイン名`Ellipsis`。`def f(): ...`のスタブ本体、`is`/`==`、`repr`、`type()`（`<class 'ellipsis'>`）、コンテナ要素として動作 |
| 追加組込み | `divmod(a, b)`（intはfloor除算・floatはCPythonのfloat_divmod補正、タプルを返す）、`pow(base, exp, mod)`（モジュラべき乗。負の指数は法における逆元を要求し、存在しなければ`ValueError`、法0は`ValueError`、結果の符号は法に従う）、`format(value, spec)`、`callable(obj)`、`str.isascii()`、`str.isprintable()` |
| 文字列エスケープ | `\n \t \r \v \f \b \a \\ \" \'`、8進`\ooo`、16進`\xHH`、`\uXXXX`/`\UXXXXXXXX`（UTF-8バイト列へ変換）、行継続`\`+改行。未知のエスケープはバックスラッシュごと保持し、`\N{...}`はUnicode名前表を持たないため明示診断する |

### 3.2 コンテナ、添字、内包表記

| コンテナ | 対応機能 | 重要な境界 |
|---|---|---|
| list | リテラル、連結、添字、slice、長さ、反復、`append`、`pop`、`insert`、`remove`、`count`、`index(value, start, stop)`、`extend`、`clear`、`reverse`、`sort`、`copy`、内包表記、`list()`、`list(iterable)` | `list(list)`は独立した浅いコピーを返す。`index`の負境界と開始・終了範囲はPythonの規則で正規化する。内包表記は通常生成でGNU statement expressionを使う |
| tuple | リテラル、添字、長さ、反復、`tuple()`、`tuple(iterable)`、アンパック | 1要素tupleの表示を含む |
| dict | リテラル、添字、`get`、`keys`、`values`、`items`、mappingまたはkey/value pair iterableを受ける`update`、`pop`、`popitem`、`setdefault`、`clear`、`copy`、`fromkeys(iterable, value=None)`、`|`、`|=`、長さ、包含、`**mapping`unpack、**dict内包表記**、`dict()`、`dict(iterable)`、`dict(key=value)`、`dict(**mapping)`、値等値比較 | 挿入順を保持し、更新・削除後・clear後も順序リンクを維持する。`fromkeys`は重複キーを除去して共通value参照を保持する。list/dict/setとそれらを含むtupleはキーとして拒否し、等しいtupleは同一キーとして扱う |
| set | リテラル、`set()`、`set(iterable)`、長さ、包含、反復、`type`、`isinstance`、**set内包表記**、`add`、`discard`、`remove`、`pop`、`clear`、`copy`、`union`、`intersection`、`difference`、`symmetric_difference`、各update系、`isdisjoint`、`issubset`、`issuperset`、集合演算 | set内包表記は単純name target、任意数の同期`for`・filter・重複排除に対応する。式位置container構築は通常生成でGNU statement expressionを使うため`--c11`では明示診断する。`set(set)`と`copy()`は独立した浅いコピーを返す。`pop`の選択順はPython仕様上依存してはならない |
| frozenset | `frozenset()`、`frozenset(iterable)` | 可変setへの互換フォールバック。不変性・ハッシュ可能性・固有型名は非対応 |

`a, *middle, z = sequence` のstarred unpack代入に加え、`for first, *middle, last in iterable:` のforターゲットにもstarred unpackを使える。各反復では最小要素数を厳密に検証し、残余を新しいlistへ束縛する。入れ子のforでも生成Cの反復・残余用一時変数は衝突しない。list・tuple・strの負添字、境界外添字、正負stepのsliceを扱い、添字とslice境界にはint/bool以外を渡せない。属性・添字を含む複合代入は、左辺の受け手と添字を一度だけ評価する。`del obj.attr`、`del mapping[key]`、`del values[index]`も対応する。

### 3.3 文、制御、例外

| 分類 | 対応する機能 |
|---|---|
| 分岐・ループ | `if` / `elif` / `else`、`while`、`for`、`break`、`continue`、for/whileの`else`。forターゲットでは通常のアンパックとstarred unpackを受理する。forは`iter()` / `next()` / `StopIteration`の汎用反復経路を使うため、list等のシーケンスに加えて本実装のジェネレータも反復できる。`match/case`はliteral、`None`、capture、`_`、OR、sequence、mapping、star、as、class、`if` guard、mapping `**rest`を受理し、subjectを一度だけ評価 |
| 例外 | `try` / `except` / `else` / `finally`、値付き`raise`、`raise exc from cause`の明示cause連鎖、`exception.__cause__`参照、ハンドラ内bare `raise`、`assert`、主要な組込み例外型。cause参照はGC走査対象 |
| with | `__enter__`、`__exit__`、例外抑止、`as`束縛、**複数コンテキスト**、括弧付きwithとtuple/list `as` target。`with A(), B():`は意味的に入れ子のwithへ展開され、enterは左から、exitは右から実行される |
| async with | 単一context manager、optional simple-name `as` target、`__aenter__`/`__aexit__`の待機、例外抑止、元例外の再送出。contextとpending exceptionはgenerator localsへ保存しGC対象にする |
| 制御保護 | `try`本体・`except`節・`else`節から脱出する`return`/`break`/`continue`は、脱出前に`finally`本体を内側から外側へ実行し、例外フレームを復元したうえで脱出する（Pythonの「保留中例外は破棄され、finallyが先に走る」規則に一致）。`finally`内の`return`、`break`、`continue`は、finally本体を実行したうえで保留中の例外・`return`を上書きする（Pythonと同じ） |
| 名前空間 | モジュール代入、`global`、ネスト`def`の読取りcapture、`nonlocal`による共有cell再束縛。外側関数がcellを所有し、同一スコープから生成した複数closureは同じ値を参照・更新する |
| 型注釈・型構文 | 変数、引数、戻り値の注釈、Python 3.13の型パラメータリスト（既定値、`*Ts`、`**P`を含む）、soft keywordの`type Alias[params] = expression`を構文として受理し、freestanding互換の型消去モードでは実行時に無視 |
| import | `import`、`from ... import`の基本経路、登録済みモジュールのランタイム解決 |

### 3.3.1 構造的パターンマッチング

`match`のsubjectは一度だけ評価され、caseは上から順に試行される。patternが成功してからguardを評価し、guardが偽なら次のcaseへ進む。listおよびtupleには`[first, *middle, last]`、`(first, *middle, last)`のsequence patternを使用でき、star captureには常に新しいlistが束縛される。mappingには`{"key": value}`、`{"key": value, **rest}`、`{**rest}`を使用できる。`**rest`は要求済みのキーを除いた**独立した新しいdict**であり、入力dictの挿入順を保存する。`pattern as whole`はpattern成功後にsubjectそのものを束縛し、`a | b`は左から順に最初に成功した代替patternを採用する。

| 対応pattern | 例 | 実行時の契約 |
|---|---|---|
| 値、capture、wildcard | `0`、`name`、`_` | 値は等価比較、captureは成功時に束縛、`_`は束縛しない |
| sequence / star | `[1, item, *tail]` | listまたはtupleだけを対象にし、固定部分の最小長または完全長を検証 |
| mapping / 残余mapping | `{"kind": tag, **rest}` | dictだけを対象にし、指定キーの存在を非送出で確認。`rest`は指定キーを除く新規dict |
| as / OR / guard | `[x, y] as pair`、`1 | 2 if ok` | pattern成功後にas捕捉、ORは最初の成功枝、guardは全捕捉後に一度だけ評価 |
| class / keyword属性 | `Point()`、`Point(x=0, y=value)`、`int()` | 型名に一致するinstanceまたは組込み型を照合し、属性不在は送出せずcase失敗として次のcaseへ進む |
| class positional | `Point(x, y)`、`Point(x, y=0)`、`int(value)` | 自作classはclass属性`__match_args__`のstr tupleで位置を属性名へ対応付け、組込み型は一要素でsubject自身を照合する |
| dotted value / class | `Palette.RED`、`Registry.types.Point(x, y=0)` | 属性連鎖を各case試行で一度だけ評価し、値patternは等価比較、class patternは評価済みclass objectに対する継承照合を行う |

`__match_args__`を使わないclass positional推論、任意の`collections.abc.Mapping`実装、複数star pattern、OR枝間で異なるcapture集合を持つpatternは、現段階では完全な意味論の対象外である。これらは移植性優先の互換フォールバックと明示診断の対象とし、生成Cの誤動作を許容しない。

### 3.4 関数とクラス

| 分類 | 対応する機能 |
|---|---|
| 関数 | `def`、Python 3.13型パラメータ、return、デフォルト引数、位置専用引数`/`、キーワード引数、`*args`、`**kwargs`、キーワード専用引数の基本経路。**module-level decorator**は上から下への式評価、下から上への適用、callable objectへの再束縛を行い、同期・async functionを対象にする |
| lambda | 基本lambda式と関数ローカル自由変数capture。C11のclosure objectと環境辞書で実装し、lambda自体はGNUネスト関数に依存しない |
| クラス | 型パラメータリストを構文受理する単一継承、コンストラクタ、インスタンス属性、通常メソッド、メソッドのデフォルト引数。**module-level class decorator**はclass objectを同じcallable再束縛経路で置換する。class内async protocol methodが実際のawait/async制御を含む場合は専用state machineへlowerする。クラス本体の直下に置いたclass（**ネストしたクラス定義**）はC名を外側クラス名で前置して衝突を避け（`Outer__Inner`）、外側クラスの`__classobj()`が上から下への評価順で属性として登録するため、外部からは`Outer.Inner`で参照・構築できる。クラス本体の式からはその名前で直接参照でき、同じ単純名を持つ別々の外側クラスや二段以上のネストも扱う。Python仕様どおりクラススコープの名前はメソッド本体から見えない（CPythonでは`NameError`）ため、その位置での参照は黙って壊れたCを出さず明示診断する。関数本体の中のclass定義はCの入れ子関数定義になり得ないため未対応として診断する |
| 特殊メソッド | `__init__`、`__str__`、`__repr__`、`__eq__`、`__len__`、`__getitem__`、`__iter__` / `__next__`のランタイム経路 |
| ジェネレータ | 順次トップレベル文からなる`def`本体の`yield [value]`と反復値委譲`yield from iterable`、および単一`for name in iterable`と任意filterを持つgenerator expression `(expression for name in iterable if condition)`。関数ローカルgenerator expressionは外側のローカル値を生成時にGC走査対象のgenerator localsへcaptureし、`list(expr for target in iterable)`のように唯一の関数引数では外側括弧を省略できる。generator式の唯一引数経路はtoken位置を消費前に値コピーし、ASan/UBSanで監査する。左端iterableは生成時に一度だけ評価し、target・iterator・再開位置もgenerator localsへ保存される |
| 協調async | `async def`、`await coroutine_call()`、算術・単項・単一比較・二項`and`/`or`を含む代入右辺の単一await、`async for`、`async with`、`asyncio.run(coroutine)`。固定長FIFOキューで子コルーチンを先に進める協調実行モデル。async forは`__aiter__`、`__anext__`待機、`StopAsyncIteration`、body/else、`break`、`continue`を状態機械化する。async withは`__aenter__`/`__aexit__`待機と例外抑止を状態機械化する |
| 属性API | `hasattr`、`getattr`、デフォルト値付き`getattr`、`setattr`、`delattr` |

## 4. 組込み関数とランタイム

主要な組込み関数として、`bool`、`int`、`float`、`str`、`list`、`tuple`、`dict`、`set`、`frozenset`フォールバック、`len`、`range`、`sum(iterable, start=0)`、`min`、`max`、`sorted`（`key=` callableと`reverse=`を含む）、`reversed`、`any`、`all`、`map`、`filter`、`enumerate`、`zip`、`round`、`abs`、`pow`、`divmod`、`ord`、`chr`、`bin`、`oct`、`hex`、`type`、`isinstance`、`repr`、`iter`、`next`、`input`、`print`を実装または変換経路として提供する。`ord`と`chr`はUTF-8の単一Unicodeスカラー値を扱い、`bin`/`oct`/`hex`は64ビット符号付き整数をPython互換の接頭辞・小文字・符号順序で文字列化する。`print`は通常の位置引数に加え、文字列の`sep`/`end`と、各キーワードに対する`None`指定時の既定値復帰を扱い、非文字列値には`TypeError`を送出する。文字列は`find`/`index`/`rfind`/`rindex`のstart/stop指定、`count(sub, start, stop)`、未検出時に`ValueError`を送出する`index`/`rindex`、回数上限と空検索文字列の挿入位置に対応する`replace(old, new, count)`、明示区切り・空白区切りの`split(sep, maxsplit)`、`strip(chars)`・`lstrip(chars)`・`rstrip(chars)`、start/stopと文字列tupleを受ける`startswith`/`endswith`、`partition`/`rpartition`、ASCIIの`isalpha`/`isdigit`/`isalnum`/`isspace`/`islower`/`isupper`/`isidentifier`、`splitlines(keepends)`、`expandtabs(tabsize)`を扱う。`\n`、`\r`、`\t`、`\v`、`\f`、`\b`、`\a`を含むPythonリテラルは正しいCエスケープとして生成し、コンテナreprでも制御文字をエスケープして表示する。`dict`は空構築、辞書コピー、2要素シーケンスからの構築、キーワード項目、`**mapping`更新を扱う。`enumerate`は既定の0または第2引数の開始値を扱い、`reversed(iterable)`は逆順化した反復可能なlistを返す互換経路として実装する。対応範囲は引数形式ごとに異なるため、用途を追加する際は必ずCPython差分テストを追加する。`isinstance`はクラス名だけでなくクラスオブジェクト（属性経由の`Outer.Inner`や変数に保持したclass object）も第2引数に取れる。

ランタイムは値演算、コンテナ、例外フレーム、GC追跡されるexplicit cause、属性、モジュール、GC、small-int cache、状態機械ジェネレータ、協調スケジューラ、`OBJ_CELL`、closure objectを持つ。`OBJ_ITERATOR`は既存コンテナだけでなく状態機械ジェネレータを収集型組込みへ渡せる。ジェネレータの局所辞書、待機中コルーチン、完了値、遅延伝播するcoroutine例外、cell値、closure環境はGCトラバーサルに含まれる。`p2c_async_next()`は同期的に完了する`__anext__`実装も完了済みcoroutineへ正規化し、`StopAsyncIteration`を親のasync for状態へ安全に伝播する。`p2c_async_call_attr()`はasync context protocol属性の同期結果を完了coroutineへ、同期例外を遅延exception付きcoroutineへ正規化する。GCはmark-and-sweep方式で、明示ルート、永続レジストリ、保守的スタック走査を組み合わせる。OS側の非同期キューなど、保守的スキャンで見えない場所に`P2C_Object*`を保持するときは、`p2c_gc_register_root()`と`p2c_gc_unregister_root()`を使用する。

## 5. 自作OSとGUIへの組込み

`P2C_Platform`はメモリ確保、再確保、解放、バイト列出力、時刻の接続点を抽象化する。freestandingビルドの既定プラットフォームは安全な未設定スタブであり、`python_to_c()`や生成Cの実行より前にOS側が有効なアダプタを`p2c_platform_set()`へ登録する。`p2c_platform_write()`は登録済み`write`コールバックを経由するため、Pythonの`print`と例外診断をOSコンソールへ接続できる。

GUIは固定長の`P2C_GuiCommand`配列とテキストアリーナへ描画命令を記録する。`P2C_GuiBackend`はclear、矩形塗りつぶし、矩形枠、線、文字列を、自作フレームバッファ、GPU、独自コンポジタへ接続できる。コマンド容量またはテキスト容量を超える命令は安全にドロップとして記録される。

### 5.1 統合ファサード `p2c_embed`

組込みを「静的ヒープ・1つの出力シンク・1つの時計」だけに縮める統合ファサードとして `p2c_embed`（`include/platform/python_code_to_c_embed.h`）を提供する。`p2c_embed_start()` は `P2C_Platform` の登録、`p2c_runtime_init()`、`p2c_gc_init()` とスタック境界の宣言、OOMハンドラの登録を一度に行い、`p2c_embed_run_program()` が生成モジュールのエントリを実行する。捕捉されなかった例外は診断をシンクへ出してNULLを返すため、カーネルは落ちない。

| 要素 | 内容 |
|---|---|
| 組込みヒープ `P2C_EmbedHeap` | 境界タグ + アドレス順空きリスト + 隣接合体。libc非依存で固定領域のみを使い、GCの解放を実際に再利用する。`p2c_embed_heap_check()` が整合性を検証する |
| `P2C_EMBED_PROVIDE_LIBC_HEAP` | `malloc/calloc/realloc/free` をこのヒープへ委譲する実装を提供（`PYTHON_CODE_TO_C_NO_LIBC_STUBS` と併用） |
| `P2C_EMBED_PROVIDE_PLATFORM_COMPAT` | `p2c_platform_init/shutdown/write/write_n/read_line/abort` も提供し、カーネルの追加ファイルを embed.c だけで済ませる |
| GCの安全側停止 | スタック境界が未宣言の間は回収を実行しない（`p2c_gc_stack_scan_available()` が false）。`p2c_gc_set_stack_bounds()` または `P2C_GC_ENTER_TASK()` で有効化する |
| OOM方針 | `p2c_runtime_set_oom_handler()` で通知先を登録。標準実装 `p2c_oom_raise_memory_error()` は事前確保済みの `MemoryError` を送出し、例外フレームが無ければpanicへ進む |
| ヒープ統計 | `p2c_runtime_heap_size/used/peak()`、`p2c_runtime_alloc_failures()`、`p2c_embed_stats()` で枯渇・回収・ピークをカーネルのログ/メトリクスへ出せる |
| カーネル提供setjmp | `PYTHON_CODE_TO_C_NO_LIBC_STUBS` を定義すると、ランタイムは `malloc/free/realloc/calloc` と `setjmp/longjmp` を定義せずカーネル実装を使う。`jmp_buf` は `void *buf[16]`（`runtime.h`）。既定のfreestanding構成ではコンパイラ組み込み（`__builtin_setjmp`/`__builtin_longjmp`）を使うため、libcなしでも例外が実際に機能する（`PYTHON_CODE_TO_C_NO_COMPILER_SETJMP`で無効化） |
| setjmp差し替え | ランタイム本体と生成Cの例外機構（`raise`/`except`/`with`/`for`/generator/`finally`/`async for`）は `P2C_SETJMP`/`P2C_LONGJMP` の2つのマクロだけを通る。`-DP2C_SETJMP(env)=my_setjmp(env)`／`-DP2C_LONGJMP(env,val)=my_longjmp((env),(val))` で、libc・コンパイラ組み込み・カーネル実装を完全に差し替えられる（回帰: `make test-setjmp-hook`） |
| 単一ヒープ | `p2c_platform_set()` の `alloc/realloc/free` は、生成Cのランタイムと変換器コア（文字列ビルダ・AST）の**両方**が使う（共通層の `p2c_heap_*`）。`PYTHON_CODE_TO_C_NO_STDLIB` のランタイム同梱スタブ（`malloc/calloc/realloc/free`）もこの共通層へ委譲するため、カーネルアロケータを注入した構成では raw な `malloc` 呼び出しを含めてヒープは1つになる。確保ブロックは共通ヘッダ（サイズ + magic）で管理され、`p2c_heap_free()` と raw `free()` を相互に使える。同じ領域を `p2c_runtime_init(heap, size)` と二重に使わないこと（回帰: `make test-heap-unification`） |
| 返却バッファの解放 | `python_to_c()` が返す生成C文字列は raw `malloc/free` と対になる契約（Hosted: libc、NO_STDLIB: スタブ／カーネル）。NO_STDLIB でカーネルアロケータを注入した場合、スタブの `malloc` がそのアロケータへ委譲されるため、返却バッファも同じ1つのヒープから出る |
| アロケータ注入 | `p2c_platform_set_allocator(kalloc, krealloc, kfree, ctx)` でアロケータだけを1回で注入でき（`write`/`clock_ms`は既定実装を継承）、`p2c_set_default_allocator(P2C_Allocator*)` で変換器コアの既定アロケータを明示注入できる。解除はどちらも `NULL` を渡す。停止順序は「`p2c_runtime_shutdown()`／`p2c_embed_stop()` → プラットフォーム解除」（回帰: `make test-allocator-injection`） |
| 変換器コアの静的フォールバック | 注入アロケータ／共有ヒープが確保に失敗する場合だけ静的バッファ（`P2C_COMPILER_FALLBACK_HEAP_SIZE`、既定64KiB）を使う。変換開始時の8バイトプローブで判定するため、`p2c_core_static_allocator_active()` が `false` ならOSのヒープだけを使っている。`-DP2C_COMPILER_FALLBACK_HEAP_SIZE=0` で完全無効化（未注入構成では`P2C_ERR_NOMEM`と注入方法を返す） |
| スタックスキャン範囲 | 保守的スタックスキャンは「現在のフレームから宣言上端まで」だけを走査する。走査語数は `p2c_gc_last_stack_words()` で確認でき、しきい値を小さくしても収集回数×スタック長に劣化しない |
| 式評価中の一時値 | 二項演算の左オペランド（`p2c_binop_begin()` がTLSへ積んだ値）と処理中の例外は、収集のマーク対象になる。右オペランドの評価中に自動収集が走っても、`p2c_binop_finish()` は解放済みポインタを受け取らない。式の途中で例外が脱出しても深さは漏れず、`try` の反復でネスト上限を誤発火しない。深さは `p2c_binop_depth()` / `p2c_fstr_depth()` で保存し、`p2c_binop_rewind()` / `p2c_fstr_rewind()` で戻す。ルート化した個数は `p2c_gc_last_temp_roots()` で観測できる |

変換器コア自体も `PYTHON_CODE_TO_C_NO_STDLIB` で完走する（既定アロケータが `malloc/free` へ委譲する）。これにより自作OS内でPythonソースをCへ変換するオンデバイス変換が可能である。詳細とカーネル側の契約は [`HOBBY_OS_EMBEDDING_ALPHA0.6.md`](HOBBY_OS_EMBEDDING_ALPHA0.6.md)、テンプレートは `templates/hobby_os/` を参照する。

### 5.1 単一ヘッダー配布

`include/python_code_to_c_single.h` は、公開API、コンパイラコア、ランタイム、GUI抽象化、プラットフォーム中核、組込み統合ファサード（`p2c_embed`）、Hosted互換アダプタを一ファイルへ展開した配布ヘッダーである。`OBJ_CELL`、closure object、遅延coroutine例外、async iterator API、async forおよびasync with状態機械生成、module-level decoratorのcallable adapter・GC root再束縛も含む。単一ヘッダーはUTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum`、型パラメータ既定値と`type`文の型消去受理、位置専用引数、`raise from`、`yield from`を含むジェネレータ状態機械、協調スケジューラ、walrus、値・sequence・mapping・star・as・OR・guard・bare/keyword属性・`__match_args__` positional・dotted value/class patternを含む`match/case`、mapping `**rest`用の残余dict生成API、dictマージ、for starred unpack、`print(sep=..., end=...)`、文字列探索範囲、`split(maxsplit)`、`str.index`/`rindex`、回数指定`replace`、範囲指定`list.index`、`strip(chars)`、prefix/suffix範囲、`partition`、`splitlines`、`expandtabs`、文字種判定までを外部ソースのリンクなしで変換する。`#define P2C_SINGLE_HEADER_IMPLEMENTATION` を定義して**一つの翻訳単位だけ**でincludeすると実装が出力され、他の翻訳単位では定義なしでincludeする。自作OS側は`PYTHON_CODE_TO_C_NO_STDLIB`を定義して`P2C_Platform`を登録する。`P2C_SINGLE_HEADER_NO_HOSTED`は、OS側が`p2c_platform_write`、`p2c_platform_read_line`、`p2c_platform_abort`などの互換フックも提供する場合だけ定義する。`make single-header`で再生成し、`make test-single-header`、`make test-single-header-c11`、`make test-single-header-freestanding`でHosted・厳格C11・freestandingの契約を確認する。

## 6. 生成Cの移植性

> **コンパイラ本体のC11適合性**と、**Python式を表現する生成CのC11適合性**は別の契約である。

通常のコンパイラ本体は`-std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wundef -Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla -Wfloat-equal`に加え、`-Wcast-align=strict -Wpointer-arith -Wbad-function-cast -Wnested-externs -Wjump-misses-init -Wlogical-op -Wduplicated-cond -Wduplicated-branches -Wrestrict -Wshift-overflow=2 -Wformat-overflow=2 -Wformat-truncation=2 -Wformat-signedness -Wstringop-overflow=4 -Wstringop-truncation -Warray-bounds=2 -Wmissing-declarations -Warith-conversion -Wmultistatement-macros -Wsizeof-pointer-memaccess -Wsizeof-array-argument -Wuse-after-free=3 -Wunused-macros -Wswitch-default -Wcast-function-type -Wimplicit-fallthrough=5 -Wmissing-parameter-type -Wcalloc-transposed-args -Wstrict-overflow=2`を基準としてビルドする。`-Wconversion`/`-Wsign-conversion`は暗黙の切り詰めと符号反転を、`-Wcast-qual`/`-Wwrite-strings`はconst契約の破壊を、`-Wvla`は組込み向けに可変長スタック配列を、`-Wcast-align=strict`はアラインメントを上げるキャストを検出する。`-Wswitch-enum`はASTノード種別の`switch`が`default:`節で未対応種別を診断する設計であり全`case`列挙に見合う安全性を生まないため、`-Wdeclaration-after-statement`はC11の混合宣言が本コードベースの意図した書き方であり646箇所の移動に見合う安全性を生まないため、`-Wc++-compat`はC++互換が対象外のため、`-Wnull-dereference`は単一ヘッダー構成（全ソースが1翻訳単位）で関数間解析が効くため未チェック確保と誤検出が混在した約300件を報告するため、それぞれ有効化しない。いずれも除外理由を`Makefile`の`WARN_CFLAGS`へ明記し、網羅性は`tests/`のCPython差分回帰で担保する。ホスト向け`CFLAGS`には実行時ハードニング`HOSTED_HARDEN`（`-fstack-protector-strong`、`-fstack-clash-protection`、`-D_FORTIFY_SOURCE=3`）を既定適用し、組込み構成では分離する。動的に検証済みのPython書式指定をC書式へ変換するランタイムだけは、静的解析できない動的書式文字列に対する`-Wformat-nonliteral`を明示的に無効化する。freestandingコアも同じ警告基準でビルドする。`make full-build`はクリーン状態からCLI、GUI、freestandingアーカイブを構築する。一方、list/set/dict内包表記は、式位置で一時コンテナを構築するためGNU statement expressionを使う。lambdaはC11 closure objectへ移行済みである。内包表記は`--c11`では明示的に拒否される。`--c11`の受理対象は、生成後に`-std=c11 -pedantic-errors`でコンパイルする品質ゲートを通す。組込み向けにはフレーム上限を機械的に検査する`make test-stack-usage`（`-Wstack-usage`、既定4096バイト）と、GCCの`-fanalyzer`で欠陥を追跡する`make test-analyzer`を用意する。詳細は [`../testing/STRICT_BUILD_AND_ANALYSIS_ALPHA0.6.md`](../testing/STRICT_BUILD_AND_ANALYSIS_ALPHA0.6.md) を参照する。

## 7. 意図的な制限と互換フォールバック

| 項目 | 現在の扱い |
|---|---|
| `yield from`、`send`、`throw`、`close` | `yield from iterable`の値委譲とStopIteration終了を対応。委譲先への`send`/`throw`/`close`、StopIteration戻り値の受取り、`yield from`を式として使う完全意味論は非対応 |
| async generator、Task、I/O待機、キャンセル | 非対応。`async with`は単一context managerとsimple-name `as` target、正常/例外exitを対応する。複数context、複合`as` target、body内の複雑な停止点、async for body/else内のawait、複数の入れ子async forは明示的な互換フォールバック対象 |
| await位置 | 独立した式文、または単純名への代入右辺にある単一await。算術・単項・単一比較・二項短絡論理を含む右辺は対応。複数await、副作用順が重要な複合式、条件式・while条件・async for body/elseをまたぐ停止は明示的な互換フォールバック対象 |
| 複素数、bytes、bytearray | 非対応 |
| デコレータ | module-level function/classに対応。decorator式は上から下へ一度ずつ評価し、返されたcallableを下から上へ適用する。async functionも対象。nested function、class method、可変長引数を持つdecorator、decorated functionへのkeyword callは、誤変換を避けるため明示診断 |
| 多重継承 | 対応。`class D(B, C)`はPythonと同じ**C3線形化**でMROを求め、ダイヤモンド継承でも基底メソッドの選択・`isinstance`・`except`の判定がCPythonと一致する。`super()`（0引数形）も**実体の型のMRO**で「メソッドを書いたクラスの次」から解決する（`super(Class, self)`の2引数形は明示診断）。直接基底は8個、MROの名前は32個、線形化の深さは12段までを上限とし、超える階層は従来の深さ優先順へフォールバックする（`Makefile`の`P2C_MRO_*`）。 |
| 基底クラスのクラス属性 | 対応。サブクラスのインスタンス・クラスオブジェクトのどちらからもMRO順に探索する |
| 束縛メソッド | 対応。`m = obj.method`で束縛メソッド（環境辞書に`self`と名前を持つクロージャ）を返し、`sorted(key=...)`・`map()`・`filter()`・コールバック引数へ渡せる。`hasattr(obj, "method")`も真になる。クラスオブジェクト経由のメソッド取り出し（`Class.method`）は未対応 |
| タプル・リストの順序比較 | 対応。`sorted()`/`min()`/`max()`は辞書式比較を行い、安定なマージソート（O(n log n)、`key=`/`reverse=`対応）で並べ替える。dict・set・None・インスタンスなど順序を持たない型同士の比較はCPython同様`TypeError` |
| `frozenset`の不変性 | setフォールバックのため非対応 |
| レキシカルクロージャ・`nonlocal`捕捉 | ネスト`def`とlambdaの読取りcapture、direct childの`nonlocal`共有cell再束縛、関数ローカルgenerator expressionの外側ローカルcaptureを対応。再帰closure、深い多段capture、nested closure内generator expression captureは明示的な互換フォールバック対象 |
| 任意精度整数 | 非対応。64ビット範囲外は`OverflowError` |
| 文字列長・添字の単位 | UTF-8**バイト**単位。CPythonのコードポイント単位とは異なり、非ASCII文字列の`len()`・添字・スライスはCPythonと一致しない（`ord`/`chr`は単一Unicodeスカラー値を扱う） |
| 埋め込まれたNULバイト | 文字列リテラル中の`\x00`は内容に含まれるが、長さ・検索はC文字列として扱うため末尾以降を保持しない |
| `\N{...}` | Unicode名前表を持たないため明示診断。`\uXXXX`/`\UXXXXXXXX`を使う |
| `divmod`/`pow`の任意精度 | 64ビット固定幅。`pow(base, exp, mod)`の法が64ビット範囲を超える場合は`OverflowError` |
| 多引数呼出し・一部リテラル要素の副作用順 | 二項式とf-string連結は順序保証済み。残る全経路は未保証 |
| `type`実行時オブジェクト | 型構文は型消去受理する。`typing.TypeAliasType`、遅延`__value__`評価、型aliasの値としての実行時利用は非対応 |
| 内包表記のstrict C11出力 | 非対応。`--c11`で明示診断 |
| generator expression | モジュールまたは通常関数内の**複数`for`節（〜8節）**、**タプル/リストのunpack target**、任意数のfilter、関数ローカル自由変数capture、唯一の関数引数での外側括弧省略に対応。最も外側のiterableだけを生成時に評価するPythonの規則にも従う。star target、nested closure内capture、`async for`、`await`は明示診断 |
| finally内の制御移譲 | `finally`内の`return`/`break`/`continue`を対応（finally本体を実行し、保留中の例外・`return`を上書きする）。`try`本体・`except`節・`else`節からの脱出では`finally`を内側から実行してから脱出する |

## 8. 品質ゲート

`make test`はHosted CLIのスモーク、GC/GUI C回帰、固定幅整数境界テスト、登録済みプラットフォーム出力アダプタ、ジェネレータ・協調asyncランタイム回帰、CPython差分スモーク、静的ヒープ・platform write・GC・コルーチンを通すベアメタル実行ハーネス、変換済みベアメタルPython例のfreestanding C11コンパイル、starred unpack、set回帰、set comprehensionのstrict C11期待診断、decoratorの期待診断、Python 3.13型構文の変換・厳格C11実行、Alpha0.6機能を含むHosted・厳格C11・freestanding単一ヘッダー検証、**749件のCPython差分コンフォーマンス**、固定seed・各48操作のdict/set差分ファジング、期待診断、ホストGUI、freestandingアーカイブ、自作OS統合ゲート（`make test-embed-runtime`、`make test-freestanding-setjmp`、`make test-embed-compile`、`make test-embed-generated`、`make test-hobby-os-template`、`make test-crlf`）、ならびに`make CC=clang test-sanitizers`によるASan/UBSanのparser専用回帰と460件差分を実行する。GCCとClangの両方で`make full-build`と`make test`を通すことを品質基準とし、common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10モジュールにはClang静的解析を追加で実行する。GCCがなくても、`make CC=clang test-single-header-c11`により単一ヘッダーだけを`-std=c11 -pedantic-errors`で自己完結ビルド・実行でき、`make CC=clang test-single-header-freestanding`により、Hosted allocator・出力・時刻シンボルを要求しない実装部をコンパイルできる。数値変換・書式化・数学関数はOS側が提供する契約である。コンフォーマンスケースの目的とIDは `docs/CONFORMANCE_TEST_MATRIX_ALPHA0.6.md` に、監査で見つけた品質課題と改善根拠は `docs/STRICT_REVIEW_ALPHA0.6.md` に記録する。

決定的コンテナファジングは品質監査で追加8 seed・各64操作、計512操作をGCC/Clang双方で実行した。seed、操作数、失敗最小化、回帰昇格の運用は [`../testing/CONTAINER_FUZZING_ALPHA0.6.md`](../testing/CONTAINER_FUZZING_ALPHA0.6.md) に定義する。

自作OS統合の品質ゲートとして、`make test-embed-runtime`（組込みヒープの分割・合体・realloc・破損検出、ライフサイクル、タスク再起動、GCの安全側停止と有効化、OOM通知、MemoryError化、panic経路）、`make test-freestanding-setjmp`（カーネル提供setjmp/longjmpの契約）、`make test-embed-compile`（カーネル相当環境での変換器コア実行）、`make test-embed-generated`（`--embed-entry`生成モジュールの実行とCPython差分）、`make test-hobby-os-template`（テンプレートの警告即エラー・コンパイルとライブラリ化）、`make test-crlf`（LF/CRLF/CRで生成Cが完全一致すること）、`make test-allocator-injection`（カーネルアロケータの注入と静的フォールバック判定）、`make test-setjmp-hook`（`P2C_SETJMP`/`P2C_LONGJMP`の差し替え）を`make test`へ追加した。詳細は [`HOBBY_OS_EMBEDDING_ALPHA0.6.md`](HOBBY_OS_EMBEDDING_ALPHA0.6.md) と [`../testing/CONFORMANCE_TEST_MATRIX_ALPHA0.6.md`](../testing/CONFORMANCE_TEST_MATRIX_ALPHA0.6.md) を参照する。

追加・修正する機能は、対応表の更新だけで完了とせず、少なくとも一つのCPython差分ケース、期待診断ケース、C API不変条件テスト、または再現可能なファジング操作列を伴わなければならない。
