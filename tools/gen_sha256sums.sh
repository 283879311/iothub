#!/usr/bin/env bash
# gen_sha256sums.sh — 重新生成工件目录的 SHA256SUMS（含镜像 bin/elf）
#
# 背景：artifacts/ 下曾三次出现 SHA256SUMS 只列日志、漏 iothub.bin/iothub.elf
# （def003 2026-11-05、ble_idle 2026-11-05、scan_v2 2026-11-06），而验收报告
# 引用该文件作为镜像 SHA 依据 —— 漏项会让取证链落空。
#
# 用法：
#   tools/gen_sha256sums.sh artifacts/scan_v2
#   tools/gen_sha256sums.sh            # 默认处理 artifacts/ 下含 iothub.bin 的目录
#
# 行为：
#   1. 全量重算：目录内除 SHA256SUMS 自身外的所有常规文件，按文件名排序；
#   2. 缺 bin/elf 视为错误（工件目录必须成对，否则拒绝写）；
#   3. 覆盖前保留 .bak，写后 sha256sum -c 自校验，失败则回滚。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

gen_one() {
  local dir="$1"
  [ -d "$dir" ] || { echo "跳过（不是目录）: $dir" >&2; return 0; }
  [ -f "$dir/iothub.bin" ] && [ -f "$dir/iothub.elf" ] || {
    echo "错误: $dir 缺 iothub.bin 或 iothub.elf，拒绝生成（镜像必须成对）" >&2
    return 1
  }

  local out="$dir/SHA256SUMS"
  local tmp
  tmp="$(mktemp "$dir/.SHA256SUMS.XXXXXX")"
  # -- 按名排序保证可重复；排除清单自身与临时文件
  ( cd "$dir" && LC_ALL=C ls -1 | grep -v -E '^(SHA256SUMS|\.SHA256SUMS\.)' | sort | xargs -r sha256sum -b ) > "$tmp"

  [ -f "$out" ] && cp "$out" "$out.bak"
  mv "$tmp" "$out"
  if ( cd "$dir" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ); then
    echo "$dir/SHA256SUMS  条目 $(wc -l < "$out") 项  （bin/elf 已含）"
  else
    echo "错误: $dir 自校验失败，已回滚" >&2
    [ -f "$out.bak" ] && mv "$out.bak" "$out"
    return 1
  fi
}

if [ "$#" -gt 0 ]; then
  for d in "$@"; do gen_one "$d"; done
else
  find "$ROOT/artifacts" -mindepth 1 -maxdepth 2 -type f -name iothub.bin -exec dirname {} \; |
    sort | while read -r d; do gen_one "$d"; done
fi
