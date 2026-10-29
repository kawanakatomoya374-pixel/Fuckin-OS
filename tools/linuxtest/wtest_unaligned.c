#include "cos.h"
#include <stdlib.h>
#include <string.h>
int main(void) {
    static const int offs[] = { 0, 100, 512, 4096, 347 };
    for (int k = 0; k < 5; k++) {
        char path[64];
        path[0] = 0; strcpy(path, "/wt0.bin"); path[3] = (char)('0' + k);
        cos_FILE *f = cos_fopen(path, "w"); if (!f) { cos_puts("open failed\n"); return 1; }
        unsigned char *b = malloc(65536 + 4096);
        for (int i = 0; i < offs[k]; i++) b[i] = 0xEE;
        cos_fwrite(b, 1, (size_t)offs[k], f);
        for (int i = 0; i < 65536; i++) b[i] = (unsigned char)(i * 7 + 3 + (i >> 8));
        size_t w = cos_fwrite(b, 1, 65536, f);
        cos_fclose(f);
        free(b);
        cos_puts(w == 65536 ? "ok\n" : "short\n");
    }
    return 0;
}
