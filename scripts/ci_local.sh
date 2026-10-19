#!/bin/bash
# 本地 CI 门禁（R11：代码修改完成后必须跑，非 0 退出码不得标记完成）
# 用法:
#   bash scripts/ci_local.sh            # 快档：三源门禁 + Python 单测（约 3 s）
#   bash scripts/ci_local.sh --build    # 全档：再加 idf.py build（约 1~2 min，需 IDF 环境）
#
# 移植自 embedspec/scripts/ci_local.sh（2026-10-19）
# 偏差：embedspec 原版含 STM32 专属的 HSE/栈自动化校验，本项目无对应项 ⇒ 改为
#       ① R27 三源门禁 ② tools/ 单测 ③（可选）ESP-IDF 构建。
# ⚠️ 本机 idf.py 必须在 PowerShell 下跑（Git Bash 会假通过，见 CLAUDE.md §构建环境）。
#    因此在 Git Bash 里跑 --build 会**明确拒跑**，而不是静默失败。
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
FAIL=0
DO_BUILD=0
[ "${1:-}" = "--build" ] && DO_BUILD=1

echo "===== CI 本地门禁 ====="
echo "  项目根: $ROOT"

# ---- 1. R27 三源门禁 ----
echo ""
echo "[1/3] R27 预研三源门禁"
if bash scripts/check_three_source.sh main; then
    :
else
    echo "  ❌ 三源门禁失败"
    FAIL=1
fi

# ---- 2. tools/ 单测 ----
echo ""
echo "[2/3] tools/ 单元测试"
PY="tools/.venv/Scripts/python.exe"
if [ ! -x "$PY" ]; then
    echo "  ⚠️ 未找到 $PY（tools/.venv 未创建）⇒ 跳过单测"
else
    if "$PY" -m pytest tools/ -q; then
        :
    else
        echo "  ❌ 单测失败"
        FAIL=1
    fi
fi

# ---- 3. 构建（可选）----
echo ""
echo "[3/3] ESP-IDF 构建"
if [ "$DO_BUILD" -eq 0 ]; then
    echo "  ⊘ 未指定 --build，跳过（快档）"
elif [ -n "${MSYSTEM:-}" ]; then
    echo "  ⛔ 检测到 MSYSTEM=$MSYSTEM ⇒ Git Bash 下 idf.py 会【假通过退 0】，拒绝执行。"
    echo "     请在 PowerShell 中按 CLAUDE.md「构建环境」手工构造 env 后跑 idf.py build，"
    echo "     或直接用 .skills/flash.ps1。"
    FAIL=1
else
    if command -v idf.py >/dev/null 2>&1; then
        if idf.py build; then
            :
        else
            echo "  ❌ 构建失败"
            FAIL=1
        fi
    else
        echo "  ⛔ 当前 shell 无 idf.py ⇒ 请在 PowerShell 下按 CLAUDE.md 配方执行"
        FAIL=1
    fi
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "===== ✅ CI 通过（R21 证据：见上方各步输出）====="
    exit 0
else
    echo "===== 📛 CI 失败 —— 不得标记开发完成（R11）====="
    exit 1
fi
