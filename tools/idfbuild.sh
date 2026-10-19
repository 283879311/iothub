#!/usr/bin/env bash
# ============================================================================
# 统一构建/烧录入口(问题与版本优化.md 1.3-18)
#
# 解决三类"每次都得重新发现"的环境问题:
#   1) 本机离线安装: `$IDF_PATH/export.sh` 不可用(缺 esp-rom-elfs 安装记录),
#      EIM 的 activate 脚本又**不能被子脚本 source**(它自认被直接执行而退出)→
#      这里直接拼 PATH(用 glob 兼容工具版本升级),不依赖任何 source。
#   2) VS Code CMake 扩展会在 build/ 写 `Unix Makefiles` 的 CMakeCache,
#      而 ESP-IDF 要用 Ninja → `idf.py build` 直接报生成器不匹配;
#      更隐蔽的是它可能挂着 `cmake --debugger` **占住 build/**,此时 idf.py 会静默卡住。
#      本脚本自动清除生成器不匹配的缓存,并对占位进程给出 PID 与处置建议。
#   3) 让"构建/烧录/监视"都是一条命令(默认动作 build)。
#
# 用法:
#   tools/idfbuild.sh                 # 默认 profile = NimBLE
#   tools/idfbuild.sh flash           # 烧录(可后接 -p /dev/ttyUSB0 等 idf.py 参数)
#   tools/idfbuild.sh monitor
#   tools/idfbuild.sh cache-guard     # 只清理被污染的 build/ 缓存
#   tools/idfbuild.sh version         # 打印环境定位结果
#   IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build   # Bluedroid 回退 profile
# 可覆盖环境: IDF_PATH / IDF_TOOLS_PATH / IDF_BUILD_DIR
# ============================================================================
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${IDF_BUILD_DIR:-$PROJ_DIR/build}"
TOOLS="${IDF_TOOLS_PATH:-$HOME/.espressif/tools}"

# ---- 1.3-62 profile 护栏:默认 NimBLE;build-bluedroid = Bluedroid 回退 ----
# 默认 build/ 使用项目根 sdkconfig(NimBLE,1.3-62 起按稳定性优先切换)。
# 非默认目录仅允许 build-bluedroid:以 sdkconfig.bluedroid 为种子(保留 1.3-39/47
# 的 ACL=1/SMP 关/GATT profiles=4 等裁剪),构建结束恢复根 sdkconfig,
# 避免 idf.py 把默认 NimBLE 配置写回/污染。
if [ -n "${IDF_BUILD_DIR:-}" ] && [ "$BUILD_DIR" != "$PROJ_DIR/build" ]; then
    if [ "$(basename "$BUILD_DIR")" != "build-bluedroid" ]; then
        echo "!! 默认 profile 已是 NimBLE;非默认目录仅支持 build-bluedroid(Bluedroid 回退)" >&2
        exit 2
    fi
    if [ ! -f "$PROJ_DIR/sdkconfig.bluedroid" ]; then
        echo "!! 缺少 sdkconfig.bluedroid(Bluedroid 回退 profile 种子)" >&2
        exit 1
    fi
    SDKCONFIG_BACKUP="$(mktemp "$PROJ_DIR/.sdkconfig.XXXXXX")"
    cp "$PROJ_DIR/sdkconfig" "$SDKCONFIG_BACKUP"
    cp "$PROJ_DIR/sdkconfig.bluedroid" "$PROJ_DIR/sdkconfig"
    trap 'cp "$SDKCONFIG_BACKUP" "$PROJ_DIR/sdkconfig"; rm -f "$SDKCONFIG_BACKUP"' EXIT
fi

# ---- 1) 定位 ESP-IDF ----
IDF_ROOT="${IDF_PATH:-}"
if [ -z "$IDF_ROOT" ]; then
    IDF_ROOT="$(ls -d "$HOME"/.espressif/v*/esp-idf 2>/dev/null | sort -V | tail -1 || true)"
fi
if [ -z "$IDF_ROOT" ] || [ ! -d "$IDF_ROOT" ]; then
    echo "!! 找不到 ESP-IDF:请设 IDF_PATH,或在 ~/.espressif/ 下安装(期望 v*/esp-idf)" >&2
    exit 1
fi
IDF_VER="$(basename "$(dirname "$IDF_ROOT")")"          # 形如 v6.0.2
export IDF_PATH="$IDF_ROOT"
export IDF_TOOLS_PATH="$TOOLS"
export ESP_IDF_VERSION="${ESP_IDF_VERSION:-${IDF_VER#v}}"  # idf.py 依赖它(否则 Version.coerce(None) 崩)
# idf.py 默认去 python_env/idfX.Y_py3.Z_env 找解释器(本机离线安装里不存在),
# 必须显式指向 EIM 实际创建的 venv,否则会打印 "... doesn't exist!" 并退回系统 python。
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$TOOLS/python/$IDF_VER/venv}"

# ---- 2) 拼 PATH(不 source 任何脚本) ----
PATH_ADD=()
add_path() { [ -d "$1" ] && PATH_ADD+=("$1"); }
add_path "$IDF_ROOT/tools"                                # idf.py 本体
add_path "$TOOLS/python/$IDF_VER/venv/bin"                # IDF 自带 python
# 工具目录有两种布局:EIM 用 <root>/{ninja,cmake,...};老式安装用 <root>/tools/{...}。两种都试。
for base in "$TOOLS" "$TOOLS/tools"; do
    for d in "$base"/xtensa-esp-elf/*/xtensa-esp-elf/bin \
             "$base"/riscv32-esp-elf/*/riscv32-esp-elf/bin \
             "$base"/ninja/* \
             "$base"/cmake/*/bin; do
        [ -d "$d" ] && PATH_ADD+=("$d")
    done
done
if [ ${#PATH_ADD[@]} -gt 0 ]; then
    JOINED="$(IFS=:; echo "${PATH_ADD[*]}")"
    export PATH="$JOINED:$PATH"
fi
# esp-rom-elfs 离线安装记录缺失会导致 idf_tools.py export 失败(v1.2.1-42):显式给目录
if [ -z "${ESP_ROM_ELF_DIR:-}" ]; then
    ESP_ROM_ELF_DIR="$(ls -d "$TOOLS"/tools/esp-rom-elfs/*/esp-rom-elfs 2>/dev/null | tail -1 || true)"
    [ -n "$ESP_ROM_ELF_DIR" ] && export ESP_ROM_ELF_DIR
fi

command -v idf.py >/dev/null 2>&1 || { echo "!! PATH 里没有 idf.py(检查 IDF_TOOLS_PATH=$TOOLS)" >&2; exit 1; }

# ---- 3) 生成器/env 守卫 ----
# 真根因(2026-10-11 抓包确认):VS Code CMake 扩展会把 CMAKE_GENERATOR 注入终端环境,
# 于是**即使 build/ 缓存不存在**,idf.py 也会用 `-G 'Unix Makefiles'` 配置 → 编译器探测失败。
# 这里强制回 Ninja(ESP-IDF 的原生生成器),从源头消除该污染。
export CMAKE_GENERATOR=Ninja
unset CMAKE_GENERATOR_INSTANCE 2>/dev/null || true
mismatch=""
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    gen="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -1 || true)"
    if [ -n "$gen" ] && [ "$gen" != "Ninja" ]; then
        mismatch="$gen"
    fi
fi
if [ -n "$mismatch" ]; then
    echo "!! $BUILD_DIR 的 CMake 生成器是 '$mismatch'(应为 Ninja)" >&2
    echo "   多半是 VS Code CMake 扩展写入的 → 自动清理 CMakeCache.txt / CMakeFiles 后继续" >&2
    rm -f "$BUILD_DIR/CMakeCache.txt"
    rm -rf "$BUILD_DIR/CMakeFiles"
fi
# 占住构建目录的调试进程(会让 idf.py 静默卡住,见 问题与版本优化.md §3.1)
DBG_PIDS="$(pgrep -f 'cmake.*--debugge[r]' 2>/dev/null | tr '\n' ' ' || true)"
if [ -n "$DBG_PIDS" ]; then
    echo "!! 检测到 cmake --debugger 进程(PID: $DBG_PIDS)—— 它会让 idf.py 构建静默卡住" >&2
    echo "   处置: kill $DBG_PIDS    (或先关掉 VS Code 的 CMake 调试会话)" >&2
fi

# ---- 4) 执行 ----
cd "$PROJ_DIR"
action="${1:-build}"
[ $# -gt 0 ] && shift
case "$action" in
    build|flash|app-flash|monitor|erase-flash|size|menuconfig|fullclean)
        idf.py -B "$BUILD_DIR" "$action" "$@"
        _rc=$?
        exit $_rc ;;   # 以退出而非 exec 收尾:EXIT trap(根 sdkconfig 恢复)必须执行
    cache-guard)
        rm -f "$BUILD_DIR/CMakeCache.txt"
        rm -rf "$BUILD_DIR/CMakeFiles"
        echo "已清理 $BUILD_DIR 的 CMake 缓存(生成器守卫)" ;;
    paths)
        echo "ninja        = $(command -v ninja || echo MISSING)"
        echo "cmake        = $(command -v cmake || echo MISSING)"
        echo "toolchain    = $(command -v xtensa-esp32-elf-gcc || echo MISSING)"
        echo "idf.py       = $(command -v idf.py || echo MISSING)"
        echo "python       = $(command -v python || echo MISSING)"
        ;;
    version)
        echo "IDF_PATH        = $IDF_PATH"
        echo "ESP_IDF_VERSION = $ESP_IDF_VERSION"
        echo "IDF_TOOLS_PATH  = $IDF_TOOLS_PATH"
        echo "BUILD_DIR       = $BUILD_DIR"
        idf.py --version ;;
    -h|--help|help|"")
        sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' ;;
    *)
        echo "未知动作: $action(可用: build|flash|app-flash|monitor|erase-flash|size|menuconfig|fullclean|cache-guard|version)" >&2
        exit 2 ;;
esac
