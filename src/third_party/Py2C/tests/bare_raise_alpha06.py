try:
    try:
        raise ValueError("again")
    except ValueError:
        raise
except ValueError:
    print("reraised")

try:
    raise
except RuntimeError:
    print("no-active")
