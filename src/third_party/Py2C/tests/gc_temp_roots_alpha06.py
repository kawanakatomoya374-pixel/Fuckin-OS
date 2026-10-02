# GCH-010: 式の途中で例外が脱出したときのTLS一時値の後始末の回帰（CPython差分）。
#
# 生成Cは二項演算を
#     (p2c_binop_begin(left), p2c_binop_finish(p2c_obj_add, right))
# の形で出す。right の評価中に例外が脱出する（longjmpする）と、以前は
# 左オペランドがbinopスタックに残ったままで、反復の65回目に
# 「binary expression nesting limit exceeded」(RuntimeError)を誤発火していた。
# f-stringも同様にビルダ（g_fstr_stack。ネイティブ確保）が残って33回目に
# ネスト上限のRuntimeErrorになり、確保が解放されずリークしていた。
# どちらも except ValueError を素通りするため、この回帰は
# 「200回ともexcept ValueErrorで捕まり、通常の出力だけが出る」ことを確認する。
# 右オペランド側で自動GCしきい値(256KiB)を超えて確保し、左オペランドが
# TLSにしか無い間に収集が走る状況も作る。


def boom():
    raise ValueError("escaped from expression")


def build_big_operand():
    acc = []
    for i in range(30000):
        acc.append([i])
    return acc


binop_caught = 0
fstr_caught = 0
for i in range(200):
    try:
        print([1, 2] + boom())
    except ValueError:
        binop_caught += 1
    try:
        print(f"n={boom()}")
    except ValueError:
        fstr_caught += 1

print(binop_caught, fstr_caught)

total = [1, 2] + build_big_operand()
print(len(total))

print([1, 2] + [3])
print(f"sum={7}")
