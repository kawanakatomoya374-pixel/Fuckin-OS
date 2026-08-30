
#include "py/gc.h"
#include "py/lexer.h"
#include "py/runtime.h"

void gc_collect(void)
{
    gc_collect_start();
    gc_collect_end();
}

void nlr_jump_fail(void *val)
{
    (void)val;
    for (;;)
    {
    }
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    (void)filename;
    return NULL;
}
