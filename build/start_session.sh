#!/usr/bin/env bash
set -euo pipefail

SESSION="olli"

if tmux attach -t "$SESSION" 2>/dev/null; then
    exit 0
fi

echo "No existing '$SESSION' session — starting tmux fresh (auto-restore will rebuild it)."
tmux
