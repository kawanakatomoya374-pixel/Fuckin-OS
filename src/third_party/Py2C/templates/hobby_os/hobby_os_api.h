#ifndef PYTHON_CODE_TO_C_HOBBY_OS_API_H
#define PYTHON_CODE_TO_C_HOBBY_OS_API_H

#include "platform/python_code_to_c_embed.h"

/* 推奨構成（p2c_embed ファサード）: embed/hobby_os_embed.c が提供する。
 * task_stack_lo/task_stack_hi はそのタスクのスタック区間の両端。 */
int p2c_hobby_run_task(void *task_stack_lo, void *task_stack_hi);

/* フルコントロール構成: platform/python_code_to_c_platform_user.c が提供する。
 * カーネル自前のアロケータとプラットフォーム層を使う。 */
int p2c_hobby_run_task_full(void *heap, size_t heap_size, void *stack_lo, void *stack_hi);

/* 変換で生成したモジュールのエントリ（--embed-entry kernel_python_program） */
P2C_Object *kernel_python_program(void);

#endif
