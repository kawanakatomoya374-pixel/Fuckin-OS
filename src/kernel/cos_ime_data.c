/* cos_ime_data.c - the IME dictionary (userland/third_party/mozc_dict/cosime.dic, built by tools/build_ime_dict.py) linked into
 * the kernel image with .incbin; cos_system_layout.c installs it as /system/ime/cosime.dic. */
__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global cos_ime_dic\n"
        "cos_ime_dic:\n"
        ".incbin \"" COS_IME_DIC_PATH "\"\n"
        ".global cos_ime_dic_end\n"
        "cos_ime_dic_end:\n"
        ".byte 0\n"
        ".previous\n");
