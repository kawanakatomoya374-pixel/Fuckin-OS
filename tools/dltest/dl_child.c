/* dl_child.c - a trivial program the dltest checks start and stop: "dlchild quick" returns at once, "dlchild sleep" stays alive for a few seconds. */
#include "cos.h"
int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 's') cos_sleep_ms(4000);
    return 0;
}
