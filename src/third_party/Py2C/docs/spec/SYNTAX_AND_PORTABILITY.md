# Python Code to C Alpha0.6 — 構文・移植性仕様

## 方針

Alpha0.6は、ホストOSと自作OSで共有できるC11コアを維持し、Python 3の実用的なサブセットをCへ変換します。新機能は、生成Cの意味が明確で、freestandingコアに外部GUI・POSIX・GCC固有機能を持ち込まず、回帰テストで検証できるものから追加します。

## 対応構文

| 分類 | 対応内容 |
|---|---|
| 代入 | 単純代入、連鎖代入、複合代入、属性・添字代入、タプルアンパック、**starred unpack代入**、`name := expression`、実行時無視の型注釈付き代入 |
| starred unpack | 代入の`a, *middle, z = seq`、`*head, z = seq`、`a, *tail = seq`に加え、forターゲットの`for first, *middle, last in iterable`。starredターゲットは1個かつ単純名に限定 |
| 関数 | Python 3.13型パラメータ、位置専用引数`/`、デフォルト引数、キーワード引数、`*args`、`**kwargs`、キーワード専用引数、既知の関数に対する呼び出し側アンパック、`print(sep=..., end=...)`。順次本体の`yield [value]`と`yield from iterable`は状態機械ジェネレータへ、`async def`・単純な`await coroutine_call()`・`asyncio.run()`はFIFO協調コルーチンへ変換する。ネスト関数・`nonlocal`捕捉は移植可能なクロージャ未実装のため診断 |
| クラス | 単一継承、`__init__`継承、通常メソッド、`__str__`、`__repr__`、`__eq__`、**メソッドのデフォルト引数** |
| 制御 | if、while、通常・starred unpack付きfor、break、continue、try/except/else/finally、値付きraise、`raise exc from cause`、bare raise、assert、**単一・複数コンテキストのwith**、literal/capture/wildcard/OR/guard、sequence、mapping、star、as、mapping `**rest`の`match/case`。match subjectは一度だけ評価し、`**rest`は指定済みキーを除く新規dictへ束縛する。forは`iter()` / `next()` / `StopIteration`の汎用反復経路を使い、ジェネレータを反復可能 |
| 単一値 | `...`（Ellipsis）とビルトイン名`Ellipsis`。`def f(): ...`のスタブ本体、`is`/`==`、`repr`、`type()`、コンテナ要素として動作 |
| 文字列エスケープ | 8進`\ooo`、16進`\xHH`、`\uXXXX`/`\UXXXXXXXX`（UTF-8へ変換）、行継続`\`+改行。未知のエスケープはバックスラッシュごと保持（CPython互換）。`\N{...}`はUnicode名前表を持たないため明示診断 |
| 式 | 算術、比較、論理、条件式、リスト・タプル・辞書・**set**、添字、スライス、属性、f-string、`str.format()`、**`%`書式（`"%s(%.2f)" % (name, x)`。`%s`/`%r`/`%a`/`%d`/`%i`/`%u`/`%f`/`%F`/`%e`/`%E`/`%g`/`%G`/`%x`/`%X`/`%o`/`%c`/`%%`と、フラグ（`-` `+` 空白 `#` `0`）・幅・精度・`*`（引数から取得）。右辺はタプル＝位置引数、`%(key)`を含む書式＝辞書、それ以外＝単一値。引数不足と余剰はTypeError、キー欠落はKeyError）**、`divmod`、`pow(base, exp, mod)`、`format(value, spec)`（桁区切り`_`/`,`と`%`型を含む）、`callable`、UTF-8単一Unicodeスカラー値を扱う`ord`/`chr`、64ビット整数をPython表記へ変換する`bin`/`oct`/`hex`、開始値を受ける`sum(iterable, start)`、範囲指定`find`/`index`/`rfind`/`rindex`/`count`、`split(sep, maxsplit)`、`strip(chars)`、prefix/suffixのstart/stop・文字列tuple、`partition`/`rpartition`、ASCII文字種判定（`isalpha`/`isdigit`/`isalnum`/`isspace`/`islower`/`isupper`/`isidentifier`/`isascii`/`isprintable`）、`splitlines(keepends)`、`expandtabs(tabsize)`、回数指定`str.replace`、範囲指定`list.index`、lambda、**list/set/dict内包表記**、`print(sep=..., end=...)`、setの`&`/`|`/`-`/`^`と拡張代入、dictの`|`/`|=`。Pythonの改行系・制御文字リテラルは安全なCエスケープで生成し、二項式とf-string連結は左から右へ評価 |
| 辞書 | 辞書リテラル、`**mapping`unpack、dict内包表記、`dict()`、`dict(iterable)`、`dict(key=value)`、`dict(**mapping)`、`dict.fromkeys(iterable, value=None)`、`keys`/`values`/`items`/mapping・pair iterable `update`/`pop`/`popitem`/`setdefault`/`clear`/`copy`、`|`/`|=`、値等値比較。更新は左から右の後勝ちで、`fromkeys`は共通value参照を持ち、非ハッシュ可能キーは拒否 |
| 集合 | `{1, 2, 3}`、`set()`、`set(iterable)`、`len`、包含判定、`for`反復、`type`、`isinstance`、`add`/`discard`/`remove`/`pop`/`clear`/`copy`、`union`/`intersection`/`difference`/`symmetric_difference`、各update系、`isdisjoint`/`issubset`/`issuperset`、集合演算、挿入順の表示。重複要素は構築時に排除し、`pop`の選択要素に依存しない。非ハッシュ可能要素は拒否 |
| frozenset | `frozenset()` と `frozenset(iterable)` は**可変setへの明示的互換フォールバック**として変換。値の構築・反復・包含・長さ取得は利用可能だが、不変性・固有型名・ハッシュ可能性は提供しない |
| 型構文 | soft keyword `type Alias[params] = expression`、型パラメータ既定値、TypeVarTuple、ParamSpecを型消去で受理。`typing.TypeAliasType`などの実行時型aliasは非対応 |
| ランタイム | コンテナ、GC追跡されるexception cause、small-int cache、状態機械ジェネレータ、固定長FIFO協調スケジューラ、math、ヘッドレスpygame API |

## 未対応構文のフォールバック（`--fallback`）

未対応構文は既定で**位置付きの明確な診断**を出して停止します（不正なCを出力しない
方針）。ビルドを止めたくない場合は `--fallback` を指定すると、未対応の箇所が
**実行時に `NotImplementedError` を送出するスタブ**へ置き換わり、変換とビルドは成功します。

- 対象: parser レベル（`bytes` リテラル、複素数リテラル、`\N{...}`）と
  codegen レベルの未対応式・未対応文
- 到達しなければそのまま動く（未対応機能を使わない経路を試せる）
- 到達した場合は `NotImplementedError: unsupported <種別> at line N (converted with --fallback)`
- 回帰: `make test-fallback`（strict診断 / 未到達で動作 / 到達でNotImplementedError）

## GC（適応しきい値と統計）

自動収集のしきい値は状況に応じて調整されます。

- 収集してもほとんど解放されない（`freed * 4 < live`）→ しきい値を倍々に伸ばす（上限4MiB）
- よく解放できる（`freed > live / 2`）→ 基準値へ25%ずつ戻す
- `p2c_gc_set_threshold(bytes)` は基準値と現在値を同時に設定（成長回数をリセット）
- `p2c_gc_set_adaptive(false)` で固定しきい値に戻す
- `p2c_gc_stats(&stats)` で収集回数・追跡数・ピーク・現在のしきい値・成長回数・
  走査スタック語数・TLS一時ルート数を取得
- 回帰: `make test-gc-adaptive`（同一負荷で固定しきい値と比較。実測 96回→9回）

## 意図的な制限

デコレータ、`yield from`への`send`、`throw`、`close`、委譲戻り値、async generator、`async for`、`async with`、Task、キャンセル、I/O待機、複素数、bytes/bytearray、レキシカルクロージャと`nonlocal`捕捉は未対応です。多重継承は`class D(B, C)`と`super()`を含めて対応しますが、直接基底8個・MROの名前32個・線形化の深さ12段を上限とし、超える階層は深さ優先順へフォールバックします（`Makefile`の`P2C_MRO_*`）。pattern matchingではclass pattern、属性をたどるvalue pattern、任意の`collections.abc.Mapping`実装、複数star pattern、OR枝ごとに異なるcapture集合は未対応であり、移植性を保つ明示診断または互換フォールバックの対象です。`yield [value]`、`yield from iterable`、`async def`、単純な`await coroutine_call()`、`asyncio.run()`は対応しますが、停止点をネストした制御構造・複合式へ置く完全な状態分割は対象外です。整数は64ビット固定幅であり、CPythonの任意精度整数とは異なり範囲外で`OverflowError`になります。文字列はUTF-8バイト列として保持し、`len()`・添字・スライスは**バイト単位**です（CPythonのコードポイント単位とは異なります）。`\N{...}`（Unicode名前エスケープ）は名前表を持たないため明示診断し、`\uXXXX`/`\UXXXXXXXX`を使用してください。二項式とf-string連結は順序保証済みですが、多引数呼出しおよび一部リテラル要素に副作用を置いた場合の全経路は未保証です。list/set/dict内包表記とlambdaは現在のC出力方式に依存するため、厳格なISO C11のみで生成コードをコンパイルする構成では無効化または代替バックエンドを選ぶ必要があります。これはコンパイラコアのfreestandingビルドとは別の、生成コードのバックエンド制約です。`frozenset`は上表の通りsetへフォールバックするため、不変コンテナが必須のコードでは使用しないでください。

## 自作OSへの組み込み

コンパイラコアは `P2C_Platform` のアロケータ、再確保、解放、出力、時刻フックを通じてOSへ接続します。OS側は利用前に`p2c_platform_set()`でアダプタを登録し、`print`と例外診断は登録済み`write`コールバックへ出力されます。GUIは `P2C_GuiContext` の固定コマンドバッファへ記録し、OS側の `P2C_GuiBackend` がフレームバッファ、GPU、または独自コンポジタに描画します。GUIコアはネットワーク、ウィンドウシステム、特定のピクセル形式を要求しません。

GC管理オブジェクトをOSの非同期キューや描画状態が保持する場合、保持スロットを `p2c_gc_register_root()` で登録し、不要になった時点で `p2c_gc_unregister_root()` を呼びます。これにより、保守的スタック走査の範囲外でも到達性が明示されます。

## 検証

`make full-build` はクリーン状態からHosted CLI、ローカルGUI、freestandingコア、生成済み単一ヘッダーを、`-Werror`、`-Wpedantic`、`-Wshadow`、`-Wformat=2`、`-Wstrict-prototypes`、`-Wmissing-prototypes`、`-Wold-style-definition`、`-Wredundant-decls`、`-Wundef`、`-Wconversion`、`-Wsign-conversion`、`-Wcast-qual`、`-Wwrite-strings`、`-Wdouble-promotion`、`-Wvla`、`-Wfloat-equal`を含む警告即エラー設定（`-Wswitch-enum`と`-Wnull-dereference`は除外理由を`Makefile`へ明記）で構築します。`make test` はスモーク、GC・GUI Cテスト、64ビット整数境界テスト、プラットフォーム出力アダプタ、ジェネレータ・協調asyncランタイム、静的ヒープ・platform write・GC・awaitを通すベアメタル実行ハーネス、変換済みベアメタルPython例のfreestanding C11コンパイル、starred unpack、set回帰、for starred unpack、`print`キーワード、文字列探索範囲、`split(maxsplit)`、`strip(chars)`、prefix/suffix範囲、partition、文字種判定、splitlines、expandtabs、制御文字リテラル・repr、高次dict/set、list拡張、UTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum`、`divmod`/`pow(a,b,mod)`/`format`/`callable`、`...`（Ellipsis）、文字列エスケープ（8進/16進/`\u`/`\U`/行継続/未知エスケープ）、Python 3.13型パラメータ・`type`文の型消去変換、位置専用引数、`yield from`、`raise from`、try/finally脱出順序の回帰、sequence/mapping/as/star/OR/guardとmapping `**rest`を含むHosted・strict C11・freestanding単一ヘッダー検証、**555件のCPython意味論差分コーパス**、既定3 seed・各48操作の決定的dict/set差分ファジング、strict C11・ネスト関数診断、ホストGUI、freestandingコア、自作OS統合ゲート（`p2c_embed` のヒープ・ライフサイクル・GC安全側停止・OOM・panic、カーネル提供setjmp/longjmp、カーネル相当環境での変換器コア実行、`--embed-entry`生成モジュールのCPython差分、組み込みテンプレートのコンパイル、LF/CRLF/CRでの生成C一致）の構築を実行します。GCCとClangの両方で`make full-build`と`make test`を通すことを品質基準とし、GCCがない環境でも`make CC=clang test-single-header-c11`で単一ヘッダーを`-pedantic-errors`の厳格C11として自己完結検証でき、`make CC=clang test-single-header-freestanding`でHosted allocator・出力・時刻参照のない実装部を検査できます。setはCPythonと異なり挿入順で内部保持・表示します。そのため順序を仕様としない集合の差分テストでは、`sorted()`で正規化して比較します。`make freestanding` は標準Cライブラリを使わないコア静的ライブラリを構築し、`make run INPUT=...` はHosted変換・コンパイル・実行を一括化します。ファジングのseed、失敗最小化、固定回帰への昇格は `docs/CONTAINER_FUZZING_ALPHA0.6.md` に定義します。

## 固定上限と `-D` 上書き（Alpha0.6）

組込み（カーネル）向けに一部の表は固定長配列で持っています。上限に達したときは
**黙って処理を飛ばさず**、`p2c: limit exceeded: ...` を必ず出力します（同じ上限について一度だけ）。
例外を投げても安全な場所（クラスレジストリ）では `RuntimeError` も投げます。

| 上限 | 既定値 | 上書きマクロ | 上限に達したとき |
|---|---|---|---|
| クラスレジストリ | 256 | `-DP2C_MAX_CLASS_REGISTRY=N` | 通知 + `RuntimeError`（登録を無言で落とさない） |
| MRO 名数 | 32 | `-DP2C_MRO_MAX_NAMES=N` | 通知（C3 を計算できないため DFS へフォールバック） |
| MRO 基底数 | 8 | `-DP2C_MRO_MAX_BASES=N` | 同上 |
| MRO 深さ | 12 | `-DP2C_MRO_MAX_DEPTH=N` | 同上 |
| MRO アリーナ | 3072 bytes | `-DP2C_MRO_ARENA_BYTES=N` | 同上 |
| GC ルート表 | 1024 | `-DP2C_GC_ROOT_CAPACITY=N` | 通知 + abort（回収を続けると不正解放になるため停止） |
| async キュー | 256 | `-DP2C_ASYNC_QUEUE_CAPACITY=N` | `RuntimeError` |

検証は `make test-limits`（`tests/limit_diagnostics_alpha06.sh`）:

- 既定値: 通知なし・CPython と同一出力
- `-DP2C_MAX_CLASS_REGISTRY=2`: 通知と `RuntimeError` が出る（無言スキップの廃止）
- `-DP2C_MRO_MAX_NAMES=2`: 通知が出て、結果は CPython と同一か、さもなくば明示的に失敗する（無言の不一致を出さない）
- `-DP2C_MAX_CLASS_REGISTRY=1024 -DP2C_MRO_MAX_NAMES=32`: 通知なし・CPython と同一出力（上書きが効く）

