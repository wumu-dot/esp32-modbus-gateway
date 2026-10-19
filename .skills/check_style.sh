#!/bin/bash
# C 代码静态规范检查（R11 / 移植自 embedspec/.skills/check_style.sh）
# 用法: bash .skills/check_style.sh
# 偏差：源码根 firmware/Core+App → main/；排除 managed_components（R19 禁改区）
set -uo pipefail

cd "$(dirname "$0")/.."
echo "📝 C 代码静态检查（main/）"

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "⚠️ cppcheck 未安装，跳过（安装: winget install Cppcheck，或 scoop install cppcheck）"
    exit 0
fi

cppcheck --enable=warning,performance,unusedFunction \
    --suppress=missingIncludeSystem \
    --inline-suppr \
    -I main \
    main/ 2>&1 | tail -30

echo "✅ 静态检查完成"
