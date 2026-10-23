# 2026-08-27 — SMP GUI・Google入力・性能改善の調査記録

## 現時点で確認した事実

`src/kernel/smp.c` のAPは一般スケジューラを独立実行していない。APは `smp_ap_entry()` のwork queue/steal loopで短いkernel work itemを実行し、timer/scheduler IRQはBSP専属である。したがって、一般GUI threadやNetSurf/QuickJSをAPへ自由に移動させる実装ではない。

一方で `src/gui/core/output/gui_renderer.c` には既存の限定的なSMP利用がある。`gui_renderer_fill_rect_parallel()` は、16384 pixel以上のfill rectをonline CPU数（最大8）に応じた非重複horizontal stripesへ分け、BSPがtile 0、AP background jobsが残りを描く。AP work failure時はBSPへ安全にfallbackし、join後にBSPがcombined dirty regionをmarkする。このため、大きな単色fillについてはSMP2=2/SMP4=4/SMP8=8 stripeを使える設計である。

しかし、通常のdesktop/window/NetSurf plotterは `gui_render_engine.c` → `vga_fill_rect` / `vga_draw_string` 等を直接通る。レンダリングengineのclip state `g_clip`、rendererのclip/dirty state、windowの描画順、NetSurf callbackは単一GUI owner前提であり、画面全体をtileとしてAPに分割した実装ではない。よって「GUI全体がSMP数に応じてタイル分割済み」とは現時点で言えない。

`gui_renderer_present()` は既にdirty unionを使い、dirty rectangleだけをVGA backbufferに`vga_copy_rect_strided()`し、同矩形だけVRAMへ`vga_flip_rect()`する。次draw surfaceへの同期はrowごとのgeneric `memcpy`である。`vga_copy_rect_strided()` は `gfx_blit()`経由であり、SIMD活用箇所としてはpresent/Backbuffer copyとgfx surface copyが第一候補である。

## 安全な実装方針

1. GUI owner/BSPだけがNetSurf DOM、window list、clip state、dirty region、draw command build、frame flipを扱う。
2. draw command build後に、並列可能な非重複pixel region（full clear、large opaque fills、software surface copy、image conversion）だけをtile jobへ変換する。
3. APはBSP-owned metadataへ書かず、自身に割り当てられたbackbuffer regionだけを書き込む。BSPは全job completionをjoinしてからdirty unionとpresentを一回だけ行う。
4. AVX2はCPUID leaf 7 + OSXSAVE/XCR0を満たす場合だけ使用し、x86_64必須のSSE2または`uint64_t`copyにfallbackする。TCG/QEMUまたは未保存YMM contextではAVX2を発行しない。
5. Google検索20秒目標はDNS、TLS、HTTP response、script execution、DOM-to-box、first paint、form submit、result first paintの時刻を別々に出し、最遅stageを根拠に改善する。

## 直近の阻害要因

前回のQMP input injectionでは`mouse_set`をQMP commandとして送ったが、QEMU 8.2は`CommandNotFound`を返した。そのため相対mouse eventはcurrent PS/2 mouseへ送られ、EHCIのQEMU HID Mouseに届かなかった。QMP schema/HMP互換の正しいHID device選択方法を確定し、QMP errorを成功扱いしない注入harnessへ修正する必要がある。

GoogleのSecure Boot SMP4初期表示は起動中で、AP online/Google rendering/fatal exceptionのログ回収を継続する。

## 2026-08-27 — SMP4 strict Secure Boot実機回帰とtile present実装

SMP4 strict QEMUのSecure Boot UEFI起動で、ACPI/CPUIDはpossible CPUs=4を検出し、AP 1〜3を起動、各APのkernel workload completion、`Online CPUs=4 (AP per-CPU kernel work loops active)`を記録した。Google外部HTTPSページもDOM-to-box、QuickJS page script、HTTP/2、Google logo image、描画までfatal exceptionなく到達した。

実desktopの最終presentは`gui_renderer.c`ではなく、GUI ownerの`gui_lifecycle.c`が`gui_draw()`後に呼ぶ`vga_flip()`であることを確認した。そこで`vga_flip()`およびlarge dirty `vga_flip_rect()`の32bpp same-format BitBltを、online CPU数に一致する非重複gridへ分割する`vga_copy_rect_tiled()`を追加した。gridはSMP2=1x2、SMP4=2x2、SMP6=2x3、SMP8=2x4であり、BSPがtile0、AP background jobsが残りを担当する。BSPは全jobをjoinした後にframe boundaryへ戻る。window order/DOM/NetSurf callbacks/clip/dirty metadataは引き続きBSP GUI ownerのみが扱うため、描画順のraceを導入しない。queue submit失敗はBSPによる当該tile copyへfallbackする。

修正版はSecure Boot SMP4でビルド・起動し、Google画面を`validation/secureboot_smp4_google_tiled_present.png`として保存した。表示面のtile境界破綻やfatal faultは観測されなかった。ただし本時点ではtile jobの一回限りのserial telemetryを未追加であり、4 tileが実際にAPへdispatchされた回数のログは次の回帰で追加・確認する。

QMP mouseについては、直接`mouse_set`をQMP commandとして送った場合は`CommandNotFound`だった。`human-monitor-command`経由の`mouse_set 4`でQEMU HID Mouseをcurrentへ選択すること自体は成功した。しかしTinyUSBログにはKeyboardのみがmountしMouseはmountしない。QEMUの`usb-mouse`はEHCI上の別root portとなり、C-OS TinyUSB EHCI HCDが一root device前提のため第二root deviceを列挙していない。`usb-hub`はQEMUでfull-speedのみでEHCI直結にspeed mismatch、TinyUSB treeにはUHCI HCDが同梱されていない。mouseの`usb_version=2` propertyはQEMUで受理されるが、HCDのsingle-root-port制約を解消しない。よって現時点でGoogle formへのUSB mouse clickを成功と主張しない。UHCI companion HCDまたはhigh-speed composite/HID topologyを実装・検証する必要がある。
