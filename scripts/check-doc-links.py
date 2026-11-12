#!/usr/bin/env python3
"""检查仓库里 Markdown 文档的相对链接（只用标准库）。

    python3 scripts/check-doc-links.py              # 扫全仓，有问题退出码 1
    python3 scripts/check-doc-links.py docs/ README.md   # 只扫给定的文件 / 目录
    python3 scripts/check-doc-links.py --json       # 机器可读输出
    python3 scripts/check-doc-links.py --no-untracked   # 不报"目标存在但未入库"

查什么：
  * 相对链接（`[x](路径)`、`![x](路径)`、`[x]: 路径` 引用定义、HTML 的 href / src）
    的目标文件或目录存在，且**大小写完全一致**（macOS 盘不分大小写，GitHub 分）；
  * 目标存在于工作区但没入库（`git ls-files` 里没有）—— GitHub 上点开是 404；
  * `#锚点` 按 GitHub 的生成规则能在目标 .md 里找到：标题渲染成纯文本 → 小写 →
    去掉不是字母 / 数字 / 组合符 / 连接标点（`_`）/ 空格 / `-` 的字符（中文保留，
    全角标点、★、emoji 去掉）→ 空格换 `-`（不合并）→ 重复标题依次加 `-1`、`-2`；
    另认 `<a name=…>` / `<a id=…>` 与 `id="…"` 显式锚点。
不查什么：外链（http / https / mailto / 任何带 scheme 的）、代码块与行内代码里的东西、
  非 .md 目标的锚点（如源码的 `#L12`）。

默认扫 `git ls-files -co --exclude-standard` 里的全部 *.md（不在 git 里时退回目录遍历），
排除 refs/、out/、.claude/、node_modules/、installer/live/build/ 等第三方或生成物。
输出每条一行：`文件:行 → 目标  原因`。
"""
import argparse
import html
import json
import os
import re
import subprocess
import sys
import unicodedata
from urllib.parse import unquote

EXCLUDE_PREFIXES = (
    "refs/", "out/", ".claude/", ".git/", "installer/live/build/",
    "installer/live/.dart_tool/",
)
EXCLUDE_PARTS = ("node_modules", ".dart_tool", "__pycache__")

SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
ATX_RE = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?[ \t]*$")
SETEXT_RE = re.compile(r"^ {0,3}(=+|-+)[ \t]*$")
# 行内链接 / 图片：只要 `](` 之后的目标部分，目标里允许一层成对括号
INLINE_LINK_RE = re.compile(
    r"\]\(\s*(<[^>\n]*>|(?:[^()\s]|\([^()\s]*\))*)(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?\s*\)")
REFDEF_RE = re.compile(r"^ {0,3}\[[^\]]+\]:\s*(<[^>]*>|\S+)")
HTML_ATTR_RE = re.compile(r"<(?:a|img|source|link)\b[^>]*?\b(?:href|src)\s*=\s*([\"'])(.*?)\1", re.I)
HTML_ANCHOR_RE = re.compile(r"<[a-zA-Z][^>]*?\b(?:id|name)\s*=\s*([\"'])(.*?)\1", re.I)


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        out = subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout.strip()
        return out, True
    except (OSError, subprocess.CalledProcessError):
        return os.path.dirname(here), False


def excluded(rel):
    if rel.startswith(EXCLUDE_PREFIXES):
        return True
    return any(p in rel.split("/") for p in EXCLUDE_PARTS)


def git_files(root, *args):
    out = subprocess.run(["git", "-C", root, "ls-files", "-z", *args],
                         capture_output=True, check=True).stdout
    return [p.decode("utf-8") for p in out.split(b"\0") if p]


def list_md(root, use_git, paths):
    if use_git:
        files = [f for f in git_files(root, "-co", "--exclude-standard") if f.endswith(".md")]
    else:
        files = []
        for d, dirs, names in os.walk(root):
            rel_d = os.path.relpath(d, root).replace(os.sep, "/")
            rel_d = "" if rel_d == "." else rel_d + "/"
            dirs[:] = [x for x in dirs if not excluded(rel_d + x + "/")]
            files += [rel_d + n for n in names if n.endswith(".md")]
    files = sorted(f for f in set(files) if not excluded(f) and os.path.isfile(os.path.join(root, f)))
    if paths:
        sel = []
        for p in paths:
            rel = os.path.relpath(os.path.abspath(p), root).replace(os.sep, "/")
            if rel == ".":
                return files
            sel += [f for f in files if f == rel or f.startswith(rel.rstrip("/") + "/")]
        files = sorted(set(sel))
    return files


# ---------- 标题 → GitHub 锚点 ----------

def strip_inline_code(text, keep_content):
    """把行内代码换掉。keep_content=True 时留内容（算锚点用），否则换成等长空格（找链接用）。"""
    out, i = [], 0
    while i < len(text):
        if text[i] == "`":
            j = i
            while j < len(text) and text[j] == "`":
                j += 1
            ticks = text[i:j]
            k = text.find(ticks, j)
            while k != -1 and k + len(ticks) < len(text) and text[k + len(ticks)] == "`":
                k = text.find(ticks, k + len(ticks) + 1)
            if k == -1:
                out.append(ticks)
                i = j
                continue
            inner = text[j:k]
            if keep_content:
                s = inner
                if len(s) >= 2 and s[0] == " " and s[-1] == " " and s.strip():
                    s = s[1:-1]
                out.append("\0" + s + "\1")
            else:
                out.append(" " * (k + len(ticks) - i))
            i = k + len(ticks)
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def heading_plain_text(raw):
    """近似 GitHub 渲染后标题的 textContent。"""
    t = strip_inline_code(raw, keep_content=True)
    # 代码内容先保护起来
    codes = []

    def keep(m):
        codes.append(m.group(1))
        return "\2%d\3" % (len(codes) - 1)
    t = re.sub("\x00(.*?)\x01", keep, t, flags=re.S)
    t = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", t)          # 图片：无文本
    t = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", t)      # 链接：取文字
    t = re.sub(r"\[([^\]]*)\]\[[^\]]*\]", r"\1", t)     # 引用式链接
    t = re.sub(r"<[^>]+>", "", t)                       # HTML 标签
    t = t.replace("\\", "")
    t = re.sub(r"(\*+|~~)", "", t)
    t = re.sub(r"(?<![0-9A-Za-z])_+|_+(?![0-9A-Za-z])", "", t)  # 强调用的 _，词内的 _ 保留
    t = html.unescape(t)
    t = re.sub("\x02(\\d+)\x03", lambda m: codes[int(m.group(1))], t)
    return t.strip()


def slugify(text):
    out = []
    for ch in text.lower():
        if ch == " " or ch == "-":
            out.append("-" if ch == " " else ch)
            continue
        cat = unicodedata.category(ch)
        if cat[0] in "LMN" or cat == "Pc":
            out.append(ch)
    return "".join(out)


def collect_anchors(lines):
    anchors, seen = set(), {}

    def add(base):
        slug, n = base, 0
        while slug in seen:
            n += 1
            slug = "%s-%d" % (base, n)
        seen[slug] = True
        seen.setdefault(base, True)
        anchors.add(slug)

    in_fence, fence = False, ""
    prev = ""
    for line in lines:
        m = FENCE_RE.match(line)
        if m:
            if not in_fence:
                in_fence, fence = True, m.group(1)
            elif m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and not line.strip()[len(m.group(1)):].strip():
                in_fence = False
            prev = ""
            continue
        if in_fence:
            continue
        for am in HTML_ANCHOR_RE.finditer(line):
            anchors.add(am.group(2))
        m = ATX_RE.match(line)
        if m and not line.lstrip().startswith("#!"):
            txt = re.sub(r"[ \t]+#+[ \t]*$", "", m.group(2) or "")
            add(slugify(heading_plain_text(txt)))
            prev = ""
            continue
        sm = SETEXT_RE.match(line)
        if sm and prev.strip() and not re.match(r"^\s*([|>#*+-]|\d+[.)]\s)", prev):
            add(slugify(heading_plain_text(prev.strip())))
            prev = ""
            continue
        prev = line
    return anchors


# ---------- 链接提取 ----------

def iter_links(lines):
    in_fence, fence = False, ""
    for no, line in enumerate(lines, 1):
        m = FENCE_RE.match(line)
        if m:
            if not in_fence:
                in_fence, fence = True, m.group(1)
            elif m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence):
                in_fence = False
            continue
        if in_fence:
            continue
        clean = strip_inline_code(line, keep_content=False)
        for lm in INLINE_LINK_RE.finditer(clean):
            yield no, lm.group(1).strip("<>")
        rm = REFDEF_RE.match(clean)
        if rm:
            yield no, rm.group(1).strip("<>")
        for hm in HTML_ATTR_RE.finditer(clean):
            yield no, hm.group(2)


# ---------- 目标检查 ----------

class Checker:
    def __init__(self, root, use_git, check_untracked):
        self.root = root
        self.tracked = set()
        self.tracked_dirs = set()
        self.check_untracked = check_untracked and use_git
        if use_git:
            for f in git_files(root):
                self.tracked.add(f)
                parts = f.split("/")
                for i in range(1, len(parts)):
                    self.tracked_dirs.add("/".join(parts[:i]))
        self.anchor_cache = {}
        self.listdir_cache = {}

    def exact_case_exists(self, rel):
        cur = self.root
        for part in [p for p in rel.split("/") if p]:
            if cur not in self.listdir_cache:
                try:
                    self.listdir_cache[cur] = set(os.listdir(cur))
                except OSError:
                    self.listdir_cache[cur] = set()
            if part not in self.listdir_cache[cur]:
                return False
            cur = os.path.join(cur, part)
        return True

    def anchors_of(self, rel):
        if rel not in self.anchor_cache:
            with open(os.path.join(self.root, rel), encoding="utf-8", errors="replace") as fh:
                self.anchor_cache[rel] = collect_anchors(fh.read().splitlines())
        return self.anchor_cache[rel]

    def check(self, src, target):
        """返回 None（没问题）或原因字符串。"""
        if not target or SCHEME_RE.match(target) or target.startswith("//"):
            return None
        path, _, frag = target.partition("#")
        path = unquote(path.split("?", 1)[0])
        frag = unquote(frag)
        if path:
            base = "" if path.startswith("/") else os.path.dirname(src)
            rel = os.path.normpath(os.path.join(base, path.lstrip("/"))).replace(os.sep, "/")
            if rel == ".":
                rel = ""
            if rel.startswith("../") or rel == "..":
                return "目标在仓库之外"
            full = os.path.join(self.root, rel)
            if not os.path.exists(full):
                return "目标不存在"
            if not self.exact_case_exists(rel):
                return "大小写不一致（macOS 上能开，GitHub 上 404）"
            if self.check_untracked and rel and rel not in self.tracked and rel not in self.tracked_dirs:
                return "目标存在但未入库（GitHub 上 404）"
        else:
            rel = src
        if frag and rel.endswith(".md") and os.path.isfile(os.path.join(self.root, rel)):
            if frag not in self.anchors_of(rel) and frag.lower() not in self.anchors_of(rel):
                return "锚点 #%s 在 %s 里找不到" % (frag, rel)
        return None


def main():
    ap = argparse.ArgumentParser(description="检查 Markdown 相对链接与 #锚点（GitHub 规则）")
    ap.add_argument("paths", nargs="*", help="只扫这些文件 / 目录（默认全仓）")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--no-untracked", action="store_true", help="不报告“目标存在但未入库”")
    ap.add_argument("--exclude", action="append", default=[], metavar="前缀",
                    help="另外排除的路径前缀（相对仓库根，可重复）")
    args = ap.parse_args()

    root, use_git = repo_root()
    files = [f for f in list_md(root, use_git, args.paths)
             if not any(f.startswith(x) for x in args.exclude)]
    chk = Checker(root, use_git, not args.no_untracked)
    problems, nlinks = [], 0
    for f in files:
        with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
        for no, target in iter_links(lines):
            nlinks += 1
            why = chk.check(f, target)
            if why:
                problems.append({"file": f, "line": no, "target": target, "reason": why})

    if args.json:
        json.dump({"files": len(files), "links": nlinks, "problems": problems},
                  sys.stdout, ensure_ascii=False, indent=2)
        print()
    else:
        for p in problems:
            print("%s:%d → %s  %s" % (p["file"], p["line"], p["target"], p["reason"]))
        sys.stdout.flush()
        print("扫了 %d 个文件、%d 条相对 / 页内链接，问题 %d 条" % (len(files), nlinks, len(problems)),
              file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
