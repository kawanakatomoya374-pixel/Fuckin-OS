# Python 3.13 構文・移植性・性能ギャップ監査

## 公式仕様の確認結果

Python 3.13の完全文法は、現在の実装対象に加えて、`except*`、async `for`/`with`、generator expression、async comprehension、`@`/`@=`、複素数、bytes/bytearray、dotted value/class pattern、class positional pattern、`__match_args__`などを定義する。[1]

単純文仕様は、代入の反復可能unpack、属性・添字・slice代入、`raise ... from None`、通常の`yield`/`yield from`、`global`/`nonlocal`等の意味論を規定する。[2] 式仕様はgenerator expressionの遅延評価、左端iterableだけが生成時に評価されること、async comprehension、完全なgenerator send/throw/closeを規定する。[3]

Python 3.13の言語変更にはtype parameter既定値、annotation scope拡張、`locals()`の定義済み更新意味論などがある。後二者はfreestandingコンパイラの実行時frame APIを要求するため、優先度を下げる。[4]

## 現行ギャップの優先順位

| 候補 | 構文価値 | C11/freestanding影響 | 性能・移植性判断 |
|---|---:|---:|---|
| class positional patternと`__match_args__` | 高 | 中 | class pattern既存基盤を拡張し、classメタデータをC静的配列化すれば動的反射を避けられる |
| dotted value/class pattern | 高 | 中 | module/class属性解決・評価順・例外伝播を確立してから追加する |
| generator expression | 高 | 高 | true lazy iteratorと閉包相当の捕捉状態を要求するため、現行state machineの大規模拡張が必要 |
| async for/async with | 高 | 高 | await停止点、cleanup、例外フレームをまたぐため、協調schedulerの改修を先行させる必要がある |
| `except*` / ExceptionGroup | 中 | 非常に高 | split/merge・禁止制御移譲・例外階層を新設する必要がある |
| `@` / `@=`、複素数、bytes | 中 | 高 | ランタイムobject種と数値/バイト列APIの新設が必要 |
| `nonlocal` closure | 高 | 非常に高 | closure environmentのGC・ABI契約を設計し直す必要がある |

## 今回の選定

まず、**class positional patternのうち自作classの`__match_args__`を静的メタデータとして生成する経路**を監査・実装候補とする。これは直前に導入したbare/keyword属性class patternを再利用し、C11・freestandingで動的型検査やCPython C APIを持ち込まずに済む。実装開始前にclass AST・コード生成・ランタイムclass metadataの既存設計を確認し、属性宣言順を安全に取得できない場合は、利用者明示の`__match_args__`だけを受理する。

性能面では、全追加経路でsubject・class判定・属性取得の再評価を行わず、生成Cの状態変数で一度だけ保持する。性能変化は固定入力の反復実行時間ではなく、既存差分テストおよび生成C構造の一回評価不変条件で回帰を防ぐ。

## 参照

[1]: https://docs.python.org/3.13/reference/grammar.html "Python 3.13 Full Grammar specification"
[2]: https://docs.python.org/3.13/reference/simple_stmts.html "Python 3.13 Simple statements"
[3]: https://docs.python.org/3.13/reference/expressions.html "Python 3.13 Expressions"
[4]: https://docs.python.org/3.13/whatsnew/3.13.html "What’s New In Python 3.13"

## Class positional patternの実装設計

既存ASTは`P2C_MATCH_CLASS`に`children`と`attr_names`を持つ。keyword childは`attr_names[i]`が属性名であり、positional childは同じ添字に`NULL`を格納して区別する。これにより既存の再帰解放・capture事前宣言を変えず、位置・keyword混在の子pattern順序を保存できる。

生成器はclass名による`p2c_isinstance_of_class()`判定を一度だけ行い、positional childごとに`p2c_match_class_positional(subject, index, &value)`、keyword childごとに既存の非送出`p2c_hasattr()`/`p2c_getattr()`を順に呼ぶ。各取得値は新しいC局所へ一度だけ保存してから再帰patternへ渡す。これによりattribute取得の重複、副作用の重複、guard前の再評価を防ぐ。

ランタイムはinstanceのclass objectから`__match_args__`を取得し、tupleの該当要素がstrであることを確認してから属性を非送出取得する。built-in class patternは公式仕様どおり一つのpositional patternだけをsubject自身に対応付ける。classの`__match_args__`がない、型がtupleでない、index範囲外、tuple要素がstrでない、属性がない場合はすべてcase失敗として扱い、例外を残さない。

この設計はC11・freestandingで追加OSコールバック、動的loader、CPython C API、可変長スタックを要求しない。新規APIは単一ヘッダー再生成に含め、CPython差分ではcustom class、継承、mixed positional/keyword、nested pattern、`__match_args__`不在・不正型・属性不在、組込みint/strの一要素positionを固定する。
