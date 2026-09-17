/* Fixture: a program written the way an external developer would write
 * one, built with tools/cos-cc and the real C-OS userland runtime.
 *
 * Everything else in fixtures/ is a targeted probe for one ELF feature.
 * This one is the integration check: if the documented way to build a
 * program for this OS stops producing something the loader accepts, that
 * is the failure users actually hit, and no amount of per-feature
 * coverage catches it. */
#include "cos.h"

__thread int tls_counter = 41;

static int   g_ctor_ran;
static char  g_big_bss[16384];   /* forces a large .bss: memsz >> filesz */
const char   g_marker[] = "COS_APP_MARKER";

static void __attribute__((constructor)) init_me(void) { g_ctor_ran = 1; }
static void __attribute__((destructor))  fini_me(void) { g_ctor_ran = 0; }

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    g_big_bss[0] = (char)argc;
    return g_ctor_ran + tls_counter + (argv != 0);
}
