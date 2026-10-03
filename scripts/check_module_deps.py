#!/usr/bin/env python3
"""Fails when a src/server module includes a header from a module it may not depend on.

The server core is one shared library, so layering needs a check of its own. This
resolves every `#include "minimal/..."` in src/server/<module>/ to the module
whose include/ tree provides it and checks the edge against the module's DEPS.

The allowed edges are the DEPS lists in each module's CMakeLists.txt, which is also
what the build links, so a header can neither be included without a declared
dependency nor be reachable by the compiler without one.
"""
import os
import re
import sys

DEPS_RE = re.compile(r"DEPS\s+([^\n]*)")


def declared_deps(server, modules):
    """DEPS lists from src/server/<module>/CMakeLists.txt: the single source of
    truth, shared with the build (minimal_server_module in src/server/CMakeLists.txt)."""
    allowed = {}
    for module in modules:
        path = os.path.join(server, module, "CMakeLists.txt")
        text = open(path).read() if os.path.exists(path) else ""
        match = DEPS_RE.search(text)
        allowed[module] = set(match.group(1).split()) if match else set()
    return allowed


INCLUDE = re.compile(r'#include\s+"(minimal/[^"]+)"')


def main(root):
    server = os.path.join(root, "src", "server")
    modules = [d for d in os.listdir(server) if os.path.isdir(os.path.join(server, d, "include"))]
    allowed = declared_deps(server, modules)
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
        if dst in allowed.get(src, set()):
            continue
        failed = True
        print(f"forbidden dependency {src} -> {dst}:")
        for entry in where[:5]:
            print(f"    {entry}")
    # A cycle in the declared graph would make the layering meaningless.
    state = {}

    def visit(node, stack):
        nonlocal failed
        if state.get(node) == 2:
            return
        if state.get(node) == 1:
            failed = True
            print("dependency cycle: " + " -> ".join(stack[stack.index(node):] + [node]))
            return
        state[node] = 1
        for dep in sorted(allowed.get(node, ())):
            visit(dep, stack + [node])
        state[node] = 2

    for module in sorted(modules):
        visit(module, [])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.getcwd()))
