#!/usr/bin/env python3
"""Lists numbers that may be hidden judgment calls (ARCHITECTURE.md "Policy constants").

Two passes over src/ and plugins/ (tests excluded):

  literal  -- clang-tidy readability-magic-numbers (config in .clang-tidy): a bare
              literal in an expression or a non-const initializer.
  named    -- a const/constexpr k-name initialized with a number OUTSIDE a
              policy.hpp. clang-tidy exempts these by design, which is exactly
              how a judgment call hides behind a name.

Every hit must end up as an external fact (named or cited where used), a
derivation written as one, or a policy.hpp entry. Until the sweep is finished
this is an inventory tool, not a gate: it exits 1 whenever anything is listed.

Usage:
  check_policy_constants.py [--build-dir build] [--unit PREFIX] [--tsv] [--named-only]
"""
import argparse
import os
import platform
import re
import shutil
import signal
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_RE = re.compile(r"\.(cpp|cc|hpp|h)$")
TIDY_RE = re.compile(r"^(/\S+):(\d+):\d+: warning: (\S+) is a magic number")
NAMED_RE = re.compile(
    r"\b(?:constexpr|const)\b[^=;(]*?\b(k[A-Z]\w*)\s*(?:=|\{)\s*([^;]*)")
HAS_NUMBER = re.compile(r"(?<![\w.])\d")
STRING_LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')


def unit_of(rel):
    """The review unit a path belongs to: a plugin, a server module, common, or
    a client area (include/minimal/client/<area>/ folds into <area>)."""
    parts = rel.split("/")
    if parts[0] == "plugins":
        return "/".join(parts[:3])
    if parts[1] == "server":
        return "src/server/" + parts[2]
    if parts[1] == "client":
        rest = parts[2:]
        if rest[:3] == ["include", "minimal", "client"]:
            rest = rest[3:]
        return "src/client/" + rest[0] if len(rest) > 1 else "src/client"
    return "src/" + parts[1]


def source_files():
    out = subprocess.run(["git", "ls-files", "src", "plugins"], cwd=ROOT,
                         capture_output=True, text=True, check=True).stdout
    return [f for f in out.split() if SOURCE_RE.search(f)]


def find_tool(name):
    found = shutil.which(name)
    if found:
        return found
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin"):
        candidate = os.path.join(prefix, name)
        if os.path.exists(candidate):
            return candidate
    sys.exit(f"{name} not found (install LLVM, e.g. `brew install llvm`)")


def literal_pass(build_dir, unit, jobs):
    runner, tidy = find_tool("run-clang-tidy"), find_tool("clang-tidy")
    files = r"^" + re.escape(ROOT) + "/" + (re.escape(unit) if unit else r"(src|plugins)/") \
        + r".*\.(cpp|cc)$"
    cmd = [sys.executable, runner, "-p", build_dir, "-quiet", "-j", str(jobs),
           "-clang-tidy-binary", tidy, files]
    if platform.system() == "Darwin":
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                             text=True).stdout.strip()
        if sdk:
            cmd.insert(2, "-extra-arg=-isysroot" + sdk)
    output = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout
    hits = set()
    for line in output.splitlines():
        match = TIDY_RE.match(line)
        if not match:
            continue
        path = os.path.relpath(match.group(1), ROOT)
        if path.startswith(("src/", "plugins/")):
            hits.add((path, int(match.group(2)), "literal", match.group(3)))
    return hits


def named_pass(files):
    hits = set()
    for rel in files:
        if os.path.basename(rel) == "policy.hpp":
            continue
        with open(os.path.join(ROOT, rel), errors="replace") as handle:
            for number, line in enumerate(handle, 1):
                # Strings first: a ';' or '//' inside one is not code.
                code = STRING_LITERAL.sub('""', line).split("//", 1)[0]
                match = NAMED_RE.search(code)
                if match and HAS_NUMBER.search(match.group(2)):
                    hits.add((rel, number, "named", match.group(1)))
    return hits


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)  # `| head` is a normal consumer
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    parser.add_argument("--unit", default="", help="path prefix, e.g. plugins/server/memory")
    parser.add_argument("--tsv", action="store_true", help="every hit, not per-unit counts")
    parser.add_argument("--named-only", action="store_true", help="skip clang-tidy")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    files = [f for f in source_files() if f.startswith(args.unit)]
    hits = named_pass(files)
    if not args.named_only:
        hits |= {h for h in literal_pass(args.build_dir, args.unit, args.jobs)
                 if h[0].startswith(args.unit)}

    if args.tsv:
        lines = {}
        for path, number, kind, what in sorted(hits):
            if path not in lines:
                with open(os.path.join(ROOT, path), errors="replace") as handle:
                    lines[path] = handle.read().splitlines()
            text = lines[path][number - 1].strip()
            print(f"{unit_of(path)}\t{path}:{number}\t{kind}\t{what}\t{text}")
    else:
        counts = {}
        for path, _, kind, _ in hits:
            entry = counts.setdefault(unit_of(path), {"literal": 0, "named": 0})
            entry[kind] += 1
        print(f"{'unit':40} {'literal':>8} {'named':>6}")
        for unit, entry in sorted(counts.items(), key=lambda kv: -sum(kv[1].values())):
            print(f"{unit:40} {entry['literal']:8} {entry['named']:6}")
        print(f"{'total':40} {sum(e['literal'] for e in counts.values()):8} "
              f"{sum(e['named'] for e in counts.values()):6}")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
