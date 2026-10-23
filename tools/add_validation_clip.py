#!/usr/bin/env python3
"""Wire a new Media validation clip into the build.

    tools/add_validation_clip.py <name> <clip.mp4>

<name> is a lowercase identifier such as hevc_12bit. This:
  * embeds the clip as src/assets/sample_test_<name>_mp4.c,
  * adds VALIDATION_OPEN_<NAME> to the Makefile (-D flag, object list, object rule),
  * writes it to /desktop/test_<name>.mp4 at boot (src/fs/fs.c) and, when the flag is set,
    opens it in Media (src/kernel/kernel.c).

Then:  make iso VALIDATION_OPEN_<NAME>=1 VALIDATION_FRAME_CRC=1
       tools/frame_crc_ref.py <clip.mp4> --diff <serial log>
Idempotent: does nothing if <name> is already wired.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def main():
    if len(sys.argv) != 3 or not re.fullmatch(r'[a-z][a-z0-9_]*', sys.argv[1]):
        sys.exit(__doc__)
    name, clip = sys.argv[1], sys.argv[2]
    NAME = name.upper()
    sym = 'sample_test_%s_mp4' % name
    asset_path = os.path.join(ROOT, 'src/assets/%s.c' % sym)
    flag = 'VALIDATION_OPEN_' + NAME
    cflag = 'COS_' + flag

    data = open(clip, 'rb').read()
    lines = ['    ' + ', '.join('0x%02X' % b for b in data[i:i + 12]) + ',' for i in range(0, len(data), 12)]
    open(asset_path, 'w').write('/* Auto-generated from %s; do not edit by hand. */\nconst unsigned char %s[] = {\n%s\n};\n'
                                'const unsigned int %s_len = %dU;\n' % (os.path.basename(clip), sym, '\n'.join(lines), sym, len(data)))

    mk_path = os.path.join(ROOT, 'Makefile')
    mk = open(mk_path).read()
    if flag not in mk:
        # -D flag next to the other VALIDATION_OPEN_* defines
        anchor = re.search(r'^[ \t]*-DCOS_VALIDATION_OPEN_HEVC=.*\\\n', mk, re.M)
        assert anchor, 'cannot find the VALIDATION_OPEN_* define block in the Makefile'
        mk = mk[:anchor.end()] + '\t          \t-D%s=$(if $(%s),$(%s),0) \\\n' % (cflag, flag, flag) + mk[anchor.end():]
        # object list
        o = re.search(r'^[ \t]*\$\(OBJ_DIR\)/assets/sample_test_hevc_mp4\.o \\\n', mk, re.M)
        assert o, 'cannot find the asset object list'
        mk = mk[:o.end()] + '\t\t$(OBJ_DIR)/assets/%s.o \\\n' % sym + mk[o.end():]
        # object rule
        r = mk.index('$(OBJ_DIR)/assets/netsurf_icon_png.o:')
        rule = ('$(OBJ_DIR)/assets/%s.o: $(SRCROOT)/assets/%s.c | $(OBJ_DIR)\n\tmkdir -p $(dir $@)\n'
                '\t$(CC) $(filter-out -O2,$(CFLAGS)) -O0 $(INCLUDES) -c $< -o $@\n') % (sym, sym)
        mk = mk[:r] + rule + mk[r:]
        open(mk_path, 'w').write(mk)

    fs_path = os.path.join(ROOT, 'src/fs/fs.c')
    fs = open(fs_path).read()
    if cflag not in fs:
        fs = fs.replace('extern const unsigned int sample_test_hevc_mp4_len;\n',
                        'extern const unsigned int sample_test_hevc_mp4_len;\nextern const unsigned char %s[];\nextern const unsigned int %s_len;\n' % (sym, sym), 1)
        anchor = '#if COS_VALIDATION_OPEN_HEVC\n    fs_bootstrap_write_asset("/desktop/test_hevc.mp4", (const char*)sample_test_hevc_mp4, (uint64_t)sample_test_hevc_mp4_len);\n#endif\n'
        assert anchor in fs
        add = ('#ifndef %s\n#define %s 0\n#endif\n#if %s\n    fs_bootstrap_write_asset("/desktop/test_%s.mp4", (const char*)%s, (uint64_t)%s_len);\n#endif\n'
               % (cflag, cflag, cflag, name, sym, sym))
        fs = fs.replace(anchor, anchor + add, 1)
        open(fs_path, 'w').write(fs)

    k_path = os.path.join(ROOT, 'src/kernel/kernel.c')
    k = open(k_path).read()
    if cflag not in k:
        anchor = '    cos_media_open("/desktop/test_hevc.mp4");\n#endif\n'
        assert anchor in k
        add = ('#ifndef %s\n#define %s 0\n#endif\n#if %s\n    extern void cos_media_open(const char *path);\n'
               '    serial_puts("[VALIDATION] launching Media.c-os with /desktop/test_%s.mp4\\n");\n'
               '    cos_media_open("/desktop/test_%s.mp4");\n#endif\n' % (cflag, cflag, cflag, name, name))
        k = k.replace(anchor, anchor + add, 1)
        open(k_path, 'w').write(k)
    print('wired %s: make iso %s=1 VALIDATION_FRAME_CRC=1' % (name, flag))

main()
