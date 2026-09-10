#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_ROOT="$(dirname "$BUILD_DIR")"
TOOLS_DIR="$SOURCE_ROOT/tools"
DEST="$HOME/olli"

if [ -e "$DEST" ] && [ ! -d "$DEST" ]; then
    echo "Removing existing non-directory $DEST"
    rm -f "$DEST"
fi
mkdir -p "$DEST"

# Copy to a temp file then atomically mv into place, so this still works
# even if a build (e.g. olli itself) is currently running in a tmux pane.
install_file() {
    local src="$1" name="$2"
    local tmp
    tmp="$(mktemp "$DEST/.${name}.XXXXXX")"
    cp "$src" "$tmp"
    chmod --reference="$src" "$tmp"
    mv -f "$tmp" "$DEST/$name"
    echo "Installed $name"
}

install_file "$BUILD_DIR/olli" olli

for tool in hue clock presence; do
    install_file "$TOOLS_DIR/$tool/$tool" "$tool"
done

# rag_tool/rag_admin live nested under tools/rag/ rather than flat like the
# tools above, so they need their own install_file calls instead of joining
# the loop. rag_db itself has no standalone binary to install - it's a
# library the other two link against (the only executable in that
# directory, rag_db_test, is a test harness, not a deployable program).
install_file "$TOOLS_DIR/rag/rag_tool/rag_tool" rag_tool
install_file "$TOOLS_DIR/rag/rag_admin/rag_admin" rag_admin

install_file "$BUILD_DIR/start_session.sh" start_session.sh
cp -v "$BUILD_DIR/SETUP.txt" "$DEST/"

echo "Installed to $DEST"
