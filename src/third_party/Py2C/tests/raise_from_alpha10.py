try:
    try:
        raise ValueError("root")
    except ValueError as cause:
        raise RuntimeError("outer") from cause
except RuntimeError:
    print("caught")
