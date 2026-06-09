#!/usr/bin/env bash
# Advisory clang-tidy pass over src/accessible/ after an edit. Prints findings but never
# fails the tool call: clang-tidy exits non-zero on any finding, and a non-zero hook exit
# would otherwise surface as a "Hook Error" in the conversation.
set -uo pipefail

f=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('tool_input',{}).get('file_path',''))" 2>/dev/null) || exit 0

[[ "$f" =~ src/accessible/.*\.(cpp|hpp|mm)$ ]] || exit 0

CLANG_TIDY=$(command -v clang-tidy 2>/dev/null || echo "/opt/homebrew/opt/llvm/bin/clang-tidy")
command -v "$CLANG_TIDY" >/dev/null 2>&1 || exit 0

RACK_ROOT=$(git -C "$(dirname "$f")" rev-parse --show-toplevel 2>/dev/null) || exit 0
DB="$RACK_ROOT/compile_commands.json"
[[ -f "$DB" ]] || exit 0

# Homebrew clang-tidy doesn't inherit Apple clang's implicit macOS SDK path, so it can't
# find the libc++ headers (<memory>, …) and every finding degrades to "file not found".
# Feed it the active SDK explicitly so the analysis is real.
SDK=$(xcrun --show-sdk-path 2>/dev/null) || SDK=""
SDK_ARGS=()
[[ -n "$SDK" ]] && SDK_ARGS=(--extra-arg=-isysroot --extra-arg="$SDK")

# Discard clang-tidy's own diagnostic noise (stderr); keep its findings (stdout). The
# trailing `|| true` keeps a non-zero clang-tidy exit from failing the hook.
"$CLANG_TIDY" -p "$DB" "${SDK_ARGS[@]}" "$f" 2>/dev/null || true
exit 0
