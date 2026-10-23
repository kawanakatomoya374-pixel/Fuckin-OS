values = [1, 2, 3]
if (count := len(values)) == 3:
    print("count", count)

index = 0
seen = []
while (current := values[index]) < 3:
    seen.append(current)
    index += 1
print(seen, current)

scaled = [twice for value in values if (twice := value * 2) > 2]
print(scaled, twice)
print(total := count + twice)
