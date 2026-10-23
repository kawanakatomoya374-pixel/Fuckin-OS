/* Fixture: a dynamically linked PIE that calls into a library, uses TLS
 * of its own, and has a constructor. This is the shape an ordinary
 * externally-built program has, and the shape the old loader could not
 * run at all: ET_DYN, DT_NEEDED, PT_TLS and an init array.
 *
 * Built with --no-dynamic-linker so it carries no PT_INTERP: C-OS links
 * it itself rather than handing control to an ld.so it does not have. */
extern int dep_call(void);
extern int base_data;

__thread int exec_tls = 0xEE;
int exec_result;
int ctor_ran;

static void __attribute__((constructor)) ctor(void) { ctor_ran = 1; }

int exec_entry_value(void) { return dep_call() + base_data; }

void _start(void) { exec_result = exec_entry_value(); for (;;) { } }
