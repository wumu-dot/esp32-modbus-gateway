#!/bin/sh
# ================================================================
# pre-commit-daily-report.sh —— 日报「configure 检查」钩子
#
# 定位（CMake 类比）：
#   模板（模板-日报.md）= CMakeLists      —— 声明必须产出什么
#   日报               = 构建产物
#   本钩子             = configure 阶段   —— 产物不合规格就拦住提交
#
# 行为：
#   1) 只校验 staged 里**看起来是日报**的文件（名含"当日工作小结"或"日报"，排除"模板"）
#   2) **只校验 2026-11-25 及之后的**（新规范生效日）；历史报告视为已归档，不再改造
#   3) 不含日报的提交直接放行，不给日常提交加延迟
#
# 三个踩过的坑（改本文件前先读）：
#   ① git 默认 core.quotepath=true 会把中文名转义成 \345\256\241
#      ⇒ case 模式匹配不上 ⇒ 钩子**静默放行**（比没钩子更危险）
#      所以下面必须 `git -c core.quotepath=false`。
#   ② 用 `| while` 会把循环放进子 shell，RC 传不出去 ⇒ 改成 for 循环。
#   ③ 临时文件 + rm 在受限环境下可能失败 ⇒ 干脆不用临时文件。
#
# 安装（报告仓库根目录）：
#   cp <此文件> .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
# ================================================================

PY="C:/Users/wumu2/pythonapp/python.exe"
CHECK="C:/Projects/esp32-modbus-gateway/scripts/check_daily_report.py"
TPL="C:/Users/wumu2/OneDrive/桌面/报告/模板-日报.md"
SINCE="2026-11-25"          # 新日报规范生效日；此日期之前的日报不校验

if [ ! -f "$CHECK" ] || [ ! -f "$PY" ]; then
    echo "[daily-report] 跳过：检查器或 Python 不存在（$CHECK）" >&2
    exit 0
fi
if [ ! -f "$TPL" ]; then
    echo "[daily-report] 跳过：模板不存在（$TPL）" >&2
    exit 0
fi

FILES=$(git -c core.quotepath=false diff --cached --name-only --diff-filter=ACM 2>/dev/null)

RC=0
NCHECK=0
for f in $FILES; do
    case "$f" in
        *模板*)        continue ;;
        *当日工作小结*|*日报*) ;;
        *)             continue ;;
    esac
    [ -f "$f" ] || continue

    # 取文件名里的 YYYY-MM-DD，早于生效日则跳过
    d=$(echo "$f" | grep -oE '20[0-9]{2}-[0-9]{2}-[0-9]{2}' | tail -1)
    if [ -n "$d" ] && [ "$d" \< "$SINCE" ]; then
        echo "[daily-report] 跳过历史日报（早于 $SINCE）：$f" >&2
        continue
    fi

    NCHECK=$((NCHECK + 1))
    echo "" >&2
    echo "──── 校验日报：$f" >&2
    if ! "$PY" "$CHECK" --report "$f" --template "$TPL" >&2; then
        RC=1
    fi
done

if [ "$RC" -ne 0 ]; then
    echo "" >&2
    echo "❌ 日报未通过规格检查（$NCHECK 份），提交已拦截。" >&2
    echo "   修法：按上面 ❌ 条目逐项修。" >&2
    echo "   ⛔ 不建议 --no-verify：那等于在日报库里留下假账，事后无法分辨。" >&2
fi
exit "$RC"
