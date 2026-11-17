#!/usr/bin/env python3
"""Turn a macOS `sample` report into something you can actually read.

`sample` gives a call tree with fully-qualified C++ template names, which is
noise for finding hot code. This script rebuilds the tree, then prints:

  * samples per thread (did one thread do all the work?)
  * how much of the time is idle (kevent / ulock / join) vs real work
  * hottest functions by *self* time (where the CPU actually is)
  * hottest functions by *inclusive* time (which paths to attack)
  * a folded-stack file for flamegraphs

Usage:
    python3 tools/prof.py sample.txt [--top N] [--folded out.folded] [--width N]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from dataclasses import dataclass, field

# Frame lines look like:
#     +   3228 main  (in edge-proxy-asio) + 320  [0x104aba190]
#     ! : | + 667 foo  (in edge-proxy-asio) + 300
# The column at which the sample count starts encodes the nesting depth, so we
# rebuild the tree from that instead of trying to interpret +/:/!/| markers.
FRAME_RE = re.compile(
    r"^(?P<prefix>[\s|:+!]*?)(?P<count>\d+) (?P<sym>.+?)"
    r"(?:\s+\(in (?P<mod>[^)]*)\)"
    r"(?: \+ [\d,]+)?"
    r"(?:\s+\[[^\]]+\])?"
    r")?$"
)

GENERIC = {"decltype", "std::enable_if", "std::declval", "void", "operator()"}
HEADER_RE = re.compile(r"Total number of samples:\s*(\d+)")

# Frames that mean "not running" on this platform. kevent is the async event
# loop waiting for I/O; ulock/psynch are thread joins and lock waits.
IDLE = {
    "kevent",
    "__ulock_wait",
    "__ulock_wait2",
    "__psynch_cvwait",
    "__psynch_mutexwait",
    "_pthread_join",
    "start_wqthread",
    "mach_msg2_trap",
}


@dataclass
class Frame:
    sym: str
    module: str
    count: int
    col: int
    parent: "Frame | None" = None
    children: list["Frame"] = field(default_factory=list)

    @property
    def self_samples(self) -> int:
        return max(0, self.count - sum(c.count for c in self.children))


def shorten(sym: str) -> str:
    """Collapse a mangle-flavoured C++ signature down to a readable name."""
    out = []
    depth = 0
    for ch in sym:
        if ch == "<":
            depth += 1
            continue
        if ch == ">":
            depth = max(0, depth - 1)
            continue
        if depth == 0:
            out.append(ch)
    s = "".join(out)
    s = re.sub(r"\(.*$", "", s)  # drop trailing argument list
    s = re.sub(r"\s+", " ", s).strip()
    s = s.replace("void ", "").replace("const &", "& ")
    if s in GENERIC:
        # mp11 / enable_if machinery: the useful name was inside the templates
        return "(compiler-generated glue)"
    return s or sym


def truncate(s: str, width: int) -> str:
    if len(s) <= width:
        return s
    return s[: width - 1] + "\u2026"


def parse(path: str) -> tuple[list[Frame], int]:
    roots: list[Frame] = []
    stack: list[Frame] = []
    in_graph = False
    total = 0

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if line.startswith("Call graph:"):
                in_graph = True
                continue
            if not in_graph:
                m = HEADER_RE.search(line)
                if m:
                    total = int(m.group(1))
                continue
            if not line.strip() or not line[0].isspace():
                # next section ("Total number in stack ...") -- stop here
                break
            if line.strip().startswith("Binary Images"):
                break

            m = FRAME_RE.match(line.rstrip("\n"))
            if not m:
                continue
            col = m.start("count")
            frame = Frame(
                sym=m.group("sym").strip(),
                module=(m.group("mod") or "thread").strip(),
                count=int(m.group("count")),
                col=col,
            )

            while stack and stack[-1].col >= col:
                stack.pop()
            if stack:
                frame.parent = stack[-1]
                stack[-1].children.append(frame)
            else:
                roots.append(frame)
            stack.append(frame)

    if not total:
        total = sum(r.count for r in roots)
    return roots, total


def walk(roots: list[Frame]):
    for r in roots:
        yield r
        yield from walk(r.children)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("report", help="text output of /usr/bin/sample")
    ap.add_argument("--top", type=int, default=15, help="rows per table (default 15)")
    ap.add_argument("--width", type=int, default=88, help="symbol width (default 88)")
    ap.add_argument("--folded", help="write folded stacks here for flamegraphs")
    ap.add_argument(
        "--module",
        help="only count frames from this module (e.g. edge-proxy-asio)",
    )
    args = ap.parse_args()

    roots, parsed_total = parse(args.report)
    if not roots:
        print(f"no call graph found in {args.report}", file=sys.stderr)
        return 1

    def is_idle_waiter(root: Frame) -> bool:
        """True for a thread that only exists to join/wait on others.

        `main()` in the proxy spawns worker threads and then joins them, so
        its whole stack is main -> thread::join -> __ulock_wait. Counting that
        thread doubles the denominator and makes every other number look half
        as big.
        """
        if "main-thread" not in root.sym:
            return False
        idle_self = sum(c.self_samples for c in walk([root]) if shorten(c.sym) in IDLE)
        return root.count > 0 and idle_self >= 0.95 * root.count

    waiters = [r for r in roots if is_idle_waiter(r)]
    active = [r for r in roots if r not in waiters] or roots
    # Percentages are relative to threads that can actually do work.
    total = sum(r.count for r in active) or parsed_total

    # short name -> counts (computed over working threads only)
    self_by_name: Counter[str] = Counter()
    incl_by_name: Counter[str] = Counter()
    module_of: dict[str, str] = {}

    def name_of(f: Frame) -> str:
        return truncate(shorten(f.sym), 200)

    def counted(f: Frame) -> bool:
        return not args.module or f.module == args.module

    for f in walk(active):
        if not counted(f):
            continue
        name = name_of(f)
        self_by_name[name] += f.self_samples
        module_of.setdefault(name, f.module)

    # Inclusive time: a sample counts toward a frame if that frame is anywhere
    # on its stack. Dedupe per stack so re-entrant/recursive frames cannot
    # exceed 100%.
    def accumulate_inclusive(frame: Frame, path: list[str]) -> None:
        name = name_of(frame)
        here = path + [name] if counted(frame) else path
        self_n = frame.self_samples
        if self_n:
            for unique in set(here):
                incl_by_name[unique] += self_n
        for child in frame.children:
            accumulate_inclusive(child, here)

    for root in active:
        accumulate_inclusive(root, [])
    for name, n in incl_by_name.items():
        module_of.setdefault(name, "")

    def pct(n: int, denom: int | None = None) -> str:
        d = denom if denom is not None else total
        return f"{100.0 * n / d:5.1f}%" if d else "  n/a"

    def table(title: str, counter: Counter[str], what: str) -> None:
        print(f"\n{title} ({what})")
        print("-" * (args.width + 18))
        rows = [r for r in counter.most_common(args.top) if r[1] > 0]
        if not rows:
            print("  (nothing above the sampling threshold)")
            return
        peak = rows[0][1]
        for name, n in rows:
            bar = "#" * max(1, round(24 * n / peak))
            print(
                f"  {pct(n)}  {truncate(name, args.width):<{args.width}}  {n:>5}  {bar}"
            )

    print(f"\nprofile: {args.report}")
    print(f"samples: {total} from {len(active)} working thread(s), ~1 ms apart")
    if waiters:
        labels = ", ".join(
            re.sub(r"DispatchQueue_\d+: ", "", re.sub(r"\s+", " ", w.sym))
            for w in waiters
        )
        print(f"excluded: idle waiter thread(s) blocked in join: {labels}")

    print("\nper-thread samples")
    print("-" * (args.width + 18))
    busy_by_thread: dict[int, int] = {}
    for t in active:
        label = re.sub(r"\s+", " ", t.sym)
        label = re.sub(r"DispatchQueue_\d+: ", "", label)
        idle_here = sum(c.self_samples for c in walk([t]) if shorten(c.sym) in IDLE)
        busy_here = max(0, t.count - idle_here)
        busy_by_thread[id(t)] = busy_here
        busy_pct = 100.0 * busy_here / t.count if t.count else 0.0
        print(
            f"  {100.0 * t.count / total:5.1f}% of all  {t.count:>5} samples  "
            f"{busy_pct:5.1f}% busy  {truncate(label, args.width - 32)}"
        )

    idle = sum(n for name, n in self_by_name.items() if name in IDLE)
    busy = total - idle
    print(
        f"\ntime split: {pct(busy).strip():>6} runnable, "
        f"{pct(idle).strip():>6} blocked/idle "
        f"(kevent/ulock = waiting for I/O or locks, normal for an async server)"
    )

    if len(active) > 1 and busy > 0:
        busiest = max(active, key=lambda t: busy_by_thread[id(t)])
        share = busy_by_thread[id(busiest)] / busy
        if share >= 0.6:
            print(
                f"  note: one thread holds {share:.0%} of all runnable samples "
                f"({truncate(re.sub(r'\s+', ' ', busiest.sym), 40)}) -- "
                f"adding worker threads only helps if they share the accept loop"
            )

    table("hot functions by SELF time", self_by_name, "where the CPU is")
    table("hot functions by INCLUSIVE time", incl_by_name, "which call path to attack")

    # Kernel-side cost is the usual answer for a syscall-heavy proxy.
    syscalls = Counter(
        {
            name: n
            for name, n in self_by_name.items()
            if module_of.get(name, "").startswith("libsystem_kernel")
        }
    )
    if syscalls:
        table("syscall / kernel time", syscalls, "self time inside libsystem_kernel")

    if args.folded:
        with open(args.folded, "w") as fh:

            def emit(frame: Frame, path: list[str]) -> None:
                name = truncate(shorten(frame.sym).replace(";", ","), 200)
                here = path + [name]
                if frame.self_samples:
                    fh.write(";".join(here) + f" {frame.self_samples}\n")
                for child in frame.children:
                    emit(child, here)

            for r in roots:
                emit(r, [])
        print(f"\nfolded stacks: {args.folded}")
        print(
            "  flamegraph:  flamegraph.pl --title 'edge-proxy' {} > out.svg".format(
                args.folded
            )
        )
        print("  or drop the file on https://www.speedscope.app")

    return 0


if __name__ == "__main__":
    sys.exit(main())
