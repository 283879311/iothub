#!/usr/bin/env bash
# 可证伪性检查：把 operation.c 中 `if (op->expiring)` 变异为 `if (0)`（不修改产品代码，
# 只在临时目录生成变异副本），重新编译并运行 S1。正确实现的闸门被去掉后，
# 测试必须在 A1 上失败；若变异体仍通过，说明测试无区分力。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sed 's/if (op->expiring) {/if (0) {/g' \
    "$ROOT/main/operation.c" > "$TMP/operation_mutant.c"
N=$(grep -c "if (0) {" "$TMP/operation_mutant.c" || true)
if [ "${N:-0}" -lt 1 ]; then
  echo "[FALSIFY] 变异未生效（产品代码可能已改动 'if (op->expiring)' 写法）" >&2
  exit 2
fi

cc -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
   -I"$HERE/stub" -I"$ROOT/main" -DAPP_C1_TEST_HOOKS=1 \
   -o "$TMP/mutant" \
   "$HERE/test_operation_window.c" "$HERE/rtos_shim.c" \
   "$TMP/operation_mutant.c" "$ROOT/main/operation_state.c"

set +e
OPS_REPS=1 "$TMP/mutant" > "$TMP/out.txt" 2>&1
RC=$?
set -e

if [ "$RC" -eq 0 ]; then
  echo "[FALSIFY] FAILED：闸门被去掉后测试仍通过——测试没有区分力" >&2
  cat "$TMP/out.txt" >&2
  exit 1
fi
if ! grep -q "ASSERT A1=FAIL" "$TMP/out.txt"; then
  echo "[FALSIFY] FAILED：变异体失败但不是 A1（失败点不对）" >&2
  cat "$TMP/out.txt" >&2
  exit 1
fi
echo "[FALSIFY] OK：变异体（expiring 闸置空，共 ${N} 处）在 A1 失败，测试可区分缺陷行为"
