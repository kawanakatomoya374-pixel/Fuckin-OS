#ifndef PYTHON_CODE_TO_C_HOST_STACK_BOUNDS_H
#define PYTHON_CODE_TO_C_HOST_STACK_BOUNDS_H

/* ホスト（テストハーネス）側の実装。カーネルではタスク構造体が持つ
 * stack_base/stack_size を返す関数に置き換える。 */
void p2c_host_stack_bounds(void **stack_lo, void **stack_hi);

#endif
