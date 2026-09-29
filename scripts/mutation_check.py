"""Checks that the differential test actually catches bugs.

For each hand-written mutation (a small, plausible bug) this script copies the
engine headers to a scratch directory, applies the mutation, compiles
tests/difftest.cpp against the mutated headers and runs it. A mutation counts
as "caught" if the difftest exits non-zero (a reported mismatch or a crash).

Usage: python scripts/mutation_check.py [--cxx g++] [--ops 20000] [--out results]
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Mutation:
    name: str
    file: str  # path under include/lob/
    old: str
    new: str
    bug: str


MUTATIONS = [
    Mutation(
        "limit_boundary",
        "order_book.hpp",
        "taker_side == Side::Buy ? level_price <= limit : level_price >= limit",
        "taker_side == Side::Buy ? level_price < limit : level_price >= limit",
        "a buy order does not trade at exactly its limit price",
    ),
    Mutation(
        "lifo_queue",
        "order_book.hpp",
        "Order* maker = level->head;",
        "Order* maker = level->tail;",
        "newest order at a price trades first (LIFO instead of FIFO)",
    ),
    Mutation(
        "modify_same_qty_requeues",
        "order_book.hpp",
        "new_price == order->price && new_qty <= order->qty",
        "new_price == order->price && new_qty < order->qty",
        "modify to the same quantity sends the order to the back of the queue",
    ),
    Mutation(
        "modify_down_no_event",
        "order_book.hpp",
        "level->reduce(order, order->qty - new_qty);\n                listener_.on_level_update",
        "level->reduce(order, order->qty - new_qty);\n                if (false) listener_.on_level_update",
        "modify-down changes the book but emits no level update",
    ),
    Mutation(
        "fok_exact_liquidity_killed",
        "order_book.hpp",
        "liquidity_up_to(side, price, qty) < qty",
        "liquidity_up_to(side, price, qty) <= qty",
        "FOK is killed when available liquidity exactly equals its quantity",
    ),
    Mutation(
        "level_total_not_reduced_on_erase",
        "price_level.hpp",
        "        total_qty -= order->qty;\n        --count;",
        "        --count;",
        "removing an order does not subtract its quantity from the level total",
    ),
    Mutation(
        "map_best_bid_is_lowest",
        "map_levels.hpp",
        "side_ == Side::Buy ? &std::prev(levels_.end())->second : &levels_.begin()->second",
        "&levels_.begin()->second",
        "map book: best bid is the lowest bid instead of the highest",
    ),
    Mutation(
        "flat_best_not_improved",
        "flat_levels.hpp",
        "if (non_empty_ == 0 || is_better(index, best_))",
        "if (non_empty_ == 0)",
        "flat book: a new better price does not become the best level",
    ),
    Mutation(
        "idmap_no_backward_shift",
        "order_id_map.hpp",
        "if (dist_to_hole < dist_to_next)",
        "if (false && dist_to_hole < dist_to_next)",
        "hash map deletion leaves holes that cut probe chains",
    ),
]


def build_and_run(mutation: Mutation, cxx: str, ops: int, work: Path) -> tuple[bool, str]:
    include = work / "include"
    if include.exists():
        shutil.rmtree(include)
    shutil.copytree(ROOT / "include", include)
    target = include / "lob" / mutation.file
    source = target.read_text(encoding="utf-8")
    if source.count(mutation.old) != 1:
        raise SystemExit(f"{mutation.name}: pattern must occur exactly once in {mutation.file}")
    target.write_text(source.replace(mutation.old, mutation.new), encoding="utf-8")

    exe = work / ("difftest_mutant.exe" if sys.platform == "win32" else "difftest_mutant")
    cmd = [cxx, "-std=c++20", "-O2", f"-I{include}", f"-I{ROOT / 'tools'}", f"-I{ROOT / 'tests'}"]
    cmd += [str(ROOT / "tests" / "difftest.cpp"), "-o", str(exe)]
    if sys.platform == "win32":
        cmd.append("-static")  # avoid picking up a mismatched libstdc++ DLL
    subprocess.run(cmd, check=True)

    run = subprocess.run([str(exe), "--ops", str(ops), "--seeds", "1"], capture_output=True, text=True, timeout=600)
    output = (run.stderr or run.stdout).strip().splitlines()
    detail = output[0] if output else f"exit code {run.returncode}"
    return run.returncode != 0, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--ops", type=int, default=20_000, help="requests per scenario")
    parser.add_argument("--out", type=Path, help="directory for mutation_check.json/.md")
    args = parser.parse_args()

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for mutation in MUTATIONS:
            caught, detail = build_and_run(mutation, args.cxx, args.ops, Path(tmp))
            print(f"{'CAUGHT ' if caught else 'MISSED '} {mutation.name}: {detail}")
            rows.append({"mutation": mutation.name, "bug": mutation.bug, "caught": caught, "detail": detail})

    caught_count = sum(r["caught"] for r in rows)
    print(f"{caught_count}/{len(rows)} mutations caught by the difftest")
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        summary = {"ops_per_scenario": args.ops, "caught": caught_count, "total": len(rows), "mutations": rows}
        (args.out / "mutation_check.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        lines = [
            f"Difftest run per mutant: 4 scenarios x 1 seed x {args.ops} requests.",
            "",
            "| mutation | injected bug | caught | first report |",
            "|---|---|---|---|",
        ]
        for r in rows:
            detail = r["detail"].replace("|", "/")
            lines.append(f"| {r['mutation']} | {r['bug']} | {'yes' if r['caught'] else 'no'} | {detail} |")
        (args.out / "mutation_check.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0 if caught_count == len(rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
