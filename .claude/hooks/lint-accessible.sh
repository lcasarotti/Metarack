#!/usr/bin/env bash
set -euo pipefail

f=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('tool_input',{}).get('file_path',''))")

[[ "$f" =~ src/accessible/.*\.(cpp|hpp|mm)$ ]] || exit 0

CLANG_TIDY=$(command -v clang-tidy 2>/dev/null || echo "/opt/homebrew/opt/llvm/bin/clang-tidy")
command -v "$CLANG_TIDY" >/dev/null 2>&1 || exit 0

RACK_ROOT=$(git -C "$(dirname "$f")" rev-parse --show-toplevel 2>/dev/null) || exit 0
DB="$RACK_ROOT/compile_commands.json"
[[ -f "$DB" ]] || exit 0

"$CLANG_TIDY" -p "$DB" "$f" 2>/dev/null
