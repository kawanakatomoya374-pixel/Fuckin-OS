# 2026-08-27 — 汎用CSS/forms・USB HID・DOM mutation進捗

## 汎用HTML/CSS/forms回帰

添付 `index.html` をページ固有の分岐なしで再検証した。author inline stylesheetを初回DOM-to-box変換の前に通常のstylesheet fetchとして登録し、Flex special-elementのpost-conversion blockificationを一般修正した。その結果、Secure Boot UEFI・SMP2実機で中央カード、author CSSの背景/配色、`display:flex; flex-direction:column`、4個の独立item、visible text/number input、button、result領域が表示された。

証跡: `validation/secureboot_smp2_attachment_index_flex_form_blockification.png`

## EHCI / USB HID列挙

poll-driven EHCI completion scanがQH overlayだけを見て、controllerが未取得のactive qTDを完了扱いしていた。これによりGET_DESCRIPTOR data phaseが疑似完了となり、次stageがdevice STALLになっていた。`ehci.c` で実scheduled qTDの `active` をcompletion必須条件にした。

Secure Boot SMP2でGET_DESCRIPTOR setup/data/status、SET_ADDRESSを実際に通過し、`[USB] Keyboard connected.` を確認した。QEMU `info usb` では `usb-kbd` / `usb-mouse` がEHCI Port 1/2、480 Mb/sである。

## USB keyboard one-shot input

USB HID snapshot注入がPS/2用software typematicを開始し、短いHID pressでも連続文字になる回帰を修正した。`keyboard_inject_scancode()` はUSB press後にlegacy repeatを停止する。USB native auto-repeatは将来HID timingで実装する余地があるが、現時点は正確なone-shot press/releaseを優先する。

入力先inputにUSB keyboard reportが届き、NetSurf keypress処理・textarea value同期まで到達することを実画面で確認した。以前の連続 `1` は修正前typematicの証跡であり、正常値入力の成功証跡ではない。

## 残る重大回帰: post-ready DOM mutation + existing forms

button clickによりQuickJS listenerがresult `textContent` を変更した後、汎用DOM mutation reboxでpage faultを再現した。RIP `0x3A326C` は `form_gadget_sync_with_dom` (`form.c:2158`)。

`html_rebuild_layout_after_dom_mutation()` がforms/controlをfreeしてから旧layout/boxを破棄しているため、旧box側または同期DOM event側がstale `form_control` を参照している可能性が高い。これは添付固有ではなく、live DOM mutation中の既存formを含むページ一般のlifetime回帰として、旧box破棄・form free・form rediscoveryの順序を修正する必要がある。

このfaultがあるため、`validation/secureboot_smp2_attachment_index_positive_usb_hid.png` はbutton後のblank/navigating状態を示すだけであり、正の算術結果成功証跡ではない。正の結果を達成済みとは扱わない。

## 診断整理予定

root cause確定後、以下の一時診断を削減する: `[USB] EHCI dev0 ...`、HID report trace、Flex/CSS selection trace、`[NSTEXT]`。

## 関連証跡

| 内容 | パス |
|---|---|
| 添付CSS/forms表示 | `validation/secureboot_smp2_attachment_index_flex_form_blockification.png` |
| USB keyboard mount後の入力画面 | `validation/secureboot_smp2_input1_keyboard_after_ehci_fix.png` |
| button後のfault状態（成功ではない） | `validation/secureboot_smp2_attachment_index_positive_usb_hid.png` |
| QEMU上流EHCI比較用ソース | `validation/upstream_reference/tinyusb_ehci_master.c` |

## DOM mutation reboxの解消と正の算術証跡

最初のform lifetime修正では、手動 `box_free()` がtalloc destructorとCSS resultsを二重解放することを実機#GPで確認したため撤回した。最終修正は、old layoutを通常どおりtalloc contextで破棄する前に、live DOM全体から `__ns_key_box_node_data` を再帰的に解除する方式である。これにより、live DOMを再box化する際に旧box（およびfree済みのgadget control）を `box_for_node()` が再利用する問題を解消した。libcss node dataも従来どおり再帰解除し、新しいselection contextを作成する。

Secure Boot UEFI・SMP2で空フォームbutton clickの`エラー: 正しい数値を入力してください。`への更新後もDOM mutation reboxが完走し、page fault/#GPなしでフォーム表示を維持した。証跡は `validation/secureboot_smp2_attachment_index_error_node_mapping_fix.png`。

続いて同一strict QEMU実機で、EHCI USB keyboardから最初のnumber inputへ`1`、二番目へ`2`を入力し、USB mouseの分離press/releaseでbuttonをclickした。QuickJS listenerが`input.value`を読み、加算し、`textContent`を更新した後のreboxも完走し、画面に **`結果: 1 + 2 = 3`** を表示した。最終証跡は `validation/secureboot_smp2_attachment_index_positive_usb_hid_final.png`。これはC-OS FAT32 `file:` fetch → NetSurf HTML/CSS/forms → EHCI USB HID mouse/keyboard → NetSurf input/click → QuickJS listener → libdom textContent mutation → generic rebox →実画面描画の正のend-to-end証明である。

## 2026-08-27 — HTML描画・受信上限を10MiBへ拡張

従来のWikipedia専用64KiB truncationを削除し、HTTP transportの `HTTP_MAX_RESP` を `10u * 1024u * 1024u` へ更新した。これはheap上のper-document response bufferであり、NetSurfのHTML/CSS/DOM conversionへ渡るpayloadを10MiBまで保持する。ローカル `file:` fetcherも旧 `FS_MAX_DATA`（32KiB metadata/editor上限）から独立させ、同じ10MiB document safety limitへ更新した。FatFs側の `fs_read_file_at()` は既にファイルサイズに応じてheap bufferを拡張する実装であり、ブラウザ用file fetchの10MiB上限と整合する。

Secure Boot UEFI・SMP2 strict QEMUで10MiB版ISOを起動し、HTTP/file fetcher登録、FAT32上の `file:///browser/index.html` 2939 bytes読込、NetSurf DOM-to-box完了、Flex column layout、画面描画、EHCI USB keyboard mountを確認した。起動・描画ログにpage fault、#GP、panicはなく、既存のCSS/forms/QuickJS回帰を壊していない。成果物は `build/secureboot/C-OS_4.0.8_alpha_secure.iso`。

10MiBは安全上限であり、10MiBのDOMを無条件に快適処理できるという性能保証ではない。HTML payload以外にQuickJS heap、libdom node、CSS box tree、画像デコード、GUI backbufferが追加で必要となる。また、HTTP clientのwire responseは10MiBへ拡張したが、gzip/Brotli展開後のDOM負荷と複数同時転送時の総メモリ消費は別途SMP/性能回帰が必要である。

## 2026-08-27 — QuickJS 24MiB、NetSurf 128MiB、Google外部JavaScript回帰

QuickJS runtimeの `JS_SetMemoryLimit` は従来64MiBを宣言していたが、実kernel allocatorは32MiBであり上限と実体が矛盾していた。`COS_QUICKJS_RUNTIME_MEMORY_LIMIT_BYTES` を24MiBとして明示し、page scriptごとに適用した。NetSurfの`hlcache`（HTML/CSS/JS/image resource cache）limitは16MiBから128MiB、hysteresisは16MiBへ拡張した。これを実際に成立させるため、BSS-backed kernel heapを32MiBから256MiBに拡張した。これは128MiB llcache上限、24MiB QuickJS上限、HTTP staging、DOM/CSS box tree、decoded image、GUI/kernel headroomを同居させるためである。

Secure Boot UEFI・strict QEMU・SMP2で、`[MEMORY] Heap ... size=256 MiB`、llcache 134217728 bytes、`QuickJS runtime=24MiB`、FAT32 local browser pageのDOM-to-box/Flex/form render、EHCI USB keyboard mountを確認した。page fault、#GP、panic、allocator OOMは記録されなかった。

外部検証は検証用のコンパイル時開始URL（`BROWSER_SMOKE_START_URL`）を用い、C-OS実機で `https://www.google.com/` を通常のNetSurf HTTP(S) fetcher経由で読込した。TLS ALPN h2、HTTP/2 200、Google logo PNG、CSS subresource、favicon fetchを確認し、Google画面を `validation/secureboot_smp2_google_external_http2.png` として保存した。複数のGoogle inline script（486、1822、8344、1873、4077 bytes）がQuickJSで実行された。

初回はGoogleの標準bootstrapが `location.search.indexOf(...)` を読む際に `location.search` が`undefined`であり、`TypeError: cannot read property 'indexOf' of undefined`を記録した。ページ固有の分岐は追加せず、document URLから `document.URL` と`location.href/protocol/host/hostname/pathname/search/hash/origin`を同期する一般API `cos_js_set_page_location()` を導入した。修正後のGoogle Secure Boot SMP2再実行では当該例外を再現せず、すべて記録されたpage scriptが`page script complete`となり、fatal exceptionなしで描画完了した。

Google初期表示・external script実行・HTTP2 subresourceロードは実証済み。ただし、この回のGoogleフォームへのQMP USB HID入力注入はマウス座標が実検索inputへ到達せず、検索送信の実証には至っていない。外部Web Platform互換性もChrome/Firefox同等ではない。Location assignmentによるnavigation、SVG/icon rendering、完全cookie document API、全Web framework APIは後続の一般互換性作業として残る。


## 2026-08-27 — QuickJS `document.cookie` と `location.href` の実機回帰

QuickJSの静的な空文字列だった`document.cookie`を廃止し、既存HTTP/1.1・HTTP/2共通のbounded Cookie jarへ接続した。JavaScriptによるCookie assignmentは、現在コミット済みのHTTP(S) URLのhost/path/schemeから小型scopeを構築して同じjarに保存する。`Path`、`Domain`、`Secure`、`Max-Age=0`の既存jar規則に従い、subsequent fetchで同じHTTP request header生成経路を利用する。10MiB response bufferを所有する`http_client_t`をDOM accessorのスタックに生成しないよう、専用のURL scope helperを追加した。

`location.href`、`location.assign()`、`location.replace()`、`location.reload()`は、QuickJS/libdom callback中に`browser_window_navigate()`を再入呼出ししない。一件だけの正規化URLキューをGUI ownerの次のredrawで消費する方式にした。相対、root-relative、network-path、query-only、fragment-only、HTTP(S)、`file:`のURL解決を、Web APIのページURL状態から行う。実装中、`cos_js_set_page_location()`がLocation accessorの`href`へ単なる同期値を代入してsetterを発火し、同一URLの無限再読込を誘発する問題を検出した。コミットURLの同期では`href` setterを呼ばず、getterをWeb contextのlive stateに一元化して修正した。また、NetSurf fetch identityがHTTP fragmentを送信しないため、script-visible Location用にコミット済みURL表記を保持し、content URLのbase部分と一致する時だけfragment付きURLをQuickJSへ返すよう修正した。

`validation/browser/quickjs_cookie_location_selftest.html`とdestination fixtureをHTTP originで配信し、Strict QEMU（q35、TCG thread=multi、E1000、EHCI、Secure Boot UEFI、SMP2）で検証した。QEMU user-netのhost gatewayはこのprofileでは`192.168.70.2`であり、誤った`10.0.2.2`設定の接続失敗を識別して検証URLを修正した。最終runではsource scriptとdestination scriptがともにQuickJSで例外なく完了し、destination HTTP requestは一回のみであった。request headerには`Cookie: cos_cookie_test=visible`が含まれた。画面上でpath、query、fragment、origin、Cookie継続、`document.URL === location.href`の全項目がPASSとなった。

| 証跡 | 内容 |
|---|---|
| `validation/browser/secureboot_smp2_cookie_location_navigation_pass.png` | Secure Boot UEFI・SMP2上の可視的PASS画面 |
| `validation/browser/cookie_location_smp2_fragment_result_extract.txt` | source/destination QuickJS完了、Cookie header、destination fetch=1、fatal/exceptionなし |
| `validation/browser/cookie_location_smp2.serial.log` | 完全serial runtime記録 |
| `validation/browser/quickjs_cookie_location_selftest.html` | HTTP originのCookie/Location source fixture |
| `validation/browser/quickjs_cookie_location_destination.html` | 相対URL・query・fragment・Cookie継続のdestination fixture |

この範囲は**same-processのbounded session Cookie jarとscript-visible Cookie API**の実証であり、Cookie永続化、`HttpOnly`の非露出、`SameSite`、expiry日時、認証・同意画面の完全互換性は未達である。`location.replace()`は現在通常のhistory navigationと同じキューに入るため、replace固有のhistory entry置換も未達として扱う。
