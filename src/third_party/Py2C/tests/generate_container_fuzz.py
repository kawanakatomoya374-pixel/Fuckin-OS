#!/usr/bin/env python3
"""Generate deterministic, side-effect-focused dict/set differential programs."""

from __future__ import annotations

import argparse
import random
from pathlib import Path

GENERATOR_VERSION = "container-fuzz-v1"
KEYS = ["a", "b", "c", "d", "e"]
VALUES = [-3, -1, 0, 1, 2, 5]
SET_VALUES = [-4, -2, -1, 0, 1, 2, 4, 7]


def literal_list(values: list[object]) -> str:
    return repr(values)


def emit_operation(rng: random.Random, index: int) -> list[str]:
    op = rng.randrange(11)
    key = rng.choice(KEYS)
    value = rng.choice(VALUES)
    payload = [rng.choice(SET_VALUES) for _ in range(rng.randrange(0, 5))]
    lines: list[str] = []
    if op == 0:
        lines.append(f"d[{key!r}] = {value}")
    elif op == 1:
        lines.append(f"d.setdefault({key!r}, {value})")
    elif op == 2:
        lines.append(f"d.pop({key!r}, {value})")
    elif op == 3:
        pairs = [(rng.choice(KEYS), rng.choice(VALUES)) for _ in range(rng.randrange(0, 4))]
        lines.append(f"d.update({pairs!r})")
    elif op == 4:
        from_keys = [rng.choice(KEYS) for _ in range(rng.randrange(0, 5))]
        lines.append(f"d = dict.fromkeys({from_keys!r}, {value})")
    elif op == 5:
        lines.append(f"s.add({value})")
    elif op == 6:
        lines.append(f"s.discard({value})")
    elif op == 7:
        lines.append(f"s.update({literal_list(payload)})")
    elif op == 8:
        lines.append(f"s.intersection_update({literal_list(payload)})")
    elif op == 9:
        lines.append(f"s.difference_update({literal_list(payload)})")
    else:
        lines.append(f"s.symmetric_difference_update({literal_list(payload)})")
    probe = [rng.choice(SET_VALUES) for _ in range(rng.randrange(0, 5))]
    lines.append(
        f"print('F{index:03d}', list(d.items()), sorted(s), "
        f"s.issubset({literal_list(probe)}), s.issuperset({literal_list(probe)}), "
        f"s.isdisjoint({literal_list(probe)}))"
    )
    return lines


def generate(seed: int, cases: int) -> str:
    rng = random.Random(seed)
    lines = [
        "d = {'seed': 0}",
        "s = set()",
        "print('META', 'container-fuzz-v1')",
    ]
    for index in range(cases):
        lines.extend(emit_operation(rng, index))
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate deterministic dict/set fuzz source")
    parser.add_argument("--seed", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--cases", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.cases < 1 or args.cases > 1000:
        raise SystemExit("--cases must be between 1 and 1000")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.seed, args.cases), encoding="utf-8")
    print(f"generated {GENERATOR_VERSION}: seed={args.seed} cases={args.cases} output={args.output}")


if __name__ == "__main__":
    main()
