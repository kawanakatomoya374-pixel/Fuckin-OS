#include <stdio.h>
#include <unistd.h>
#include "cosarc.h"
static void pl(const char *l) { puts(l); }
int main(int argc, char **argv) { char cwd[1024]; getcwd(cwd, sizeof cwd); return cosarc_main(argc - 1, argv + 1, cwd, pl); }
