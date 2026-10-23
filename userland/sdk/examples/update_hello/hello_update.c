/* hello_update.c - the first program that was delivered by a C-OS update (release 4.0.9-r1): /apps/samples/hello_update.c-os */
#include <stdio.h>
int main(void) {
    printf("Hello from the first C-OS update!\n");
    printf("This program was not in your system image: cosupdate downloaded it, checked its signature and SHA-256, and installed it.\n");
    return 0;
}
