; =============================================================================
; boot.asm - C-OS 4.0.6 Bootloader (Multiboot2, 32→64bit transition)
; Reconstructed from kernel.elf binary analysis
;
; 【修正箇所】
;   1. Multiboot2ヘッダーの length フィールドを 28 → 正確な値に修正
;      (ラベルで自動計算させる方式に変更)
;   2. framebuffer タグに end タグを追加（必須）
;   3. framebuffer タグの flags を 1(オプション) → 0(必須) に戻す
;      これで起動時に framebuffer が確実に要求される
; =============================================================================

[BITS 32]

; =============================================================================
; Multiboot2 ヘッダー
; 注意: GRUBはこのヘッダーを8KB以内に見つけなければならない
; =============================================================================
section .multiboot2
align 8

mb2_header_start:
    dd  0xE85250D6                              ; magic
    dd  0                                       ; arch (0 = i386 protected mode)
    dd  mb2_header_end - mb2_header_start       ; length
    dd  -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))  ; checksum

; --- framebuffer タグ (type=5) ---
align 8
    dw  5                   ; type = framebuffer request
    dw  0                   ; flags = 0 (required)
    dd  20                  ; size = 20 bytes
    dd  1024                ; width
    dd  768                 ; height
    dd  32                  ; bpp (bits per pixel)
; --- end tag (type=0) ---
align 8
    dw  0                   ; type = 0 (end tag)
    dw  0                   ; flags = 0
    dd  8                   ; size = 8
mb2_header_end:

; =============================================================================
; データ領域 (GDT・保存領域)
; 実際のリンク先アドレスをラベル参照で埋め込む
; =============================================================================
section .data
align 8

; GDT エントリ (GDT本体はこのセクションに配置)
gdt_start:
    dq  0x0000000000000000  ; [0] Null descriptor
    dq  0x00AF9A000000FFFF  ; [1] 64-bit code  (selector=0x08)
    dq  0x00CF92000000FFFF  ; [2] 32-bit data  (selector=0x10)

gdt_ptr:
    dw  0x0017              ; limit = 3 entries * 8 - 1 = 23
    dd  gdt_start           ; base  = gdt_start のリンク後アドレス

; multiboot レジスタ保存領域
align 8
mb2_magic:  dd 0            ; eax (multiboot magic) を保存
mb2_info:   dd 0            ; ebx (multiboot info ptr) を保存

; =============================================================================
; エントリポイント
; GRUBからここに制御が渡る (32bit protected mode)
; =============================================================================
section .text
global _start
_start:
    cli                             ; 割り込み無効化

    ; スタックポインタ設定 (32bit用一時スタック)
    mov     esp, 0x00800000

    ; multibootレジスタを保存 (eax=magic, ebx=info_ptr)
    mov     [mb2_magic], eax
    mov     [mb2_info],  ebx

    ; シリアルポートへデバッグ文字列 "B3\r\n" 送信
    mov     dx, 0x3F8
    mov     al, 'B'
    out     dx, al
    mov     al, '3'
    out     dx, al
    mov     al, '2'
    out     dx, al
    mov     al, 0x0D        ; CR
    out     dx, al
    mov     al, 0x0A        ; LF
    out     dx, al

    ; =====================================================================
    ; CPUID で 64bit (Long Mode) サポートを確認
    ; =====================================================================

    ; まず CPUID 命令自体がサポートされているか確認 (EFLAGS.ID bit)
    pushf
    pop     eax
    mov     ecx, eax
    xor     eax, 0x00200000     ; ID ビットを反転
    push    eax
    popf
    pushf
    pop     eax
    push    ecx
    popf
    xor     eax, ecx
    jz      .no_longmode        ; ID ビットが変化しなければ CPUID 非対応

    ; CPUID 拡張機能の最大値を取得
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      .no_longmode        ; 拡張CPUID非対応

    ; Long Mode (64bit) サポートを確認
    mov     eax, 0x80000001
    cpuid
    test    edx, 0x20000000     ; LM bit (bit 29)
    jz      .no_longmode

    ; =====================================================================
    ; ページテーブル設定 (4レベルページング)
    ; PML4 @ 0x1000, PDPT @ 0x2000..0x7000 (6エントリ), PD @ 0x3000..0x6000
    ; =====================================================================

    ; ページテーブル領域をゼロクリア (0x1000 〜 0x8000)
    mov     edi, 0x1000
    xor     eax, eax
    mov     ecx, 0x1C00         ; 7 * 4096 / 4 dwords
    rep     stosd

    ; PML4[0] → PDPT @ 0x2000
    mov     dword [0x1000], 0x2003      ; Present + R/W
    mov     dword [0x1004], 0

    ; PDPT[0] → PD @ 0x3000
    mov     dword [0x2000], 0x3003
    mov     dword [0x2004], 0
    ; PDPT[1] → PD @ 0x4000
    mov     dword [0x2008], 0x4003
    mov     dword [0x200C], 0
    ; PDPT[2] → PD @ 0x5000
    mov     dword [0x2010], 0x5003
    mov     dword [0x2014], 0
    ; PDPT[3] → PD @ 0x6000
    mov     dword [0x2018], 0x6003
    mov     dword [0x201C], 0

    ; PD (0x3000〜0x6000): 2MB ページで全物理メモリをアイデンティティマップ
    mov     edi, 0x3000
    xor     ebx, ebx
    mov     ecx, 0x200          ; 512エントリ
.map_3000:
    mov     eax, ebx
    or      eax, 0x83           ; Present + R/W + PageSize(2MB)
    mov     [edi],   eax
    mov     dword [edi+4], 0
    add     ebx, 0x200000       ; 2MB ずつ
    add     edi, 8
    loop    .map_3000

    mov     edi, 0x4000
    mov     ecx, 0x200
.map_4000:
    mov     eax, ebx
    or      eax, 0x83
    mov     [edi],   eax
    mov     dword [edi+4], 0
    add     ebx, 0x200000
    add     edi, 8
    loop    .map_4000

    mov     edi, 0x5000
    mov     ecx, 0x200
.map_5000:
    mov     eax, ebx
    or      eax, 0x83
    mov     [edi],   eax
    mov     dword [edi+4], 0
    add     ebx, 0x200000
    add     edi, 8
    loop    .map_5000

    mov     edi, 0x6000
    mov     ecx, 0x200
.map_6000:
    mov     eax, ebx
    or      eax, 0x83
    mov     [edi],   eax
    mov     dword [edi+4], 0
    add     ebx, 0x200000
    add     edi, 8
    loop    .map_6000

    ; =====================================================================
    ; Long Mode 有効化
    ; =====================================================================

    ; CR3 = PML4 の物理アドレス
    mov     eax, 0x1000
    mov     cr3, eax

    ; CR4.PAE = 1 (Physical Address Extension)
    mov     eax, cr4
    or      eax, 0x20
    mov     cr4, eax

    ; EFER.LME = 1 (Long Mode Enable)
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, 0x100
    wrmsr

    ; CR0.PG = 1, CR0.PE = 1 (Paging + Protected Mode)
    mov     eax, cr0
    or      eax, 0x80000001
    mov     cr0, eax

    ; GDT をロード
    lgdt    [gdt_ptr]

    ; 64bit コードセグメントにファージャンプ
    ; ★ ローカルラベル(.xxx)は [BITS 32] をまたげないのでグローバルラベルを使う
    jmp     0x08:longmode64     ; selector=0x08 (64-bit code descriptor)

.no_longmode:
    cli
    hlt
    jmp     .no_longmode

; =============================================================================
; 64bit Long Mode エントリポイント
; =============================================================================
[BITS 64]
longmode64:

    ; データセグメントを 64bit 用に設定
    mov     ax, 0x10        ; selector=0x10 (data descriptor)
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; CR0: EM=0 (FPU エミュレーション無効), MP=1, TS=0
    ; ★ 64bitモードでは rax を使う (eax は不可)
    mov     rax, cr0
    and     rax, ~(1 << 2)      ; EM ビットをクリア
    or      rax, (1 << 1)       ; MP ビットをセット
    mov     cr0, rax

    ; CR4: OSFXSR=1, OSXMMEXCPT=1 (SSE2 有効)
    mov     rax, cr4
    or      rax, 0x600
    mov     cr4, rax

    ; FPU 初期化
    fninit

    ; 64bit スタック設定
    ; memory.c の静的カーネルヒープは 0x0063A000 から 32MiB を使用する。
    ; 旧 0x00900000 はこの範囲内だったため、1024x768 のバックバッファ
    ; 初期化が復帰アドレスを上書きしていた。スタックはヒープ外かつ初期
    ; アイデンティティマップ内の安全な位置へ置く。
    mov     rsp, 0x02F00000
    and     rsp, ~0xF           ; 16バイトアライメント

    ; multiboot 保存値を引数として取り出す (kernel_main(magic, info_ptr))
    mov     edi, dword [mb2_magic]   ; arg0 = multiboot magic
    mov     esi, dword [mb2_info]    ; arg1 = multiboot info ptr

    ; シリアルへ "LM\r\n" (Long Mode 起動確認)
    mov     dx, 0x3F8
    mov     al, 'L'
    out     dx, al
    mov     al, 'M'
    out     dx, al
    mov     al, 0x0D
    out     dx, al
    mov     al, 0x0A
    out     dx, al

    ; kernel_main を呼び出す (C コード)
    extern  kernel_main
    call    kernel_main

    ; kernel_main が返った場合: ハング
halt_loop:
    cli
    hlt
    jmp     halt_loop
