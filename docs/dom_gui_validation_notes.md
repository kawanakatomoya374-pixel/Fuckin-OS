# DOM GUI validation notes

## Verified on the prior single-core QEMU run

A NetSurf `data:` control page executed through the active NetSurf → QuickJS backend. Its script called `document.getElementById`, `document.createElement`, `classList.add`, `setAttribute`, `hasAttribute`, `textContent`, `appendChild`, `firstChild`, and `parentNode`. Serial output was:

```text
[JS/console.log] first true true
[NetSurf/QuickJS] page script complete
```

The captured GUI frame visibly rendered the original `seed` followed by the dynamically inserted `DOM-TREE-OK`, proving that those calls mutated real libdom nodes and reached the NetSurf box/render pipeline.

## Changes awaiting the next GUI run

The latest ISO adds `removeAttribute`, `replaceChild`, `cloneNode`, snapshot child/element collections, sibling accessors, `classList.toggle`, and a deferred `cos_netsurf_browser_notify_dom_mutation()` hook. The hook sets one browser reformat warmup pass and asks the GUI owner for redraw after real libdom mutations, avoiding an unsafe re-entrant reformat during script execution.

## Test-infrastructure observation

The fresh single-core QEMU instance reached the desktop and scheduler, but QMP Ctrl+B and the attempted relative-pointer taskbar click did not open NetSurf. The source confirms the global browser command is handled in `gui_input.c` around line 1477 and opens `WIN_BROWSER` when its internal keyboard event has `peek.ctrl` plus B. Before drawing conclusions about the latest reformat hook, use the exact input queue semantics or a reliable desktop icon/dock coordinate sequence to open the browser.

## SMP observation

The most recent SMP-8 boot test stalls while waiting for AP1 after `Starting AP 1`. This is independent of the single-core DOM validation and remains a blocking SMP bootstrap issue; do not claim the new AP work-stealing implementation or 512KiB AP stacks are runtime-validated yet.

## Remaining DOM work

Node-specific event dispatch, complete selector support, standards-compliant HTML fragment parsing/serialization, live collections, document.cookie, and browser-window mutation invalidation coverage remain incomplete and require targeted implementation and GUI tests.

## Event delivery implementation status

The current ISO has an Element-level `addEventListener(type, callback)` implementation. It stores up to 128 callbacks per event type on the stable, context-scoped JS wrapper for each real libdom node. NetSurf's `js_fire_event()` now passes its real target node across the QuickJS bridge; `cos_js_dispatch_dom_event()` wraps that target, invokes its registered listeners with `{ type, target, currentTarget }`, logs callback exceptions without crashing the browser, and then retains the existing page-level lifecycle dispatch.

This build has compile/link validation only. It still needs GUI tests for click/change/load targets, listener removal, capture/bubble order, `preventDefault`, and page teardown behavior before being represented as complete DOM Events support.

Latest successful build artifact: `C-OS_4.0.8_alpha.iso` generated after the target-listener dispatch integration.

The event implementation was subsequently hardened so missing event arrays do not perform property access on `undefined`, and `removeEventListener(type, callback)` nulls the matching stable callback slot. This revision compiled, linked, and produced the current ISO at 10:41. Runtime GUI verification remains outstanding.

## Click event GUI result (2026-08-25)

NetSurf GUI click delivery was verified down to `fire_generic_dom_event('click')`. The QuickJS bridge now receives that real event through one authoritative ingress and walks its ancestors. A `BODY` listener registered from a `data:` page executed in QEMU: serial output showed `dispatch callbacks=1` and `[JS/console.log] click`; its callback replaced `document.body.innerHTML` with `EVENT-OK`.

The first test attached a listener to a rendered `BUTTON`. NetSurf's current hit-test path reported the BODY as the click target and its ancestor chain was BODY → HTML → document, rather than the rendered button. This is a separate hit-testing/box-to-DOM-node mapping defect. It prevents element-specific pointer events from being considered complete and must be corrected before claiming full DOM Events support.

The latest source removes a duplicate native-libdom listener path which had delivered the BODY test callback three times. The current ISO uses the single NetSurf generic DOM dispatch ingress.

## Deferred DOM rebox investigation (2026-08-25)

The element-targeting defect was fixed by correcting GUI-to-document input coordinates. A screen click at the visible button now reaches the actual `BUTTON` box, and the bound QuickJS listener dispatches exactly once. The generic NetSurf DOM dispatcher is now the sole bridge ingress.

A deferred full-rebox path was added because `browser_window_reformat()` only relayouts an existing box tree. The path waits until script dispatch has unwound, clears old box/form/object/image-map state and retained libcss node data, recreates the CSS selection context, and queues a fresh `dom_to_box`. It also blocks redraw while `layout == NULL` and advances the frontend paint generation around the transition. The latest single-core QEMU run logs `Queued DOM mutation box rebuild` followed by `DOM to box conversion complete` with no kernel assertion.

## DOM mutation/rebox GUI result (2026-08-25, latest ISO)

The earlier visual-output blocker is resolved. A single-core QEMU run loaded a real NetSurf `data:` control page containing a rendered `BUTTON` and a QuickJS `addEventListener('click', ...)` callback. A physical QMP mouse click reached the real `BODY` target, invoked exactly one bound callback, and ran `document.body.innerHTML = '<p>OK</p>'` against the live libdom document. Serial evidence recorded `Queued DOM mutation box rebuild`, followed by `DOM to box conversion complete` and `[NSDRAW] DOM-rebox redraw result=1 plots rect/text/bitmap=3/1/0`. The captured 1024×768 QEMU GUI frame visibly shows `OK` in the NetSurf viewport; the former button is gone.

The rebox path now clears old CSS node data, layout-owned box/form/object/image-map state, and recreates the CSS selection context before constructing new boxes. Its completion branch intentionally **does not** run the initial-document lifecycle: the parser had already been destroyed after the first conversion, so repeating `dom_hubbub_parser_destroy()` caused a reproducible #GP. Completion is instead delivered to the GUI as a deferred edge, which invalidates its BitBlt cache before the next redraw. This preserves browser-window ownership and avoids re-entrant rendering in the NetSurf conversion callback.

This validates one concrete end-to-end mutation path—element event → QuickJS callback → real libdom structural mutation → new DOM-to-box tree → plotter → visible GUI.

### Timer-driven mutation GUI result

A separate real GUI page used `setTimeout(function(){ document.getElementById('o').textContent = 'TIMER-OK' }, 50)`. The page script executed, the delayed callback queued a new DOM box rebuild, and the serial log recorded `[NSDRAW] DOM-rebox redraw result=1 plots rect/text/bitmap=3/1/0` without an exception. The subsequent QEMU screen capture visibly displays `TIMER-OK` in the NetSurf viewport. Therefore the deferred mutation/rebox path is now verified for both pointer-event and timer callbacks.

### `removeEventListener` GUI result

A real GUI test attached a callback to the actual HTML click target, removed that exact callback, then installed a second listener which replaced `document.body.innerHTML` with the first callback's side-effect counter. The real click emitted `dispatch callbacks=2` because the bridge preserves the removed slot for stable listener-array indexing, but only the surviving callback executed; the captured QEMU frame visibly displays `0`, not the removed callback's sentinel value `99`. This verifies that `removeEventListener(type, callback)` prevents the removed callback's execution on the practical event path.

### Capture event object GUI result

A short real GUI page attached a capture listener to the HTML ancestor and clicked the body-region target. Its callback rendered `eventPhase + ',' + (event.currentTarget === html)`; the captured frame visibly displays `1,true`. This confirms that the new dispatcher supplies an ancestor capture phase and updates `currentTarget` to the listener owner rather than leaving it fixed at the original event target.

### `stopPropagation` GUI result

A real page registered an HTML capture listener which called `event.stopPropagation()` and rendered `STOP`, plus a body listener which would render `BAD` if it received the same click. The real GUI click produced a clean rebox and the captured QEMU frame visibly displays `STOP`; `BAD` did not run. This verifies that propagation stopping from an ancestor capture listener prevents delivery to downstream target/bubble listeners in the implemented event path.

### `once` GUI result

A real HTML-ancestor listener was registered with `{ once: true }`; each invocation increments a counter and writes it through `innerHTML`. Two successive physical QEMU clicks were delivered at the same page coordinate. The captured GUI frame visibly shows `1`, confirming that the listener executed for the first click only and was removed before the second.

### `stopImmediatePropagation` GUI result

A real HTML element registered two same-phase listeners. The first called `stopImmediatePropagation()` and rendered `IMMEDIATE`; the second would render `BAD`. After a physical QEMU click, the captured frame visibly displays `IMMEDIATE`, establishing that the same-element listener loop stops before the later callback.

### Passive listener GUI result

A real HTML listener was registered with `{ passive: true }`. It called `preventDefault()` and then rendered `event.defaultPrevented`. The captured QEMU frame visibly shows `false`, confirming that the bridge retains the passive-listener restriction while preserving a usable synchronous Event object.

Full cross-element capture/bubble ordering, DOM default-action cancellation integration, standard fragment parsing/serialization, and broad selector compatibility still remain required before claiming complete DOM support.

## UEFI SMP8 regression — 2026-08-25

標準 OVMF（`OVMF_CODE_4M.fd`）と q35 / 1024 MiB / `-smp 8` の実行で、UEFI 固有の ACPI テーブル参照ページフォールトを解消した。原因は `paging_map_kernel_higher()` が物理アロケータの type-1 available 容量（1017 MiB）だけを direct map していた一方、UEFI の ACPI reclaimable 領域が `0x3FB6D000` 付近に置かれ、`PHYS_TO_VIRT()` の参照が未マップとなったことである。Multiboot 報告の物理 high-water extent に direct map の上限を変更後、UEFI 上で AP1〜AP7 の C workload 完了、`Online CPUs=8`、GUI デスクトップ、E1000 GUI handoff、ACPICA 初期化まで到達した。

同一の UEFI SMP8 GUI で、`<button id=b>GO</button>` に QuickJS `addEventListener('click', ...)` を登録し、コールバック内で `document.body.innerHTML='<p>OK</p>'` を実行する data URL を読み込んだ。実画面上に `GO` ボタンが描画されたことをフレームキャプチャで確認後、その実ボタンを PS/2 マウスで押下・解放した。シリアルログでは `click-hit ... <BUTTON>`、`fire_generic_dom_event: Dispatching 'click'`、`Queued DOM mutation box rebuild`、`DOM-rebox redraw result=1` を連続して確認した。これは API の存在確認ではなく、GUI入力から実libdomイベント、QuickJS callback、DOM完全rebox、NetSurf plotter、GUI再描画までの実行経路を確認する証拠である。

| ファイル | 内容 |
|---|---|
| `artifacts/uefi_smp8_dom_before_click.png` | UEFI SMP8上で描画された `GO` ボタン |
| `artifacts/uefi_smp8_dom_click_success.png` | click callback後の `OK` DOM置換表示 |

### UEFI SMP8 timer GUI result

同じ標準 OVMF / q35 / SMP8 実行で `setTimeout(function(){ document.getElementById('o').textContent='TIMER-OK' }, 50)` を実行した。ログには timer callback 後の `Queued DOM mutation box rebuild` と `DOM-rebox redraw result=1` があり、`artifacts/uefi_smp8_timer_success.png` の実GUIは `TIMER-OK` を表示した。したがって、UEFI SMP8経路でも非同期QuickJS callbackから実libdom mutation、NetSurf再box化、描画までの経路が動作している。

### Google actual-site preparation

UEFI SMP8の実NetSurfから `https://www.google.com/` を直接取得し、DNS、TLS接続、HTTP 200、86,705 bytesのHTML受信、HTMLのDOM-to-box完了、GoogleロゴPNGの実サブリソース取得まで到達した。レスポンスのSet-Cookie由来の状態は後続のGoogleサブリソース要求でCookieヘッダとして送信されており、具体的なCookie値はログ・文書には保存しない。次に、実画面で同意UIの有無を判定し、必要な場合は同意操作と検索フォーム送信を確認する。

### Google actual search result — UEFI SMP8

Googleホームの実HTMLフォームをC-OSのPS/2マウスでフォーカスし、QMP経由で `C-OS NetSurf` を入力してEnter送信した。NetSurfはGoogleが生成した検索URLを実際にナビゲートし、`GET /search?...q=C-OS+NetSurf...`、HTTPS接続、HTTP 200、検索結果HTMLのDOM-to-box完了をログで確認した。`artifacts/uefi_smp8_google_search_results.png` は同検索URLの結果ページを描画している。現時点ではGoogleが返す複雑な結果カードの大半が表示されず、文字の一部はCJK/アラビア系グリフ不足により `?` となる。このため、実検索リクエストと結果HTMLの受信・レイアウトは確認できたが、Google検索結果の完全な表示互換性は未達成である。

Googleホームでは同意バナーが自動表示されなかったため、同意UIを強制表示するためのConsentエンドポイント検証を継続している。Cookie値・トークン値は保存しない。

### Google Cookie consent UI — UEFI SMP8

Googleの有効なdesktop同意フロー `https://consent.google.com/d?...&m=0&pc=srp&src=1` をC-OS実NetSurfで開いた。ブラウザは実際に同一オリジンの `/dl?...&escs=...` へのHTTPリダイレクトを追跡し、最終HTMLをHTTP 200で受信してDOM-to-box完了まで到達した。`artifacts/uefi_smp8_google_consent_ui.png` は「Personalization settings & cookies」とGoogleのCookie利用説明を実描画している。

同意操作のため、QEMU HMPの `mouse_move dx dy dz` によるPS/2ホイール注入を使用し、`dz=-8` をC-OSブラウザへ届けた。この操作でNetSurfの実ドキュメントスクロールと、画面下部にある同意アクションの表示を検証中である。Cookieの具体値およびGoogle発行の一時遷移トークンは保存しない。

Google同意UIではQMPのwheelイベントが現行QEMU PS/2入力経路でC-OSの`mouse.wheel`へ届かず、画面スクロールは変化しなかった。一方、実キーボードのEndキー注入はNetSurf文書を末尾までスクロールし、`Confirm your settings` と `Reject all` の実フォームボタンを表示した。これはキーボード文書スクロールが実GUIで動作する証拠であり、PS/2 wheel注入の互換性は別途修正対象として残す。

### Google Cookie consent POST — UEFI SMP8, verified

古い実装ではGoogleの`Confirm your settings`フォームPOSTがstale Keep-Alive TLS接続で失敗した。HTTP共通経路を修正し、POSTは再送せず常にfresh transportから開始するようにした。更新ISOをクリーンなOVMF変数でUEFI SMP8起動し直した後、実Google同意ページの`Confirm your settings`ボタンを物理QMPマウスでクリックした。

実行ログは`POST /save`、HTTP `303`、`https://www.google.com/`へのリダイレクト、最終HTMLのHTTP `200`を順に確認している。以後のGoogleリクエストには同意状態を表すCookieが送られるが、Cookie値は保存しない。`artifacts/uefi_smp8_google_consent_success.png`は同意UIが消えGoogleホーム本文が表示された実画面である。

この実験で、内部HTTPクライアントがリダイレクト後の最終originをNetSurf content URLへ反映しない不具合も発見した。`/save`応答本文はGoogleホームであるのにcontent URLは`consent.google.com/save`のままとなり、その直後にホーム内の相対`/search`を使うと誤って`consent.google.com/search`へ解決される。これは今回の同意POST成功とは独立したURL正規化/リダイレクト伝播の残課題であり、別途修正対象とする。

### Google actual search after confirmed consent — UEFI SMP8

同意成功後、Googleホームを明示的に再読込してから実検索フォームへ `C-OS NetSurf` を入力し、実際に送信した。`www.google.com/search?...q=C-OS+NetSurf...` に対するHTTPS GETはHTTP 200、92,818 bytes、HTMLのDOM-to-box完了まで確認した。`artifacts/uefi_smp8_google_postconsent_search.png` はこの最終検索結果URLの実GUIフレームである。

この検証は、Google Cookie設定の実フォームPOST・サーバー受理・同意状態の後続送信・検索フォーム入力・検索GET・検索結果HTML受信・NetSurfレイアウト・C-OS GUI描画を連結して確認したものとなる。ただし、Googleの現行結果ページに多いJavaScript生成カードは本ポートのDOM/JS/CSS互換性範囲外であり、検索結果の大半はまだ完全に可視化できない。また、非ASCIIグリフ不足により一部の文字は`?`として描画される。したがって、実検索トランザクションは成功したが、Google検索の完全な視覚互換性は今後の実装課題である。

### Redirect final-origin propagation regression — UEFI SMP8

内部HTTPリダイレクトの最終URLをHTTPクライアントから取得し、元のNetSurf request URLと異なるHTMLに安全にエスケープした`<base href="final-url">`を先頭注入する修正を加えた。クリーンなOVMF/SMP8実行でGoogle同意フローを再試験したところ、`/save` POSTはHTTP 303で`https://www.google.com/`へ遷移し、fetcherログは`Redirect final base URL applied: https://www.google.com/`を記録した。

そのまま表示されたGoogleホームの相対検索フォームを実クリック・送信すると、以前の誤った`consent.google.com/search`ではなく、`https://www.google.com/search?...q=C-OS+NetSurf...`へ解決され、HTTP 200とDOM-to-box完了を確認した。`artifacts/uefi_smp8_google_search_redirectbase_fixed.png`が最終GUIキャプチャである。

### UEFI Secure Boot — owned development key, firmware-enforced, SMP8

Secure Boot専用のstandalone GRUBを生成し、`grub_secure.cfg`と正確な`kernel.elf`を同一PE/COFFイメージ内へ内包した。`BOOTX64.EFI`を外部（ソースツリー外）の所有開発鍵で署名し、FAT ESPの`EFI/BOOT/BOOTX64.EFI`とUEFI-first El Torito ISOを生成した。private keyは`~/.local/share/c-os-secureboot/`（directory 0700 / key 0600）だけに保持し、ESP/ISOにはpublic DER certificateだけを含める。

`virt-fw-vars`で所有証明書をPK・KEK・dbへ登録しSecureBootEnableをONにしたOVMF varsを作成した。OVMF_CODE_4M.secboot、SMM有効Q35、SMP8、署名済みUEFI-first ISOの実行はC-OS kernel、AP1〜AP7 workload、Online CPUs=8、GUI desktopまで到達した。

同じSecure Boot varsで未署名の従来hybrid ISOを起動した負検証では、kernel serialは一切開始せず、OVMF画面に `BdsDxe: failed to load ...: Access Denied` と表示された。`artifacts/uefi_secureboot_unsigned_rejected.png`がこのfirmware強制拒否の証拠である。従ってfirmwareが単にEFIファイルの存在を見るのではなく、dbに信頼された署名を要求していることを確認した。

### EHCI/TinyUSB runtime investigation — UEFI SMP8

Q35へ`ich9-usb-ehci1`とQEMU USB Mouseを接続したUEFI/SMP8試験では、C-OSがEHCI PCI controller（MMIO `0x810A1000`）を検出しTinyUSB hostを初期化した。TinyUSB EHCIの256-entry frame-list設定ではQEMUがcontroller resetを報告したため、standard EHCIの互換性基準である1024-entry frame listへ変更し、USBCMD frame-list fieldをORではなく明示設定にした。さらにPC EHCIの`CONFIGFLAG=1`、起動時接続済みroot-portの状態同期、`tuh_task_ext(0, false)`によるnon-blocking host event drainを実装した。更新後はQEMUのEHCI resetエラーが消え、`[USB] TinyUSB non-blocking service loop active.`も確認している。

ただし、QEMU上で`PORTSC1=0x1003`（pre-connected）またはQMP hotplug後のUSB Mouseが存在する状態でも、TinyUSB HID mount callbackの`[USB] Mouse connected.`にはまだ到達していない。このため**EHCI controller初期化とnon-blocking service loopは実証済みだが、TinyUSB HIDの完全列挙・入力は未完了**として扱う。USB HIDを完全接続済みとは主張しない。

### QuickJS fetch/XMLHttpRequest implementation boundary

`cos_js_web_api.c`には実際の`fetch`、`XMLHttpRequest`、`localStorage`、`sessionStorage` bridgeが存在し、HTTP(S) URLをorigin/page URLから解決し、Promise/XHR readyState completionへ接続している。これはAPI名だけのno-opではない。

ただし`cos_js_pump_web_requests()`はGUI owner passごとに1件だけ取り出すものの、その内部で`http_get()`または`http_post()`を同期実行する。したがって現時点ではrequest-local nonblocking transportでも並列HTTPでもなく、GUIスレッドを長時間占有しうる。この範囲は「bounded queue + real completion callback」までが実装済みであり、真の非同期XHR/fetch、複数接続、HTTP/2/Brotliは未完了として扱う。

### HTTP request-local boundary regression (UEFI SMP8)

After removing the mutable HTTP `http_instance` association, adding the explicit `http_store_cookie_header_for(client, value)` path, and serializing the still-shared TCP/TLS/receive-workspace transport with a correctness gate, the incremental build and signed UEFI artifact passed static signature verification.

A fresh OVMF UEFI SMP8 run reached `CPU parallel workload complete: 1` through `7`, `Online CPUs=8`, E1000/DHCP, GUI arrival, and the NetSurf asynchronous HTTP worker. C-OS NetSurf then connected to `https://www.google.com/` through TLS, received HTTP 200 with 86,718 bytes, and created the real NetSurf content object. The QEMU state remained `running` during the test.

This confirms no regression in UEFI SMP8 or real Google HTTPS reachability. It does not yet prove that QuickJS `fetch`/XHR is nonblocking: `cos_js_web_api.c` still invokes synchronous `http_get()`/`http_post()` from its owner pump. True request-local asynchronous transport and parallel HTTP remain open work.

### QuickJS fetch asynchronous completion regression

The first workerized fetch test reached `[QJS/Web] fetch() fulfilled` but its `.then()` callback was not visible because QuickJS pending Promise jobs were not being executed by the GUI lifecycle; the screen remained blank after the script had run.

The fix adds `cos_js_pump_pending_jobs()` using `JS_ExecutePendingJob()` with an eight-job per-frame bound, called after the Web API and timer pumps and before composition. On a fresh UEFI SMP8 run, a real page script called `fetch('https://www.google.com/')`, produced `fetch() called`, `fetch() queued`, worker dispatch, HTTP response 200 (86,795 bytes), and `fetch() fulfilled`. The same run emitted `DOM-rebox redraw result=1 plots rect/text/bitmap=3/1/0`. The final QEMU capture visibly displayed `FETCH-OK:85412`, proving the asynchronous HTTP completion, Promise `.then()`, real libdom mutation, DOM rebox, and pixel rendering chain.

The response length displayed by JavaScript is the decoded response body length, while the transport log reports the wire/client body span; both are nonzero and the request was to the real Google HTTPS endpoint. The test ran with UEFI and all 8 CPUs online.

### QuickJS XMLHttpRequest constructor and async regression

The first XHR GUI test exposed a real compatibility defect: `new XMLHttpRequest()` raised `TypeError: not a constructor`. The global factory was then registered with `JS_NewCFunction2(..., JS_CFUNC_constructor_or_func, ...)`.

On the next fresh UEFI SMP8 run, a page created `new XMLHttpRequest()`, called `open('GET', 'https://www.google.com/', true)` and `send()`. The script completed without exception. The request reached Google HTTPS and returned HTTP 200 with a nonzero response body; the serial log emitted `DOM-rebox redraw result=1 plots rect/text/bitmap=3/1/0`. The final QEMU capture visibly displayed `XHR-OK:200:85364`, proving constructor use, asynchronous transport completion, `onload`, real DOM mutation, rebox, and pixels. The test ran with all 8 CPUs online and no page-fault markers.

### Incremental build default-target correction

The Makefile now declares `.DEFAULT_GOAL := iso` before the MicroPython makefile includes. This prevents the included MicroPython rules from selecting an unrelated first object as the default goal. A plain `make -j2` now reaches the normal C-OS kernel/ISO dependency graph, reuses an up-to-date kernel when appropriate, creates the hybrid BIOS+UEFI ISO, and verifies the UEFI El Torito entry without requiring a clean build.

### EHCI enumeration follow-up — SMP8

The root-port duplicate-reset path was removed: TinyUSB's host enumeration state machine now exclusively owns the 50 ms root reset sequence after `HCD_EVENT_DEVICE_ATTACH`. Completion events from the C-OS cooperative EHCI poller are also submitted to the OSAL queue as non-ISR events, matching the actual execution context.

A diagnostic SMP8 q35 run reached root attach, submitted the first dev-0 SETUP request, and completed it successfully (`bytes=8`). The event was enqueued, but no subsequent event-consumer or next control-stage transfer was observed during the run. Therefore **EHCI controller startup, root attach, first SETUP completion, and non-blocking polling are verified; full TinyUSB HID enumeration and mount callback remain incomplete**. The next investigation must focus on the C-OS TinyUSB event-consumer/task handoff after the first control transfer, not on root-port detection.


## Standalone QEMU launcher and boot artifact validation — 2026-08-26

A repository-relative launcher was added at `tools/run_qemu_c-os.sh`. It accepts `--cpus 1..8`, defaults to UEFI, and selects the matching ISO plus a disposable copy of the required OVMF variable store. `--secure-boot` selects the signed UEFI-only ISO and the OVMF variable store containing the C-OS development certificate. It can also attach the E1000 user-mode network and EHCI USB mouse without callers needing to reconstruct bus names or pflash options.

The normal hybrid ISO (`C-OS_4.0.8_alpha.iso`) has both BIOS and UEFI El Torito images. The signed artifact (`build/secureboot/C-OS_4.0.8_alpha_secure.iso`) intentionally has only a UEFI El Torito ESP and must not be used in a BIOS-only VM. Selecting the signed ISO without OVMF is therefore a likely cause of an apparent boot failure.

| Configuration | Result | Evidence |
|---|---|---|
| UEFI, SMP1, normal ISO | Passed | `Online CPUs=1`, GUI initialization, and `Entering GUI main loop` in `/tmp/cos_uefi_smp1.serial` |
| UEFI Secure Boot, SMP1, signed ISO with enrolled variables | Passed | `Online CPUs=1`, GUI initialization, and `Entering GUI main loop` in `/tmp/cos_secure_smp1.serial` |
| UEFI Secure Boot, SMP8, signed ISO with enrolled variables | Passed | AP1–AP7 workload completion, `Online CPUs=8`, GUI initialization, and `Entering GUI main loop` in `/tmp/cos_secure_smp8.serial` |

The signed `BOOTX64.EFI` is Authenticode-signed by the C-OS Development Secure Boot certificate. The Secure Boot manifest records `sbverify` output and SHA-256 evidence. The launcher copies the variable store to `/tmp`, so a test run cannot overwrite the enrollment state.
