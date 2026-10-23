words = ["banana", "kiwi", "apple", "fig"]
print(sorted(words, key=len))
print(min(words, key=len))
print(max(words, key=len))
print(sorted(words, key=len, reverse=True))
