# 代入位置のスライス（以前は 'undefined name p2c_obj_slice' で失敗していた）
a = [1, 2, 3, 4]
b = a[1:3]
print(b)
c = [10, 20, 30][0:2]
print(c)
s = "abcdef"[2:5]
print(s)
t = (1, 2, 3, 4, 5)[1:4]
print(t)
r = range(10)[2:6]
print(repr(r))
print([a[0:2], a[2:4]])
print(len([1, 2, 3][0:2]), [1, 2, 3][0:2][1])
print(a[0:2] + a[2:4])
print(a[::-1], a[::2])
print("hello world"[::2])
