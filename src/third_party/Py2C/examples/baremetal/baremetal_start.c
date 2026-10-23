#include "baremetal/python_code_to_c_baremetal.h"

/* p2c_baremetal_hello_program は python_code_to_c_baremetal.h で宣言済み
 * （-Wredundant-decls のため再宣言しない）。 */

static unsigned char p2c_baremetal_heap[131072];
static char p2c_baremetal_console[1024];

int p2c_baremetal_entry(void) {
    P2C_BaremetalBoard board;
    p2c_baremetal_board_init(&board, p2c_baremetal_heap, sizeof(p2c_baremetal_heap),
                             p2c_baremetal_console, sizeof(p2c_baremetal_console));
    return p2c_baremetal_run(&board, p2c_baremetal_hello_program);
}
