#!/bin/bash
# Build Magnify-PDVD shared libraries and fix duplicate LC_RPATH entries
# that cause dlopen failures on macOS 15+ with conda ROOT.
#
# Usage:
#   ./build.sh          # rebuild only files newer than their .so
#   ./build.sh --force  # force rebuild of all files

cd "$(dirname "$(readlink -f "$BASH_SOURCE")")"
MAGNIFY_ROOT="$(pwd)"
RPATH="/opt/anaconda3/envs/evn2/lib"
FORCE=0
[ "$1" = "--force" ] && FORCE=1

SOURCES=(
    event/BadChannels.cc
    event/RawWaveforms.cc
    event/Waveforms.cc
    event/Data.cc
    viewer/RmsAnalyzer.cc
    viewer/ViewWindow.cc
    viewer/ControlWindow.cc
    viewer/MainWindow.cc
    viewer/GuiController.cc
)

fix_rpath() {
    local so="$1"
    local count
    count=$(otool -l "$so" 2>/dev/null | grep -c "LC_RPATH" || echo 0)
    if [ "$count" -gt 1 ]; then
        install_name_tool -delete_rpath "$RPATH" "$so" 2>/dev/null || true
        install_name_tool -add_rpath    "$RPATH" "$so" 2>/dev/null || true
        echo "  fixed duplicate LC_RPATH"
    fi
}

needs_rebuild() {
    local src="$1"
    local so="${src%.cc}_cc.so"
    [ "$FORCE" = "1" ] && return 0
    [ ! -f "$so" ] && return 0
    [ "$src" -nt "$so" ] && return 0
    # also check the header alongside the source
    local hdr="${src%.cc}.h"
    [ -f "$hdr" ] && [ "$hdr" -nt "$so" ] && return 0
    return 1
}

# Build a ROOT script that loads all previously-built .so deps, then compiles the target
compile_one() {
    local src_path="$1"
    shift
    local deps=("$@")   # absolute .so paths already built

    local load_stmts=""
    for dep in "${deps[@]}"; do
        load_stmts+="gSystem->Load(\"$dep\"); "
    done

    # Suppress the ACLiC "created shared library" info line and the _main error
    root -l -b -q -e "
        gSystem->AddIncludePath(\"-I${MAGNIFY_ROOT}/event -I${MAGNIFY_ROOT}/viewer\");
        ${load_stmts}
        gSystem->CompileMacro(\"${src_path}\", \"k\");
    " 2>&1 | grep -v "^$" \
            | grep -v "^root \[" \
            | grep -v "duplicate LC_RPATH" \
            | grep -v "_main.*referenced from" \
            | grep -v "implicit entry/start" \
            | grep -v "symbol(s) not found" \
            | grep -v "linker command failed" \
            | grep -v "clang-.*error:" \
            | grep -v "^ld: " \
            | grep -v "^$" \
            || true
}

echo "Building Magnify-PDVD libraries (force=$FORCE)..."
echo ""

built_so=()

for src in "${SOURCES[@]}"; do
    src_path="${MAGNIFY_ROOT}/${src}"
    so_path="${src_path%.cc}_cc.so"

    if ! needs_rebuild "$src_path"; then
        echo "  up-to-date: $src"
        fix_rpath "$so_path"   # fix RPATH even on cached .so (git pull resets nothing)
        built_so+=("$so_path")
        continue
    fi

    echo -n "  compiling: $src ... "

    # Delete stale .so and ACLiC artifacts so CompileMacro always rebuilds
    rm -f "$so_path" "${so_path%.so}"_ACLiC_* "${so_path%.so}".d \
          "${src_path%.cc}_cc_ACLiC_"* "${src_path%.cc}_cc.d" 2>/dev/null || true

    compile_one "$src_path" "${built_so[@]}"

    if [ ! -f "$so_path" ]; then
        echo "FAILED (no .so produced)" >&2
        exit 1
    fi

    fix_rpath "$so_path"
    built_so+=("$so_path")
    echo "OK"
done

echo ""
echo "All libraries ready."
echo "Run:  ./magnify.sh <file.root>"
