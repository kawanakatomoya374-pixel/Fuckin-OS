#!/usr/bin/env python3
"""One-shot migration helper: route runtime allocation sites through the
OOM-notifying wrappers (p2c_malloc_checked / p2c_realloc_checked /
p2c_calloc_checked).

Each entry is (old_text, new_text, expected_occurrences). The script refuses to
write anything unless every expected count matches exactly, so source drift is
reported instead of silently mangled. Run it once while applying the OOM
hardening change; it is kept for review traceability.
"""
import sys

PATH = "src/runtime/python_code_to_c_runtime.c"

PAIRS = [
    ("out->u.v_str.data = (char*)malloc((size_t)repeat * len + 1);",
     'out->u.v_str.data = (char*)p2c_malloc_checked((size_t)repeat * len + 1, "str repeat");', 1),
    ("out->u.v_tuple.items = (P2C_Object**)malloc(total * sizeof(P2C_Object*));",
     'out->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(total * sizeof(P2C_Object*), "tuple items");', 1),
    ("o->u.v_list.items = (P2C_Object**)malloc(len * sizeof(P2C_Object*));",
     'o->u.v_list.items = (P2C_Object**)p2c_malloc_checked(len * sizeof(P2C_Object*), "list items");', 1),
    ("P2C_Object **new_items = (P2C_Object**)realloc(list->u.v_list.items, new_cap * sizeof(P2C_Object*));",
     'P2C_Object **new_items = (P2C_Object**)p2c_realloc_checked(list->u.v_list.items, new_cap * sizeof(P2C_Object*), "list growth");', 1),
    ("o->u.v_str.data = (char*)malloc(la + lb + 1);",
     'o->u.v_str.data = (char*)p2c_malloc_checked(la + lb + 1, "str concat");', 1),
    ("char *out = (char*)malloc(cap);",
     'char *out = (char*)p2c_malloc_checked(cap, "text builder");', 1),
    ("char *_r = (char*)realloc(out, cap);",
     'char *_r = (char*)p2c_realloc_checked(out, cap, "text builder growth");', 2),
    ("char *buf = (char*)malloc((size_t)n + 1);",
     'char *buf = (char*)p2c_malloc_checked((size_t)n + 1, "format buffer");', 1),
    ("pair->u.v_tuple.items = (P2C_Object**)malloc(2 * sizeof(P2C_Object*));",
     'pair->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(2 * sizeof(P2C_Object*), "pair items");', 3),
    ("char *buf = (char*)malloc(len + 1);",
     'char *buf = (char*)p2c_malloc_checked(len + 1, "string copy");', 2),
    ("char *buf = (char*)malloc(end - start + 1);",
     'char *buf = (char*)p2c_malloc_checked(end - start + 1, "string slice");', 1),
    ("char *buf = (char*)malloc(slen + 1);",
     'char *buf = (char*)p2c_malloc_checked(slen + 1, "string replace");', 1),
    ("char *buf = (char*)malloc((size_t)w + 1);",
     'char *buf = (char*)p2c_malloc_checked((size_t)w + 1, "string pad");', 4),
    ("flat_names = flat_n ? (const char**)malloc(flat_n * sizeof(const char*)) : NULL;",
     'flat_names = flat_n ? (const char**)p2c_malloc_checked(flat_n * sizeof(const char*), "call plan names") : NULL;', 1),
    ("flat_values = flat_n ? (P2C_Object**)malloc(flat_n * sizeof(P2C_Object*)) : NULL;",
     'flat_values = flat_n ? (P2C_Object**)p2c_malloc_checked(flat_n * sizeof(P2C_Object*), "call plan values") : NULL;', 1),
    ("P2C_Object **buf = (P2C_Object**)malloc(cap * sizeof(P2C_Object*));",
     'P2C_Object **buf = (P2C_Object**)p2c_malloc_checked(cap * sizeof(P2C_Object*), "call args");', 1),
    ("rhs_copy = (P2C_Object**)malloc(rhs_len * sizeof(P2C_Object*));",
     'rhs_copy = (P2C_Object**)p2c_malloc_checked(rhs_len * sizeof(P2C_Object*), "call rhs copy");', 1),
    ("P2C_Object **grown = (P2C_Object**)realloc(obj->u.v_list.items, cap * sizeof(P2C_Object*));",
     'P2C_Object **grown = (P2C_Object**)p2c_realloc_checked(obj->u.v_list.items, cap * sizeof(P2C_Object*), "list extend growth");', 1),
    ("size_t *lens = (size_t*)malloc(nargs * sizeof(size_t));",
     'size_t *lens = (size_t*)p2c_malloc_checked(nargs * sizeof(size_t), "varargs lengths");', 1),
    ("P2C_Object ***items = (P2C_Object***)malloc(nargs * sizeof(P2C_Object**));",
     'P2C_Object ***items = (P2C_Object***)p2c_malloc_checked(nargs * sizeof(P2C_Object**), "varargs slots");', 1),
    ("bool *owned = (bool*)malloc(nargs * sizeof(bool));",
     'bool *owned = (bool*)p2c_malloc_checked(nargs * sizeof(bool), "varargs ownership");', 1),
    ("tup->u.v_tuple.items = (P2C_Object**)malloc(nargs * sizeof(P2C_Object*));",
     'tup->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(nargs * sizeof(P2C_Object*), "varargs tuple items");', 1),
    ("P2C_Object **arr = n ? (P2C_Object**)malloc(n * sizeof(P2C_Object*)) : NULL;",
     'P2C_Object **arr = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "sorted copy") : NULL;', 2),
    ("P2C_Object **grown = (P2C_Object**)realloc((void*)arr, next_cap * sizeof(P2C_Object*));",
     'P2C_Object **grown = (P2C_Object**)p2c_realloc_checked((void*)arr, next_cap * sizeof(P2C_Object*), "sorted growth");', 1),
    ("P2C_Object **arr = n > 0 ? (P2C_Object**)malloc((size_t)n * sizeof(P2C_Object*)) : NULL;",
     'P2C_Object **arr = n > 0 ? (P2C_Object**)p2c_malloc_checked((size_t)n * sizeof(P2C_Object*), "reversed copy") : NULL;', 1),
    ("P2C_Object **tmp = n ? (P2C_Object**)malloc(n * sizeof(P2C_Object*)) : NULL;",
     'P2C_Object **tmp = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "set scratch") : NULL;', 1),
    ("P2C_Object **keys = n ? (P2C_Object**)malloc(n * sizeof(P2C_Object*)) : NULL;",
     'P2C_Object **keys = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "set keys") : NULL;', 1),
    ("o->u.v_dict.buckets = (P2C_DictEntry**)calloc(o->u.v_dict.bucket_count, sizeof(P2C_DictEntry*));",
     'o->u.v_dict.buckets = (P2C_DictEntry**)p2c_calloc_checked(o->u.v_dict.bucket_count, sizeof(P2C_DictEntry*), "dict buckets");', 2),
    ("P2C_DictEntry *entry = (P2C_DictEntry*)calloc(1, sizeof(P2C_DictEntry));",
     'P2C_DictEntry *entry = (P2C_DictEntry*)p2c_calloc_checked(1, sizeof(P2C_DictEntry), "dict entry");', 1),
    ("o->u.v_tuple.items = (P2C_Object**)calloc(len ? len : 1, sizeof(P2C_Object*));",
     'o->u.v_tuple.items = (P2C_Object**)p2c_calloc_checked(len ? len : 1, sizeof(P2C_Object*), "tuple slots");', 1),
    ("e = (P2C_MapEntry*)calloc(1, sizeof(P2C_MapEntry));",
     'e = (P2C_MapEntry*)p2c_calloc_checked(1, sizeof(P2C_MapEntry), "attribute entry");', 1),
    ("e = (P2C_DictEntry*)calloc(1, sizeof(P2C_DictEntry));",
     'e = (P2C_DictEntry*)p2c_calloc_checked(1, sizeof(P2C_DictEntry), "dict entry");', 1),
]


def main() -> int:
    with open(PATH, "r", encoding="utf-8", newline="") as fh:
        src = fh.read()
    for old, new, expected in PAIRS:
        got = src.count(old)
        if got != expected:
            print(f"count mismatch ({got} != {expected}) for: {old}", file=sys.stderr)
            return 1
        src = src.replace(old, new)
    with open(PATH, "w", encoding="utf-8", newline="") as fh:
        fh.write(src)
    print(f"converted {len(PAIRS)} allocation patterns")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
