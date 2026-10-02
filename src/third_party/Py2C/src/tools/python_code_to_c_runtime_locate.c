#include "tools/python_code_to_c_runtime_locate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int path_is_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* src_dir候補が現行レイアウト（機能ごとのサブフォルダ分け）の src/ ルートかどうかを
 * runtime/python_code_to_c_runtime.c の有無で確認し、正しければ各パスを out へ埋める。 */
static int fill_if_valid(const char *src_dir, P2C_RuntimeLocation *out) {
    char probe[P2C_RUNTIME_LOCATE_BUFSZ];  /* src_dir + "/runtime/python_code_to_c_runtime.c" 等 */
    snprintf(probe, sizeof(probe), "%s/runtime/python_code_to_c_runtime.c", src_dir);
    if (!path_is_file(probe)) return 0;
    snprintf(out->runtime_c,   sizeof(out->runtime_c),   "%s/runtime/python_code_to_c_runtime.c", src_dir);
    snprintf(out->common_c,         sizeof(out->common_c),         "%s/common/python_code_to_c_common.c", src_dir);
    snprintf(out->platform_core_c,  sizeof(out->platform_core_c),  "%s/platform/python_code_to_c_platform.c", src_dir);
    snprintf(out->platform_c,       sizeof(out->platform_c),       "%s/platform/python_code_to_c_platform_hosted.c", src_dir);
    snprintf(out->pygame_c,    sizeof(out->pygame_c),    "%s/modules/python_code_to_c_pygame.c", src_dir);
    snprintf(out->include_dir, sizeof(out->include_dir), "%s/../include", src_dir);
    return 1;
}

int p2c_locate_runtime(const char *argv0, P2C_RuntimeLocation *out) {
    memset(out, 0, sizeof(*out));

    const char *env = getenv("PYTHON_CODE_TO_C_SRC_DIR");
    if (env && *env && fill_if_valid(env, out)) return 1;

    if (argv0) {
        char buf[P2C_RTLOC_TIER1];  /* argv0 をそのままコピー */
        snprintf(buf, sizeof(buf), "%s", argv0);
        char *slash = strrchr(buf, '/');
        if (slash) {
            *slash = '\0';
            char candidate[P2C_RTLOC_TIER2];  /* buf + "/../src" */
            snprintf(candidate, sizeof(candidate), "%s/../src", buf);
            if (fill_if_valid(candidate, out)) return 1;
        }
    }

    if (fill_if_valid("src", out)) return 1;

    return 0;
}
