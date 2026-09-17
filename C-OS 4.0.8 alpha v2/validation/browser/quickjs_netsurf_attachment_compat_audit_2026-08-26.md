# 添付HTMLのQuickJS / NetSurf互換性監査

## 対象

| ファイル | 主目的 | 実行上の重要点 |
|---|---|---|
| `index.html` | 二つの数値入力と加算ボタン | `HTMLInputElement.value`、GUI由来のclick、`textContent`更新 |
| `index(1).html` | テキスト/JSON整形ツール | `textarea.value`、input/click listener、`innerHTML` fragment、class/style更新、正規表現・JSON |

## JavaScript言語機能

両ファイルはIIFE、`var`、関数、配列、文字列、正規表現、例外、`JSON.parse`/`JSON.stringify`、`parseFloat`、`isNaN`を用いる。これらはQuickJSコアの標準ECMAScript機能であり、C-OS独自の言語拡張を必要としない。

## DOM / Web APIの照合

| API | 添付HTMLでの利用 | 現行bridge | 対応方針 |
|---|---|---|---|
| `document.getElementById` | 全てのcontrol取得 | real libdom wrapper | 実機検証対象 |
| `element.value` | input/textarea読書き | real input/textarea accessor | GUI入力との同期を確認 |
| `addEventListener` | `click`/`input` listener | libdom listener + QuickJS dispatch | GUI click/keyイベントを検証 |
| `textContent` | 結果表示 | real DOM mutation | DOM変更後のredrawを検証 |
| `innerHTML` | Markdown結果/JSON強調 | bounded fragment parser | span、style、classのfragment適用を検証 |
| `classList` / `style` | 状態と配色の変更 | bridge実装済み | CSSOM反映とdirty rebuildを確認 |
| `JSON` / RegExp | JSON formatter | QuickJS core | 実行時smokeへ含める |

## CSS / 表示上の制約

添付HTMLにはcustom properties、CSS Grid、Flexbox、media query、box-shadow、`rem`、`max-width`が含まれる。QuickJS実行の必要条件ではないが、現行C-OS NetSurf portのTier-B box/layoutは完全なブラウザ実装ではないため、ピクセル単位のChrome/Firefox互換レイアウトは保証できない。今回の実装では、local `file://` fetcherでHTML/CSS/scriptを本物のNetSurf content lifecycleへ入れ、DOMイベントと表示更新を優先する。

## 実装優先順位

1. local `file://` fetcherを通じて添付HTMLをNetSurf/QuickJSで読み込む。
2. GUIのボタンclick・input編集がlibdom/QuickJS listenerへ届くことを確認する。
3. `innerHTML`によるspan/class/style fragmentと結果DOMのdirty rebuildを確認する。
4. その後、CSS layout範囲を段階的に拡張する。

## 外部Webページ回帰候補

MDNの公開 **Web Storage API example**（`https://mdn.github.io/dom-examples/web-storage/`）を外部ページ回帰候補として確認した。このページはテキストinput、`select`、変更イベント、`localStorage`、画像/CSSの動的な反映を利用する。これは添付ページ固有でない、一般的なブラウザ互換性の確認対象として採用する。

次の実装では、selector複数指定、`Element.click()`、`disabled`属性反映、GUI由来click/key/inputイベント、input/selectのvalue同期、DOM mutation後の再描画を汎用機能として扱う。

## ES2025計算機の追加監査

`es2025_ultra_calculator.html`の言語機能はQuickJSコアで実行可能である。generator、`let`/`const`、arrow function、template literal、spread、`Set`、`new Function`、正規表現、`Array.prototype.includes`/`map`/`filter`/`some`/`forEach`を使用する。`Set.prototype.difference`、`Object.groupBy`、`Array.prototype.toSorted`はページ側にフォールバックがある。

C-OS DOM bridge側で汎用的に補う必要がある項目は次の通りである。

| 一般的なWeb利用 | ES2025計算機での使用例 | 実装方針 |
|---|---|---|
| selector group | `.num, .op` | commaで区切るselector unionを追加 |
| programmatic activation | `runBtn.click()` | `Element.click()`を標準event dispatchへ接続 |
| boolean reflected property | `stepBtn.disabled` | `disabled`属性とJS propertyを同期 |
| window keyboard listener | `window.addEventListener('keydown', ...)` | GUI keyboardからreal window eventへdispatch |
| dynamic fragment | `innerHTML`でtoken/history/chart生成 | attribute/class/styleを含むbounded fragmentを継続強化 |
| form/select input | 外部Web Storage demo | `value`同期、change/input listener、select操作を実装・検証 |

これらは添付ページ専用の分岐ではなく、外部Webページの標準DOM API互換性として実装する。
