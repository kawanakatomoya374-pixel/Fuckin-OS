/* HobbyOS向けELFのリンカスクリプト (Alpha0.6)
 *
 * `make hobbyos elf` が作る「libcを使わない自己完結ELF」用の最小構成。
 * ロードアドレスはカーネルのローダに合わせて HOBBYOS_LOAD_ADDR で上書きできます
 * （例: -DHOBBYOS_LOAD_ADDR=0x100000 をリンク時に指定）。
 *
 * 設計方針:
 *   - ENTRY は p2c_hobbyos_entry（カーネルがロード後に呼ぶ関数）
 *   - .text(R+X) / .rodata(R) / .data+.bss(R+W) を別セグメントに分ける（W^X）
 *   - 動的リンク情報(INTERP)を作らない
 *   - デバッグ用の .comment/.note は捨てる（サイズと決定性のため）
 */
ENTRY(p2c_hobbyos_entry)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R + X */
    rodata PT_LOAD FLAGS(4); /* R     */
    data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = DEFINED(HOBBYOS_LOAD_ADDR) ? HOBBYOS_LOAD_ADDR : 0x400000;

    .text : ALIGN(16)
    {
        *(.text*)
    } :text

    .rodata : ALIGN(16)
    {
        *(.rodata*)
    } :rodata

    .data : ALIGN(16)
    {
        *(.data*)
    } :data

    .bss : ALIGN(16)
    {
        *(.bss*)
        *(COMMON)
    } :data

    . = ALIGN(16);
    __hobbyos_image_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
