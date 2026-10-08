#!/usr/bin/env python3
"""Fails when a src/server module includes a header from a module it may not depend on.

With --plugins, checks plugins/ against the SDKs instead: every
`#include "minimal/..."` in a plugin must be the plugin's own header, a header
listed in scripts/sdk_headers.txt (server) or scripts/client_sdk_headers.txt
(client). Anything else is a missing extension point or code on the wrong side
of the boundary.

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


def read_list(path):
    """Non-comment, non-blank lines of a manifest file."""
    if not os.path.exists(path):
        return []
    lines = []
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if line:
            lines.append(line)
    return lines


def check_plugins(root):
    """Server plugins against sdk_headers.txt, client plugins against
    client_sdk_headers.txt."""
    failed = False
    headers = project_headers(root)
    for side, manifest in (("server", "sdk_headers.txt"), ("client", "client_sdk_headers.txt")):
        sdk = set(read_list(os.path.join(root, "scripts", manifest)))
        failed |= check_plugin_tree(root, os.path.join(root, "plugins", side), sdk)
        failed |= check_sdk_closure(root, sdk, headers, manifest, strict=True)
    return 1 if failed else 0


def project_headers(root):
    """Every project header by its include path ("minimal/..."), from each
    `include` directory under src/."""
    found = {}
    for dirpath, dirnames, files in os.walk(os.path.join(root, "src")):
        if os.path.basename(dirpath) != "include":
            continue
        for inner, _, names in os.walk(dirpath):
            for name in names:
                if name.endswith((".hpp", ".h")):
                    path = os.path.join(inner, name)
                    found[os.path.relpath(path, dirpath)] = path
        dirnames[:] = []
    return found


def check_sdk_closure(root, sdk, headers, manifest, strict):
    """An SDK header may include only SDK headers: one that pulls in a core
    header hands every plugin that header too, and the plugin-side check
    above (a plugin's own includes) never sees it."""
    failed = False
    for header, path in sorted(headers.items()):
        if not in_sdk(header, sdk):
            continue
        with open(path, errors="ignore") as handle:
            for line in handle:
                match = INCLUDE.search(line)
                if match and not in_sdk(match.group(1), sdk):
                    failed |= strict
                    print(f"{'' if strict else 'warning: '}SDK header includes a non-SDK header "
                          f"({manifest}): {os.path.relpath(path, root)}: {match.group(1)}")
    return failed


def in_sdk(header, sdk):
    """A manifest line names one header, or -- ending in '/' -- a directory
    whose every header is SDK."""
    return header in sdk or any(e.endswith("/") and header.startswith(e) for e in sdk)


def check_plugin_tree(root, plugins_root, sdk):
    failed = False
    for plugin in sorted(os.listdir(plugins_root)):
        plugin_dir = os.path.join(plugins_root, plugin)
        if not os.path.isdir(plugin_dir):
            continue
        own_include = os.path.join(plugin_dir, "include")
        for dirpath, _, files in os.walk(plugin_dir):
            for name in files:
                if not name.endswith((".cpp", ".hpp", ".h")):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, errors="ignore") as handle:
                    for line in handle:
                        match = INCLUDE.search(line)
                        if not match:
                            continue
                        header = match.group(1)
                        if os.path.exists(os.path.join(own_include, header)) or in_sdk(header, sdk):
                            continue
                        failed = True
                        print(f"not in the SDK: {os.path.relpath(path, root)}: {header}")
    return failed


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--plugins"]
    root = args[0] if args else os.getcwd()
    sys.exit(check_plugins(root) if "--plugins" in sys.argv[1:] else main(root))
