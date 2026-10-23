# Python 3.13: dotted pattern と generator expression の実装調査

## 仕様上の要件

Python 3.13の完全文法では、value patternは`attr`、class patternは`name_or_attr`を受け取る。`attr`は少なくとも一つのドットを含む名前または属性連鎖であり、単独の名前はcapture patternと区別される。[1]

構造的パターンマッチングでは、subjectを一度評価し、caseを上から順に試す。pattern成功後にだけguardを評価する。value patternの値は実装により同一match文内でキャッシュされる可能性があるが、利用者はそのキャッシュに依存できない。[2]

generator expressionは括弧で囲まれたcomprehensionであり、要素式・後続for・if節は`next()`時に遅延評価される。一方で左端for節のiterable式はgenerator expressionを構築する時点で評価され、iterator作成時の例外も構築時に発生する。暗黙scopeによりターゲット名は外側へ漏れず、yield/yield fromは内側に置けない。[3]

## 実装段階

1. dotted **value pattern**を先行する。既存の`AST_ATTRIBUTE`と通常の属性評価をpattern値に再利用し、captureとの曖昧さをドット有無で解消する。`case constants.RED:`のようなmodule/class属性をC局所へ一度だけ評価し、`p2c_obj_eq()`でsubjectと照合する。
2. dotted **class pattern**はdynamic class objectとinheritance照合APIが必要である。既存`p2c_isinstance_of_class()`は文字列名入力のため、class objectを受ける`p2c_isinstance_object()`を追加する工程で実装する。
3. generator expressionは現行の`AST_COMPREHENSION`が生成する即時コンテナと異なる。まず一for・任意if・name targetの遅延state machineを新AST種別とgenerator frame localsで実装し、複数for、star target、async clause、awaitは別段階とする。

## 性能・移植性不変条件

dotted value patternはattribute連鎖をcase試行ごとに一回だけ評価する。generator expressionは左端iterableを生成時に一回だけ評価し、各nextで必要な最小作業だけを実行する。いずれもC11可変長スタック、CPython C API、OS追加コールバックを使わず、状態はGC走査済み`OBJ_ITERATOR`と通常のP2C object graph内に置く。

## 参照

[1]: https://docs.python.org/3.13/reference/grammar.html "Python 3.13 Full Grammar specification"
[2]: https://docs.python.org/3.13/reference/compound_stmts.html "Python 3.13 Compound statements"
[3]: https://docs.python.org/3.13/reference/expressions.html "Python 3.13 Expressions"

## 実装監査メモ

既存`AST_COMPREHENSION`はlist/set/dictを即時構築するため、generator expressionへ流用するとPython 3.13が規定する遅延評価・左端iterableのみの即時評価に違反する。したがってgenerator expressionは別AST種別とし、通常のgenerator functionと同じ`OBJ_ITERATOR` state machineを生成する必要がある。

既存パーサは丸括弧atomで最初の式を解析した後にcommaをtuple、右括弧をgroupとして扱う。`TOK_KW_FOR`を検出したときにだけgenerator expression ASTへ分岐できる。これにより`(x for x in source)`の最小文法を受理できるが、generator bodyは外側scopeから値を参照するため、locals captureとターゲット非漏出を実装しなければならない。

## State machine再利用の追加要件

既存`gen_suspension_function()`は直線的な関数本文のstatement indexをstateとして持つ。generator expressionではfor反復の継続位置、iterator object、ターゲット値、if通過状態を独立したgenerator localsに保存する必要があり、そのままの転用はできない。最小実装では単一`for name in iterable`・任意の単一if・任意要素式に限定し、生成時に`p2c_builtin_iter(leftmost_iterable)`をlocalsへ保存する専用step関数を生成する。

step関数は`p2c_builtin_next()`を`P2C_ExceptFrame`で囲み、StopIterationだけをfinishへ変換する。non-StopIterationはそのまま送出する。ターゲット値はgenerator localへ保存し、要素式・if式のnameをgenerator local参照へ置換する。左端iterable以外のscope参照は生成時点に評価せず外側のC局所またはmodule globalを参照する。GCはiterator locals mapが保持するiterable/targetを既存`OBJ_ITERATOR` traverseで辿る。

## 反復例外処理の再利用

通常for文は`p2c_builtin_next()`を`P2C_ExceptFrame`で囲み、`StopIteration`だけを正常終了へ変換し、それ以外を再送出する。この経路をgenerator expression stepへ移植する。stepはiterator objectをgenerator local `__p2c_genexp_iter_N`、targetを対象名のlocalへ保存する。ifが偽ならstep内部で次の反復を継続し、真なら`p2c_generator_yield()`で要素値を返す。leftmost iterableはgenerator object作成前に外側C式として一度だけ評価する。

## 実装継続の安全基線

現時点ではgenerator expressionの構文は、未完成のASTを静かに`None`へ変換しないよう明示診断のままとする。dotted value/class patternの実装済み経路はClangの警告即エラービルドで再確認済みである。generator expressionは専用step関数・free variable capture・反復状態の保存を同一変更で完結させ、CPython差分を追加してから構文受理を有効化する。

## CPython基準ケース

`tests/generator_expression_alpha10.py`は、左端iterableがgenerator object作成時に一度だけ評価されること、要素式とfilterが`next()`時に進むこと、target名が外側へ漏れないことを固定する。CPython 3.13系互換の基準出力は`['source']`、`20`、`['source']`、`[40]`、`not-leaked`である。専用state machineを有効化するまで、このケースはコンフォーマンス台帳へ登録しない。
