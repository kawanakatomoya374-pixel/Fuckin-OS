/* Instruction lengths and the disassembler (sprint disasm). */
#include "compiler_int.h"

/* instruction lengths and disassembler                              */
/* ---------------------------------------------------------------- */
int op_len(int op) {
    switch (op) {
    case OP_CONST: case OP_SMALLINT: case OP_LOAD_LOCAL: case OP_STORE_LOCAL: case OP_MAKE_CELL:
    case OP_LOAD_CELL: case OP_STORE_CELL: case OP_LOAD_UPVAL: case OP_STORE_UPVAL:
    case OP_LOAD_MODULE: case OP_STORE_MODULE: case OP_LOAD_LINK: case OP_GET_MEMBER:
    case OP_SET_MEMBER: case OP_NEW_ARRAY: case OP_NEW_TABLE: case OP_FOR_PREP:
    case OP_RANGE_PREP: case OP_TEST_ARRAY: case OP_TEST_HAS: case OP_DEFER_SCOPE: case OP_RUN_DEFERS:
    case OP_CLASS: case OP_SET_SLOT: case OP_CTOR_CHECK:
        return 3;
    case OP_CONCAT: case OP_CALL: case OP_CALL_SPREAD: case OP_DEFER: return 2;
    case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: case OP_JUMP_IF_FALSE_OR_POP:
    case OP_JUMP_IF_TRUE_OR_POP: case OP_CLOSURE: case OP_TRY_BEGIN:
        return 5;
    case OP_INVOKE: case OP_INVOKE_SPREAD: case OP_DEFER_INVOKE: case OP_RAISE_ERR: return 4;
    case OP_FOR_NEXT: return 8;
    case OP_RANGE_NEXT: return 7;
    case OP_NOP: case OP_NIL: case OP_TRUE: case OP_FALSE: case OP_POP: case OP_DUP: case OP_DUP2:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_FLOOR_DIV: case OP_MOD:
    case OP_POW: case OP_NEG: case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT:
    case OP_SHL: case OP_SHR: case OP_NOT: case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
    case OP_GT: case OP_GE: case OP_GET_INDEX: case OP_SET_INDEX: case OP_RETURN:
    case OP_NEW_INSTANCE: case OP_YIELD: case OP_RETURN_NIL: case OP_THROW: case OP_TRY_END: case OP_PANIC: case OP_ASSERT_FAIL: case OP_MATCH_FAIL:
        return 1;
    default: return 0;
    }
}

static const char *op_name(int op) {
    switch (op) {
    case OP_NOP: return "NOP"; case OP_CONST: return "CONST"; case OP_NIL: return "NIL";
    case OP_TRUE: return "TRUE"; case OP_FALSE: return "FALSE"; case OP_SMALLINT: return "SMALLINT";
    case OP_POP: return "POP"; case OP_DUP: return "DUP"; case OP_DUP2: return "DUP2";
    case OP_LOAD_LOCAL: return "LOAD_LOCAL"; case OP_STORE_LOCAL: return "STORE_LOCAL";
    case OP_MAKE_CELL: return "MAKE_CELL"; case OP_LOAD_CELL: return "LOAD_CELL";
    case OP_STORE_CELL: return "STORE_CELL"; case OP_LOAD_UPVAL: return "LOAD_UPVAL";
    case OP_STORE_UPVAL: return "STORE_UPVAL"; case OP_LOAD_MODULE: return "LOAD_MODULE";
    case OP_STORE_MODULE: return "STORE_MODULE"; case OP_LOAD_LINK: return "LOAD_LINK";
    case OP_ADD: return "ADD"; case OP_SUB: return "SUB"; case OP_MUL: return "MUL";
    case OP_DIV: return "DIV"; case OP_FLOOR_DIV: return "FLOOR_DIV"; case OP_MOD: return "MOD";
    case OP_POW: return "POW"; case OP_NEG: return "NEG"; case OP_BAND: return "BAND";
    case OP_BOR: return "BOR"; case OP_BXOR: return "BXOR"; case OP_BNOT: return "BNOT";
    case OP_SHL: return "SHL"; case OP_SHR: return "SHR"; case OP_NOT: return "NOT";
    case OP_CONCAT: return "CONCAT"; case OP_EQ: return "EQ"; case OP_NE: return "NE";
    case OP_LT: return "LT"; case OP_LE: return "LE"; case OP_GT: return "GT"; case OP_GE: return "GE";
    case OP_JUMP: return "JUMP"; case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
    case OP_JUMP_IF_TRUE: return "JUMP_IF_TRUE"; case OP_JUMP_IF_FALSE_OR_POP: return "JUMP_IF_FALSE_OR_POP";
    case OP_JUMP_IF_TRUE_OR_POP: return "JUMP_IF_TRUE_OR_POP";
    case OP_GET_INDEX: return "GET_INDEX"; case OP_SET_INDEX: return "SET_INDEX";
    case OP_GET_MEMBER: return "GET_MEMBER"; case OP_SET_MEMBER: return "SET_MEMBER";
    case OP_NEW_ARRAY: return "NEW_ARRAY"; case OP_NEW_TABLE: return "NEW_TABLE";
    case OP_CLOSURE: return "CLOSURE"; case OP_CALL: return "CALL"; case OP_CALL_SPREAD: return "CALL_SPREAD";
    case OP_INVOKE: return "INVOKE"; case OP_INVOKE_SPREAD: return "INVOKE_SPREAD";
    case OP_RETURN: return "RETURN"; case OP_RETURN_NIL: return "RETURN_NIL";
    case OP_FOR_PREP: return "FOR_PREP"; case OP_FOR_NEXT: return "FOR_NEXT";
    case OP_RANGE_PREP: return "RANGE_PREP"; case OP_RANGE_NEXT: return "RANGE_NEXT";
    case OP_THROW: return "THROW"; case OP_TRY_BEGIN: return "TRY_BEGIN"; case OP_TRY_END: return "TRY_END";
    case OP_PANIC: return "PANIC"; case OP_ASSERT_FAIL: return "ASSERT_FAIL";
    case OP_TEST_ARRAY: return "TEST_ARRAY"; case OP_TEST_HAS: return "TEST_HAS"; case OP_MATCH_FAIL: return "MATCH_FAIL";
    case OP_DEFER_SCOPE: return "DEFER_SCOPE"; case OP_RUN_DEFERS: return "RUN_DEFERS";
    case OP_DEFER: return "DEFER"; case OP_DEFER_INVOKE: return "DEFER_INVOKE";
    case OP_NEW_INSTANCE: return "NEW_INSTANCE"; case OP_CLASS: return "CLASS"; case OP_SET_SLOT: return "SET_SLOT";
    case OP_CTOR_CHECK: return "CTOR_CHECK"; case OP_RAISE_ERR: return "RAISE_ERR"; case OP_YIELD: return "YIELD";
    default: return "???";
    }
}

void proto_disasm(const Proto *p, FILE *out) {
    (void)fprintf(out, "== %s (params=%d req=%d rest=%d locals=%d stack=%d upvals=%d)\n",
                  p->name ? p->name : "<anon>", p->nparams, p->nrequired, p->has_rest,
                  p->nlocals, p->max_stack, p->nupvals);
    uint32_t pc = 0;
    while (pc < p->code_len) {
        int op = p->code[pc];
        int len = op_len(op);
        if (len == 0) { (void)fprintf(out, "%04u  ??? (0x%02X)\n", pc, (unsigned)op); break; }
        (void)fprintf(out, "%04u  %-22s", pc, op_name(op));
        const uint8_t *a = p->code + pc + 1;
        switch (op) {
        case OP_SMALLINT: (void)fprintf(out, "%d", (int)(int16_t)(a[0] | (a[1] << 8))); break;
        case OP_CONCAT: case OP_CALL: case OP_CALL_SPREAD: case OP_DEFER: (void)fprintf(out, "%u", (unsigned)a[0]); break;
        case OP_INVOKE: case OP_INVOKE_SPREAD: case OP_DEFER_INVOKE: (void)fprintf(out, "k%u argc=%u", (unsigned)(a[0] | (a[1] << 8)), (unsigned)a[2]); break;
        case OP_CLOSURE: (void)fprintf(out, "proto %u", (unsigned)a[0] | ((unsigned)a[1] << 8) | ((unsigned)a[2] << 16) | ((unsigned)a[3] << 24)); break;
        case OP_FOR_NEXT: (void)fprintf(out, "s=%u n=%u", (unsigned)(a[0] | (a[1] << 8)), (unsigned)a[2]); break;
        case OP_RANGE_NEXT: (void)fprintf(out, "s=%u", (unsigned)(a[0] | (a[1] << 8))); break;
        default:
            if (len == 3) (void)fprintf(out, "%u", (unsigned)(a[0] | (a[1] << 8)));
            break;
        }
        (void)fputc('\n', out);
        pc += (uint32_t)len;
    }
}
