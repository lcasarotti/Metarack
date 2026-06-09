#!/usr/bin/env bash
set -euo pipefail

f=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('tool_input',{}).get('file_path',''))")

[[ "$f" =~ \.(cpp|hpp)$ ]] || exit 0
command -v astyle >/dev/null 2>&1 || exit 0

astyle --suffix=none --options="/c/Rack/.astylerc" "$f"
