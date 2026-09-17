/* Fixture: a library with a real cross-object dependency. Its call to
 * base_value() becomes a JUMP_SLOT relocation that only resolves if
 * base_lib was loaded first - which is what the dependency graph is
 * for. Also carries a weak reference that is deliberately never
 * defined anywhere: the load must SUCCEED with it resolving to 0. */
extern int base_value(void);
extern int never_defined_anywhere(void) __attribute__((weak));

int dep_call(void) { return base_value() + 1; }
int dep_weak_is_null(void) { return never_defined_anywhere == 0; }
