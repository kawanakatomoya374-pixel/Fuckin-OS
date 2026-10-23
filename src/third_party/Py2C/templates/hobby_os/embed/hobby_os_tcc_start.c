/*
 * HobbyOS 向け ELF の ENTRY シム（TinyCC 専用、Beta0.1）
 * ========================================================
 * TinyCC はリンカスクリプトの ENTRY(...) も --entry も扱えず、-nostdlib -static で
 * 作る ELF の入口は常に _start になる。そこで _start から、カーネルへ公開する
 * p2c_hobbyos_entry() を呼ぶ薄いシムを置く。
 *
 * GCC/Clang の経路（make hobbyos-elf）では不要。そちらは hobbyos.ld の
 * ENTRY(p2c_hobbyos_entry) が入口を直接決める。
 *
 * カーネルへの契約:
 *   - ELF の e_entry は _start（このシム）になる。make tcc hobby c99 の manifest に
 *     `entry_sym _start matches e_entry` と記録される。
 *   - p2c_hobbyos_entry() は戻らない前提。戻ってきた場合は、ここで停止する
 *     （戻り先のスタックが無いため）。
 */

extern int p2c_hobbyos_entry(void);

void _start(void) {
    (void)p2c_hobbyos_entry();
    for (;;) {
        /* 停止（カーネル側で割り込み禁止の halt に差し替えてよい） */
    }
}

/* TinyCC の libtcc1.a（va_list.o の __va_arg）は、想定外の va_arg の経路で abort を
 * 参照する。HobbyOS は libc を持たないので、ここで停止する弱定義を置く。カーネルが
 * 強い abort を提供すればそちらが使われる（弱定義なので二重定義にはならない）。 */
__attribute__((weak)) void abort(void) {
    for (;;) {
        /* 停止 */
    }
}
