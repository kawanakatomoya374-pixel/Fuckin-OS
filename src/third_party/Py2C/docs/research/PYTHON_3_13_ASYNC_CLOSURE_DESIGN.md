# Python 3.13 非同期反復・await・closure設計記録

## 目的

本記録は、**Python Code to C Alpha0.6** における三つの拡張、すなわち関数ローカル自由変数のcapture、任意の式位置に現れる`await`、および`async for`を、GNU拡張へ依存せずISO C11とfreestanding構成を維持して導入するための実装契約である。対象はCPython全実装ではなく、既存の協調FIFOスケジューラおよびgenerator state machineと意味論上整合する移植可能な部分集合である。

## 公式意味論からの受入基準

Pythonの各関数ブロックは独立した名前空間であり、ブロック内で束縛される名前は、そのブロックのローカル名となる。現在のブロックで束縛されず、外側の関数ブロックで束縛された名前は自由変数であり、最近接の外側関数スコープから解決される。`nonlocal`はその最近接の外側関数スコープに既に存在する束縛を指し、存在しない場合は構文エラーとなる。[1]

`await`はコルーチン実行を中断し、待機対象が完了した後にその結果値として式評価を再開する。したがって、二項演算、比較、関数呼出し、条件式、条件分岐、ループ条件のいずれに現れても、中断前に評価済みの左辺・引数・分岐判定を保持しなければならない。[2]

`async for target in iterable:`は非同期反復子を得て、各回で`__anext__()`が返すawaitableを待機し、その結果をtargetへ束縛する。`StopAsyncIteration`はループ終了に変換され、通常終了時だけ`else`節が実行される。これは同期`for`における`StopIteration`との対称な契約である。[3]

> 左端iterableを生成時に一度だけ評価するgenerator expressionの既存契約は維持する。非同期generator expressionやasync comprehensionは本段階では対象外とし、明示診断を継続する。[2]

| 機能 | 最初の実装範囲 | 明示的な後続範囲 |
|---|---|---|
| closure | ネスト`def`とlambdaのread-only free variable capture、複数closure間で共有される環境 | `nonlocal`による再束縛、クラス本体特殊スコープ、decorator完全意味論 |
| general await | 代入・return・式文に加え、算術・比較・論理・条件式・call引数の式位置 | try/finallyを跨ぐawait、with/async with、comprehension内await |
| async for | async def内、単純name target、body、`else`、`break`/`continue` | tuple/star target、async generator、async comprehension、async with |

## 既存実装の制約

現在のsuspension function生成は、関数本体を文番号ベースの`switch`へ変換し、ローカル値をgenerator locals dictへ保存する。`await`は独立式文または単純代入右辺に限定され、前文がawaitだった場合にのみ`p2c_generator_await_result()`を読む。このため、`1 + await f()`、`f(await g())`、`if await pred():`、`while await pred():`は継続点より前に評価した値を保存できない。

実行時は`OBJ_ITERATOR`に`step`、`locals`、`awaiting`、`result`、`state`を保持し、GCは`locals`、`awaiting`、`result`を走査する。したがって、一般awaitとasync forの再開用一時値も同じgenerator locals dictに保存すれば、既存のGC到達性とfreestanding allocator契約を再利用できる。

既存の`OBJ_FUNCTION`は関数ポインタと名前だけを保持し、capture環境を持たない。また、意味解析は基本的なnested scope lookupを行うが、free variable集合、cell、`nonlocal`の所有スコープを注釈として記録しない。このためclosureはコード生成だけでなく、ASTまたはsemantic metadataを通じた名前解決情報を必要とする。

## closureの設計

closure専用の実行時オブジェクト型は追加せず、既存の`OBJ_FUNCTION`を後方互換に拡張する。`v_function`の末尾にGC管理下の`env` dictと、closure呼出し用の関数ポインタを追加する。従来のトップレベル関数は`env == NULL`であり、既存の`p2c_function_new()`経路を保持する。ネスト関数は`p2c_closure_new(name, entry, env)`で生成し、`p2c_call()`は通常関数かclosure entryかを分岐する。

capture値はコピーではなく、環境dict内の**cell object**で表す。外側スコープでcapture対象となる各名前に`P2C_Cell`相当のGC管理オブジェクトを一つだけ作成し、内側closureは同じcellを参照する。これにより複数closureが同じ外側束縛を共有でき、後続段階の`nonlocal`はcellの値差替えだけで実装できる。最初の段階では`nonlocal`文もcell再束縛まで実装することを目標にするが、`global`は既存のmodule-global経路を維持する。

| 層 | 追加する責務 | GC契約 |
|---|---|---|
| semantic | 各関数のlocal、free、cell、nonlocal集合を収集し、最近接の外側function ownerを解決する | 解析時のみ。C生成へ安定した注釈を渡す |
| codegen | cell所有関数でcell初期化、ネスト定義でcapture env生成、名前read/writeをlocal/cell/globalへ振り分ける | 環境はruntime dict内のobject参照のみ保持 |
| runtime | cellとclosure環境を保持するfunction object、closure call dispatch、GC traverse | function env、cell value、closure entryの引数を正しく辿る |

lambdaは同一closure基盤へ移し、既存のGNU statement-expression / nested function経路を廃止する。この変更により、lambdaもstrict C11およびfreestandingで同じcapture意味論を持つ。

## 一般awaitの設計

任意式位置awaitは、式を一度で文字列出力する`gen_suspension_expr()`を、**A-normal formに近い継続生成器**へ置換して実装する。変換器は式木を左から右へ走査し、awaitを含む部分式ごとに以下を生成する。

1. awaitより前に必要な副式をgenerator localsへ保存する。
2. `p2c_generator_await(generator, awaitable, continuation_state)`を返して中断する。
3. continuation stateで`p2c_generator_await_result(generator)`を一度だけ取得し、保存済み副式と結合して以降の式評価を続ける。

状態番号は文番号ではなく、関数単位の単調増加counterで管理する。各stateは一つのC blockとし、再開時に落下実行されないよう必ず`return`または次stateへの明示遷移を行う。式評価順はPythonと同じ左から右に固定する。

最初の一般化対象は、`AST_BINOP`、`AST_UNARYOP`、単一比較、`AST_BOOLOP`、`AST_IFEXP`、単純name callの位置引数、属性呼出しのreceiver/位置引数、`return`、`if`、`while`である。短絡評価を伴う`and`/`or`および条件式は、左辺または条件を保存してからtruthy判定をstate内で行い、不要な右辺を評価しない。

## async forの設計

`AST_ASYNC_FOR`を`AST_FOR`と同じpayloadで追加し、parserでは`async`の直後に`def`だけでなく`for`も受理する。コード生成は通常関数ではなく、suspension function内部でのみ許可する。

各async forは以下のgenerator localsを一意名で保持する。

| local | 用途 |
|---|---|
| `__p2c_asyncfor_iter_N` | `__aiter__()`評価結果。ループ全期間で一回だけ生成 |
| `__p2c_asyncfor_awaitable_N` | 現在の`__anext__()`戻り値。GCが親子待機関係を辿るため保持 |
| `__p2c_asyncfor_item_N` | await完了後の反復値。target束縛前の再開値 |
| `__p2c_asyncfor_broken_N` | `break`と通常尽尽の区別。else節の条件に使用 |

反復の各回は`__anext__()`を呼び、その戻りawaitableを`p2c_generator_await()`へ渡す。再開stateで`p2c_generator_await_result()`を読む際に`StopAsyncIteration`を捕捉し、正常終了stateへ遷移する。その他の例外はそのまま親coroutineへ伝播する。`continue`は次の`__anext__()` stateへ、`break`は`broken`を設定してループ後stateへ遷移する。正常終了時だけ`else` suiteを実行する。

最初の段階では一般object protocolを`p2c_call_attr(iterable, "__aiter__")`および`p2c_call_attr(async_iter, "__anext__")`で実装する。既存のcoroutine objectはすでにawaitableであるため、追加のTask/Future/I/O抽象化を要求しない。

## 実装順序と不変条件

まずsemantic metadataとclosure/cell runtimeを完成させ、ネスト`def`、lambda、関数ローカルgenerator expressionが外側変数を参照できる状態を作る。次に一般awaitの継続生成器を導入し、最後にその基盤上でasync forを導入する。この順序により、async for bodyとawait式が同じstate allocation・GC保持・例外伝播を共有する。

各段階で生成Cは`-std=c11 -Werror -Wpedantic`でClang/GCC双方を通す。`PYTHON_CODE_TO_C_NO_STDLIB`構成は動的ローダやOS依存APIを追加せず、既存の`P2C_Platform` allocatorのみで動作することを維持する。単一ヘッダーは正本の更新後に必ず再生成する。

## 参考資料

[1]: https://docs.python.org/3.13/reference/executionmodel.html#naming-and-binding "Python 3.13 Language Reference — Naming and binding"
[2]: https://docs.python.org/3.13/reference/expressions.html#await-expression "Python 3.13 Language Reference — Await expression and generator expressions"
[3]: https://docs.python.org/3.13/reference/compound_stmts.html#the-async-for-statement "Python 3.13 Language Reference — The async for statement"

## 実装進捗（作業中）

closure基盤として、`OBJ_FUNCTION`へGC走査対象の`env`とclosure entryを追加し、`OBJ_CELL`を導入した。ネスト`def`は外側関数のローカル値をcellとして環境dictへ格納し、生成されたstatic C11 closure entryが環境から自由変数を読む。外側関数終了後に複数のclosureインスタンスが独立して値を保持する差分テストはClang/GCC双方でCPythonと一致した。現時点のnested `def`はread-only captureを対象とし、`nonlocal`による共有cell再束縛、再帰closure、lambdaの同一基盤移行は後続作業である。

awaitについては、代入右辺の`AST_BINOP`、`AST_UNARYOP`、単純call引数に含まれる単一awaitを検出し、awaitableで中断後、次stateで`p2c_generator_await_result()`を用いて式を再構成する最初の経路を導入した。`1 + await value(2)`および`total * (await value(3))`はClang/GCC双方でCPythonと一致した。複数await、前置副作用を伴う部分式、短絡演算、条件式、`if`/`while`条件はA-normal form継続生成器へ移行するまで未完了であり、過剰な対応表明をしない。

`async for`は専用`AST_ASYNC_FOR`、parser受理、AST解放、`StopAsyncIteration`の例外生成までを導入した。現状はstate machine lowering未統合であり、無出力による誤成功を禁止する明示診断を出す。次段階では`__aiter__`、`__anext__`、awaitable保持、`StopAsyncIteration`捕捉、body/else/break/continue stateを実装する。

> 作業中の品質方針: 上記の初期実装は既存339件コンフォーマンスを維持するための中間地点である。未実装の継続変換やasync for loweringを対応済みとして機能リファレンス・リリースノート・配布アーカイブに載せず、全CPython差分・GC・GCC/Clang・freestanding品質ゲート通過後にのみ公開範囲を更新する。

既存の直接C generator/async runtime回帰もClangの`-Werror -Wpedantic`構成で通過し、closure/cellのGC traverse追加が既存のawait FIFOランタイムを退行させていないことを確認した。

nonlocal共有cell実装の開始前に、最新作業ツリーはClangの`-std=c11 -Werror -Wpedantic`で再構築可能であることを確認した。

チェックポイント: closure captureと算術式内単一awaitはClang/GCCでCPython差分一致、既存C01–C339は全件維持、async forはAST/parser受理済みでstate machine lowering待ちである。

最新チェックではGCC側でもC01–C339のCPython差分が全件一致した。したがって、作業中のclosure/cell・複合await初期経路・async for AST/parser追加は既存のGCC互換性を退行させていない。

`AST_ASYNC_FOR`はAST dumpにも統合し、Clang警告即エラー構築で確認した。これによりstate machine lowering前でも構文受理結果を可視化して検証できる。

async for lowering実装前のpreflightとして、コンパイラ実行可能状態と`tests/async_for_alpha06.py`の存在を確認した。

同期forの既存実装は専用の`active_loop_id`と`_p2c_loop_broken_N`でbreakとelse節を区別している。async for loweringでは同等のbroken状態をgenerator localsへ保存し、`StopAsyncIteration`の通常終了時だけelse stateへ遷移し、breakではelseを迂回する必要がある。

継続チェックポイントでは、コンパイラ実行可能状態とclosure capture・await binop・async forの各差分テスト資産が揃っていることを確認した。

async for loweringのランタイム下地として、`p2c_async_iter()`と`p2c_async_next()`を追加した。両者は既存の属性呼出し・例外伝播を用いてそれぞれ`__aiter__`、`__anext__`を実行し、Clang C11警告即エラー構築を確認した。

`p2c_async_iter()`と`p2c_async_next()`の追加後、GCCの警告即エラー構築も確認した。

async forランタイムAPI統合後もClang/GCC双方でコンパイラ成果物を構築可能であることを確認した。

最新チェックポイントで、非同期反復APIを含むコンパイラ基線が利用可能であることを確認した。

async state machine生成部を再確認し、現在の複合await初期経路とasync for lowering未統合状態を検証した。Clang C11警告即エラー構築は維持されている。

async関数内の`AST_ASYNC_FOR`がstate machineのdefault節へ落ちて黙って成功する経路を検出し、codegen時の明示診断へ変更した。ClangのC01–C339差分回帰は修正後も全件一致した。

非同期反復APIとasync for明示診断の追加後も、既存のasync generator CPython差分スモークテストは一致した。

## 実装確定結果

`async for`は`__aiter__`結果の一回評価、`__anext__`待機、`StopAsyncIteration`捕捉、通常終了時のelse、break、continueをgenerator state machineへloweringする。同期的な`__anext__`値または例外は完了済みcoroutineへ正規化し、generatorの遅延例外状態を通して親へ伝播する。この例外状態、cell値、closure環境はGCトラバーサルに含まれる。

自由変数captureは`OBJ_CELL`とclosure環境辞書で実装した。外側関数はdirect child nested functionのnonlocal対象にcellを所有し、読取りは`p2c_cell_get()`、nonlocal代入および複合代入は`p2c_cell_set()`を使用する。複数closureは同じ外側cellを共有する。lambdaもC11 closure entryと環境辞書を使用し、GNU nested functionに依存しない。

awaitは代入右辺にある単一awaitについて、算術、単項、単一比較、二項短絡論理の再開生成を対応する。複数await、副作用順を保持する必要がある複合式、条件・while・async for body/elseをまたぐawaitは後続対象である。

C340–C347を追加し、比較・短絡論理内await、async for自然終了else、break/continue、nonlocal共有cell、lambda captureをCPython差分で検証した。C01–C347の347件はClang/GCCで一致し、両コンパイラの`full-build`および`test`、最新単一ヘッダーのHosted・厳格C11・freestanding検証を通過した。
