#!/bin/sh
# Scaffold a user plugin into user-plugins/{server,client}/<name>, or link an
# existing plugin directory into the tree.
#
#   scripts/new-plugin.sh <name> [--client]
#       Create a new plugin from templates/: CMakeLists.txt + a working
#       minimal plugin (server: one tool; client: one status segment). The
#       sources land INSIDE this repo, which is gitignored -- fine for a
#       quick experiment, but move them into a real (versioned) repo of your
#       own and re-register with --link once the plugin is worth keeping.
#
#   scripts/new-plugin.sh --link <plugin-dir> [--client] [--name NAME]
#       Register an existing plugin directory (one with a CMakeLists.txt
#       calling minimal_add_user_plugin) by SYMLINK, relative so the wiring
#       survives moving either repo. This is how a plugin repo that lives
#       outside the harness registers: one link per plugin, in the
#       user-plugins/{server,client} subtree matching its kind. A repo
#       holding both kinds links each plugin separately.
#
# Either way, next steps:
#   make build   -- builds the plugin and deploys it into the config dir
#   enable it in plugins.json (server) / client-plugins.json (client);
#   user plugins default disabled, and that is a trust boundary, not friction.

set -eu

REPO_ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TEMPLATES="$REPO_ROOT/templates"

say() { printf '%s\n' "new-plugin: $*"; }
die() { printf '%s\n' "new-plugin: error: $*" >&2; exit 1; }

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

name=''
kind=server
link_dir=''
want_name=''

while [ $# -gt 0 ]; do
    case "$1" in
        --client)   kind=client ;;
        --server)   kind=server ;;
        --link)     [ $# -ge 2 ] || die "--link needs a directory argument"
                    link_dir=$2; shift ;;
        --name)     [ $# -ge 2 ] || die "--name needs a value"
                    want_name=$2; shift ;;
        -h|--help)  usage; exit 0 ;;
        -*)         usage >&2; die "unknown option: $1" ;;
        *)          if [ -n "$name" ]; then usage >&2; die "unexpected argument: $1"; fi
                    name=$1 ;;
    esac
    shift
done

plugins_root="$REPO_ROOT/user-plugins"
kind_dir="$plugins_root/$kind"

# --- --link mode -------------------------------------------------------------

if [ -n "$link_dir" ]; then
    [ -n "$name" ] || name=$want_name
    [ -n "$name" ] || name=$(basename "$link_dir")
    case "$name" in
        [A-Za-z0-9][A-Za-z0-9_-]*) ;;
        *) die "plugin name '$name' is not a valid target name" ;;
    esac
    [ -d "$link_dir" ] || die "no such directory: $link_dir"
    [ -f "$link_dir/CMakeLists.txt" ] ||
        die "$link_dir has no CMakeLists.txt -- a plugin directory must call minimal_add_user_plugin"
    mkdir -p "$kind_dir"
    [ ! -e "$kind_dir/$name" ] && [ ! -L "$kind_dir/$name" ] ||
        die "user-plugins/$kind/$name already exists"

    abs_target=$(CDPATH='' cd -- "$link_dir" && pwd)
    if command -v python3 >/dev/null 2>&1; then
        rel=$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
              "$abs_target" "$kind_dir")
    else
        rel=$abs_target   # absolute symlink still works, it just survives moves less well
    fi
    ln -s "$rel" "$kind_dir/$name"
    say "linked user-plugins/$kind/$name -> $rel"
    say "next: make build   (then enable in plugins.json" \
        "(server) / client-plugins.json (client) -- user plugins start disabled)"
    exit 0
fi

# --- create mode -------------------------------------------------------------

[ -n "$name" ] || { usage >&2; die "a plugin name is required"; }
case "$name" in
    [a-z][a-z0-9_-]*) ;;
    *) die "plugin name '$name' must be lowercase alphanumeric (hyphen/underscore ok)" ;;
esac
[ -d "$TEMPLATES/$kind-plugin" ] || die "template missing: templates/$kind-plugin"
[ ! -e "$kind_dir/$name" ] && [ ! -L "$kind_dir/$name" ] ||
    die "user-plugins/$kind/$name already exists"

mkdir -p "$kind_dir/$name"
for file in "$TEMPLATES/$kind-plugin"/*; do
    sed "s/@PLUGIN_NAME@/$name/g" "$file" > "$kind_dir/$name/$(basename "$file")"
done

say "created user-plugins/$kind/$name (from templates/$kind-plugin)"
if [ "$kind" = client ]; then
    say "note: add {$name} to ui.json's status_format to place the segment"
fi
say "note: these sources are NOT versioned (user-plugins/ is gitignored); move the"
say "      plugin into a real repo and re-register with --link when it earns its keep."
say "next: make build   (then enable in plugins.json" \
    "(server) / client-plugins.json (client) -- user plugins start disabled)"
