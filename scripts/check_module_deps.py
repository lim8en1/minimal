#!/usr/bin/env python3
"""Fails when a src/server module includes a header from a module it may not depend on.

The server is one CMake target, so layering is otherwise only a convention. This
resolves every `#include "minimal/..."` in src/server/<module>/ to the module
whose include/ tree provides it and checks the edge against ALLOWED below
(low layers first).

KNOWN_VIOLATIONS lists edges that exist today and are being removed (see the
module-boundaries plan). An entry that no longer occurs is itself an error, so
the list can only shrink.
"""
import os
import re
import sys

ALLOWED = {
    "llm": set(),
    "net": set(),
    "session": {"llm"},
    "policy": set(),
    "config": {"llm", "policy"},
    "sidecar": {"llm", "policy"},
    "hooks": {"config", "sidecar", "llm"},
    "tools": {"policy", "config", "hooks", "llm", "session", "sidecar"},
    "preset": {"config", "llm", "policy", "tools"},
    "commands": {"preset", "config", "tools", "llm", "policy"},
    "agent": {"preset", "commands", "config", "hooks", "llm", "policy", "session", "sidecar",
              "tools"},
    "plugins": {"agent", "commands", "config", "hooks", "llm", "net", "policy", "preset",
                "session", "sidecar", "tools"},
    "rpc": {"agent", "commands", "config", "hooks", "llm", "net", "plugins", "policy", "preset",
            "session", "sidecar", "tools"},
    "startup": {"agent", "commands", "config", "hooks", "llm", "net", "plugins", "policy",
                "preset", "rpc", "session", "sidecar", "tools"},
}

KNOWN_VIOLATIONS = {
    ("config", "tools"),
    ("sidecar", "tools"),
    ("hooks", "agent"),
    ("commands", "agent"),
    ("commands", "plugins"),
    ("agent", "plugins"),
    ("plugins", "rpc"),
}

INCLUDE = re.compile(r'#include\s+"(minimal/[^"]+)"')


def main(root):
    server = os.path.join(root, "src", "server")
    modules = [d for d in os.listdir(server) if os.path.isdir(os.path.join(server, d, "include"))]
    provider = {}
    for module in modules:
        inc = os.path.join(server, module, "include")
        for dirpath, _, files in os.walk(inc):
            for name in files:
                provider[os.path.relpath(os.path.join(dirpath, name), inc)] = module

    edges = {}
    for module in modules:
        for dirpath, _, files in os.walk(os.path.join(server, module)):
            for name in files:
                if not name.endswith((".cpp", ".hpp", ".h")):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, errors="ignore") as handle:
                    for line in handle:
                        match = INCLUDE.search(line)
                        target = provider.get(match.group(1)) if match else None
                        if target and target != module:
                            edges.setdefault((module, target), []).append(
                                f"{os.path.relpath(path, root)}: {match.group(1)}")

    failed = False
    for (src, dst), where in sorted(edges.items()):
        if dst in ALLOWED.get(src, set()) or (src, dst) in KNOWN_VIOLATIONS:
            continue
        failed = True
        print(f"forbidden dependency {src} -> {dst}:")
        for entry in where[:5]:
            print(f"    {entry}")
    for src, dst in sorted(KNOWN_VIOLATIONS):
        if (src, dst) not in edges:
            failed = True
            print(f"stale KNOWN_VIOLATIONS entry {src} -> {dst}: remove it from the list")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.getcwd()))
