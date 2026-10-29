/* Object model internals shared by value.c, table.c, format.c and gc.c. */
#ifndef VALUE_INT_H
#define VALUE_INT_H

/* Values, objects, strings, arrays, ordered tables, equality, formatting, GC */
#include "sp.h"

/* functions shared between the parts of this stage */
void *new_obj(VM *vm, size_t size, int type, int with_id);
void obj_release_block(VM *vm, Obj *o);

#endif
