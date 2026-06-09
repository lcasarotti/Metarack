#!/usr/bin/env bash
set -euo pipefail

f=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('tool_input',{}).get('file_path',''))" 2>/dev/null) || exit 0

[[ "$f" =~ \.(cpp|hpp)$ ]] || exit 0
command -v astyle >/dev/null 2>&1 || exit 0

RACK_ROOT=$(git -C "$(dirname "$f")" rev-parse --show-toplevel 2>/dev/null) || exit 0
ASTYLERC="$RACK_ROOT/.astylerc"
[[ -f "$ASTYLERC" ]] || exit 0

astyle --suffix=none --options="$ASTYLERC" "$f"
