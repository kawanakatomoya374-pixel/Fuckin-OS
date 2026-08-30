#include <string.h>
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/lexer.h"
#include "py/parse.h"
#include "py/compile.h"
#include "py/builtin.h"
#include "../../include/serial.h"
#include "micropython_interface.h"

// MicroPython REPL implementation using the real core
void micropython_repl_run(void) {
    extern void *mp_hal_get_heap_start(void);
    extern void *mp_hal_get_heap_end(void);

    // Initialize GC and Runtime
    gc_init(mp_hal_get_heap_start(), mp_hal_get_heap_end());
    mp_init();

    serial_puts("Welcome to MicroPython on C-OS!\n");

    // Simple REPL loop (since pyexec is missing)
    for (;;) {
        mp_hal_stdout_tx_strn(">>> ", 4);
        
        char line[256];
        int i = 0;
        for (;;) {
            int c = mp_hal_stdin_rx_chr();
            if (c == '\r' || c == '\n') {
                mp_hal_stdout_tx_strn("\n", 1);
                line[i] = '\0';
                break;
            } else if (c == 8 || c == 127) { // Backspace
                if (i > 0) {
                    i--;
                    mp_hal_stdout_tx_strn("\b \b", 3);
                }
            } else if (i < 255) {
                line[i++] = c;
                char ch = (char)c;
                mp_hal_stdout_tx_strn(&ch, 1);
            }
        }

        if (strlen(line) == 0) continue;
        if (strcmp(line, "exit") == 0) break;

        // Execute line
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, line, strlen(line), 0);
            mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
            mp_obj_t module_fun = mp_compile(&parse_tree, lex->source_name, true);
            mp_call_function_0(module_fun);
            nlr_pop();
        } else {
            // uncaught exception
            mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        }
    }

    mp_deinit();
}

int micropython_repl_execute_file(const char* filename) {
    char output[1024];
    output[0] = '\0';
    int rc = micropython_execute_file(filename, output, sizeof(output));
    if (output[0] != '\0') {
        serial_puts(output);
        serial_puts("\n");
    }
    return rc;
}
