#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/include/python_code_to_c_single.h"}
mkdir -p "$(dirname "$OUT")"

emit_without_project_includes() {
    sed '/^[[:space:]]*#include[[:space:]]*"/d' "$1"
}

{
    cat <<'PROLOGUE'
/*
 * Python Code to C Alpha0.6 — single-header distribution.
 *
 * Define P2C_SINGLE_HEADER_IMPLEMENTATION in exactly one translation unit
 * before including this file to emit the compiler, runtime and GUI core.
 * Define P2C_SINGLE_HEADER_NO_HOSTED with PYTHON_CODE_TO_C_NO_STDLIB when a
 * target OS supplies its own P2C_Platform implementation.
 *
 * Hosted feature macros (_GNU_SOURCE / _POSIX_C_SOURCE) are defined only when
 * PYTHON_CODE_TO_C_NO_STDLIB is *not* set. A hobby OS / freestanding target
 * therefore includes this header without silently enabling POSIX or GNU
 * extensions: the header stays pure ISO C11 + freestanding.
 */
#ifndef PYTHON_CODE_TO_C_SINGLE_H
#define PYTHON_CODE_TO_C_SINGLE_H

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

PROLOGUE

    for header in \
        include/common/python_code_to_c_common.h \
        include/platform/python_code_to_c_platform.h \
        include/platform/python_code_to_c_gui.h \
        include/lexer/python_code_to_c_lexer.h \
        include/parser/python_code_to_c_ast.h \
        include/parser/python_code_to_c_astdump.h \
        include/parser/python_code_to_c_parser.h \
        include/semantic/python_code_to_c_semantic.h \
        include/codegen/python_code_to_c_codegen.h \
        include/runtime/python_code_to_c_runtime.h \
        include/platform/python_code_to_c_embed.h \
        include/modules/python_code_to_c_pygame.h \
        include/core/python_code_to_c.h; do
        printf '\n/* BEGIN %s */\n' "$header"
        emit_without_project_includes "$ROOT/$header"
        printf '\n/* END %s */\n' "$header"
    done

    cat <<'EPILOGUE'
#endif /* PYTHON_CODE_TO_C_SINGLE_H */

#ifdef P2C_SINGLE_HEADER_IMPLEMENTATION
#ifndef P2C_SINGLE_HEADER_IMPLEMENTED
#define P2C_SINGLE_HEADER_IMPLEMENTED

EPILOGUE

    for source in \
        src/common/python_code_to_c_common.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_embed.c \
        src/platform/python_code_to_c_gui.c \
        src/lexer/python_code_to_c_lexer.c \
        src/parser/python_code_to_c_ast.c \
        src/parser/python_code_to_c_astdump.c \
        src/parser/python_code_to_c_parser.c \
        src/semantic/python_code_to_c_semantic.c \
        src/codegen/python_code_to_c_codegen.c \
        src/runtime/python_code_to_c_runtime.c \
        src/modules/python_code_to_c_pygame.c \
        src/core/python_code_to_c.c; do
        printf '\n/* BEGIN %s */\n' "$source"
        emit_without_project_includes "$ROOT/$source"
        printf '\n/* END %s */\n' "$source"
    done

    cat <<'HOSTED'
#ifndef P2C_SINGLE_HEADER_NO_HOSTED
/* BEGIN src/platform/python_code_to_c_platform_hosted.c */
HOSTED
    emit_without_project_includes "$ROOT/src/platform/python_code_to_c_platform_hosted.c"
    cat <<'EPILOGUE'
/* END src/platform/python_code_to_c_platform_hosted.c */
#endif

#endif /* P2C_SINGLE_HEADER_IMPLEMENTED */
#endif /* P2C_SINGLE_HEADER_IMPLEMENTATION */
EPILOGUE
} > "$OUT"

printf '%s\n' "$OUT"
