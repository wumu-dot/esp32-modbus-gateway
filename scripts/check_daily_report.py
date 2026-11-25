#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_daily_report.py -- 日报「configure 检查」。

定位（与 CMake 的类比）：
    模板   = CMakeLists        —— 声明"必须产出什么"
    日报   = 构建产物
    本脚本 = configure 阶段    —— 产物不合规格就报错，而不是"看起来差不多"

设计原则（核心）：
    **必需章节从模板本身解析**，不在脚本里另存一份清单。
    否则模板改了、脚本没改，就变成"两处声明"，必然漂移。

检查四项：
    C1 章节齐全      —— 模板里每个 `## ` 标题，报告里都要有
    C2 指针存在      —— 反引号里的仓库路径要真实存在；commit hash 要能在 git 里查到
    C3 命令带工作目录 —— ```bash 代码块里若出现构建/烧录命令，须有 `cd `
    C4 无含糊措辞    —— 禁止「应该可以 / 大概 / 可能没问题 / 差不多」这类不可判定表述

用法：
    python scripts/check_daily_report.py --report <日报.md> [--template <模板.md>] [--repo <仓库根>]

退出码：0 全过；1 有 FAIL；2 参数/文件问题。
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

DEFAULT_TEMPLATE = r"C:\Users\wumu2\OneDrive\桌面\报告\模板-日报.md"

# C4：不可判定表述（命中即 FAIL）
HEDGE_WORDS = [
    "应该可以", "应该没问题", "大概", "可能没问题", "差不多",
    "估计能", "理论上没问题", "这里应该",
]

# C2：只检查"像路径"的反引号内容
PATH_RE = re.compile(r"`([^`\n]+)`")
# 形如 xxx/yyy.ext 或 xxx\yyy.ext（带扩展名或含斜杠）
LOOKS_LIKE_PATH = re.compile(r"[A-Za-z0-9_.\-\\/]+\.(?:c|h|md|py|ps1|sh|txt|json|cfg|mk|elf|bin|log)$"
                            r"|(?:^|[\\/])[A-Za-z0-9_.\-]+[\\/][A-Za-z0-9_.\-]+$")
COMMIT_RE = re.compile(r"\b([0-9a-f]{7,40})\b")
# 章节判定：抽取 `## ` 标题里的中文编号后关键词
SECTION_NUM_RE = re.compile(r"^##\s*([一二三四五六七八九十]+)、\s*(.+)$")


def read_text(p: str) -> str:
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def template_sections(tpl: str):
    """从模板取「必须有的章节」：返回 [(序号, 标题)]，忽略附录/自检等非编号节。"""
    out = []
    in_comment = False
    for line in tpl.splitlines():
        if line.strip().startswith("<!--"):
            in_comment = True
        if in_comment:
            if "-->" in line:
                in_comment = False
            continue
        m = SECTION_NUM_RE.match(line.strip())
        if m:
            out.append((m.group(1), m.group(2).strip()))
    return out


def report_sections(rep: str):
    got = {}
    in_comment = False
    for line in rep.splitlines():
        if line.strip().startswith("<!--"):
            in_comment = True
        if in_comment:
            if "-->" in line:
                in_comment = False
            continue
        m = SECTION_NUM_RE.match(line.strip())
        if m:
            got[m.group(1)] = m.group(2).strip()
    return got


def git_has_commit(repo: str, sha: str) -> bool:
    try:
        r = subprocess.run(["git", "-C", repo, "cat-file", "-e", f"{sha}^{{commit}}"],
                           capture_output=True)
        return r.returncode == 0
    except Exception:
        return False


def resolve_path(repo: str, raw: str):
    """把反引号内容当路径解析；返回 True(存在)/False(不存在)/None(不是路径)。

    搜索顺序（相当于编译器的 include 路径）：
      1) 原样（绝对路径或相对 CWD）
      2) 仓库根
      3) 仓库根的常见子层：firmware/、firmware/soundDog/、main/
      4) **跨仓库**：`<兄弟仓库名>/...` → 在仓库的父目录下找
         （本项目实际存在两仓互引：日报常写 `esp32-modbus-gateway/scripts/...`）
    """
    s = raw.strip().strip("<>").strip()
    if not s or len(s) > 200:
        return None
    if not LOOKS_LIKE_PATH.search(s):
        return None
    # 去掉行号/章节后缀，如 `main.c:214` 或 `A7-04 §9`
    s = re.split(r"[:：]\s*\d|§", s)[0].strip()
    if not s:
        return None

    cands = [s, os.path.join(repo, s)]
    for sub in ("firmware", os.path.join("firmware", "soundDog"), "main"):
        cands.append(os.path.join(repo, sub, s))
    # 跨仓库：取第一段当兄弟目录名
    parent = os.path.dirname(os.path.abspath(repo))
    cands.append(os.path.join(parent, s))

    for c in cands:
        if os.path.exists(c):
            return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="日报 configure 检查（对照模板校验）")
    ap.add_argument("--report", required=True, help="待检查的日报路径")
    ap.add_argument("--template", default=DEFAULT_TEMPLATE, help="模板路径（必需章节的唯一来源）")
    ap.add_argument("--repo", default=r"C:\Projects\SoundDog", help="指针解析用的仓库根")
    ap.add_argument("--quiet-warn", action="store_true", help="不打印 WARN 明细")
    args = ap.parse_args()

    if not os.path.exists(args.report):
        print(f"[FATAL] 报告不存在: {args.report}")
        return 2
    if not os.path.exists(args.template):
        print(f"[FATAL] 模板不存在: {args.template}")
        return 2

    rep = read_text(args.report)
    tpl = read_text(args.template)
    fails, warns = [], []

    # ---------- C1 章节齐全 ----------
    need = template_sections(tpl)
    got = report_sections(rep)
    missing = [(n, t) for n, t in need if n not in got]
    if missing:
        for n, t in missing:
            fails.append(f"C1 缺章节：『{n}、{t}』")
    c1 = "PASS" if not missing else "FAIL"

    # ---------- C2 指针存在 ----------
    bad_paths, bad_commits, nptr = [], [], 0
    for m in PATH_RE.finditer(rep):
        raw = m.group(1)
        r = resolve_path(args.repo, raw)
        if r is None:
            continue
        nptr += 1
        if r is False:
            bad_paths.append(raw)
    for m in COMMIT_RE.finditer(rep):
        sha = m.group(1)
        if len(sha) < 7:
            continue
        # 排除纯数字（可能是数值）与明显非 hash
        if sha.isdigit():
            continue
        if not git_has_commit(args.repo, sha):
            # 网关侧 commit 不在 SoundDog 仓库里，试网关仓库
            if not git_has_commit(r"C:\Projects\esp32-modbus-gateway", sha):
                bad_commits.append(sha)
    if bad_paths:
        for p in sorted(set(bad_paths)):
            fails.append(f"C2 路径不存在：`{p}`")
    if bad_commits:
        for c in sorted(set(bad_commits)):
            fails.append(f"C2 commit 查不到：`{c}`（两个仓库都没有）")
    c2 = "PASS" if not (bad_paths or bad_commits) else "FAIL"

    # ---------- C3 命令带工作目录 ----------
    # 判定方式：逐条命令跟踪「在它之前是否出现过 cd」。
    # ⚠️ 已知盲区（不假装能抓）：本检查只能发现"整块完全没有 cd"；
    #    若块内另有 cd 但"烧录前的 cd 被删/写错"，它仍会通过——要抓这种需要有
    #    「命令 → 期望目录」的映射表（相当于 per-project manifest），当前未做。
    #    因此 C3 定位为"提醒级"，真正的目录正确性靠 §4.2 的人工核对。
    BUILD_CMD = re.compile(
        r"^\s*(?:sudo\s+)?(make|cmake|ninja|idf\.py|python[0-9.]*|py\b|openocd|"
        r"esptool(?:\.py)?|arm-none-eabi-\w+|gcc|g\+\+|pytest|pnpm|npm|node)\b")
    nblock = 0
    for m in re.finditer(r"```(?:bash|sh|powershell)?\n(.*?)```", rep, re.S):
        body = m.group(1)
        nblock += 1
        seen_cd = False
        for ln in body.splitlines():
            if re.match(r"^\s*cd\s+", ln):
                seen_cd = True
                continue
            if BUILD_CMD.match(ln) and not seen_cd:
                warns.append(f"C3 命令前未标工作目录：{ln.strip()[:70]}")
    c3 = "PASS" if not warns else "WARN"
    if warns:
        # C3 是「能让人跑错目录」，严重度等同 FAIL —— 明确提示但不改退出码语义
        for w in warns:
            fails.append(w.replace("C3 ", "C3 "))

    # ---------- C4 含糊措辞 ----------
    hedge_hits = [(w, rep.count(w)) for w in HEDGE_WORDS if w in rep]
    for w, n in hedge_hits:
        fails.append(f"C4 不可判定表述：『{w}』×{n}")
    c4 = "PASS" if not hedge_hits else "FAIL"

    # ---------- 输出 ----------
    print("=" * 62)
    print(f"报告  : {os.path.basename(args.report)}")
    print(f"模板  : {os.path.basename(args.template)}（必需章节 {len(need)} 个）")
    print(f"体量  : {len(rep.splitlines())} 行 / {len(rep)} 字符")
    print("-" * 62)
    print(f"  C1 章节齐全        {c1}   （缺 {len(missing)} 个）")
    print(f"  C2 指针存在        {c2}   （检查 {nptr} 个路径 / {len(bad_commits)} 个 commit 异常）")
    print(f"  C3 命令带工作目录  {c3}   （{nblock} 个代码块，{len(warns)} 个无 cd）")
    print(f"  C4 无含糊措辞      {c4}   （命中 {len(hedge_hits)} 类）")
    print("-" * 62)
    for w in warns:
        if not args.quiet_warn:
            print("  ⚠️  " + w)
    for f in fails:
        print("  ❌  " + f)
    print("=" * 62)
    print("结论：" + ("✅ 通过（符合模板规格）" if not fails else f"❌ 不通过（{len(fails)} 项 FAIL）"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
