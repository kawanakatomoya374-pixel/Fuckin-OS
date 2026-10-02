squares = {value: value * value for value in range(6) if value % 2 == 0}
print(squares)

last_wins = {value % 2: value for value in [2, 1, 4, 3]}
print(last_wins)

pairs = {left * 10 + right: left + right for left in [1, 2] for right in [3, 4] if left + right > 4}
print(pairs)
