#!/usr/bin/env python3
"""7.4迁移暂存区（patches/migration-7.4/）的工具。只用标准库。

  migrate.py prime  <内核树>             在现行基线上打一遍现行补丁链，生成三方合并所需的原始blob
  migrate.py tree   <内核树> [--survey]  在内核树里从新基线重建整条补丁链
  migrate.py tree   <内核树> --resume    解决完冲突（git am --continue或--skip）后继续
  migrate.py record <内核树>             把内核树里与来源不同的补丁写回override/、base/、add/
  migrate.py check                       检查暂存区与当前dev是否脱节
  migrate.py series                      列出新基线上的补丁链（不碰内核树）

补丁链 = base/（buildbot层）+ dev的KPATCHES（去掉drop.tsv，用override/替换）+ add.tsv插入的补丁。
补丁用"键"标识，不用编号：dev补丁为<子系统>/<slug>，base层为base/<slug>，新增补丁为add/<子系统>/<slug>，
slug即文件名去掉NNNN-前缀与.patch。dev重新编号不影响这里的任何记录。

内核树须是一个git工作树，且已含BASE_COMMIT（prime还需OLD_BASE_COMMIT）。工具会把它切到分离HEAD并重置，
树里未提交的改动会丢失。进度存在该树git目录下的migration-7.4.json。
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
KAP = REPO / "scripts" / "kernel-apply-patches.sh"
STATE_NAME = "migration-7.4.json"
PRIME_REF = "refs/migration-7.4/prime"


def die(msg, code=1):
    print(f"✗ {msg}", file=sys.stderr)
    sys.exit(code)


# ---------- 暂存区数据 ----------

def read_conf():
    conf = {}
    for line in (HERE / "migration.conf").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            conf[k.strip()] = v.strip()
    for k in ("BASE_TAG", "BASE_COMMIT", "OLD_BASE_TAG", "OLD_BASE_COMMIT"):
        if k not in conf:
            die(f"migration.conf缺少{k}")
    return conf


def read_tsv(name):
    rows = []
    p = HERE / name
    if not p.exists():
        return rows
    for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        rows.append((n, line.split("\t")))
    return rows


def slug_of(path):
    return re.sub(r"^\d{4}-", "", Path(path).stem)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def subject_of(path):
    """mbox头里的Subject，去掉[PATCH]前缀，折行接回。"""
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines):
        if line.startswith("Subject:"):
            s = line[len("Subject:"):].strip()
            j = i + 1
            while j < len(lines) and lines[j].startswith((" ", "\t")):
                s += " " + lines[j].strip()
                j += 1
            return re.sub(r"^\[[^]]*\]\s*", "", s)
        if not line.strip():
            break
    return None


def dev_entries():
    """dev的KPATCHES，按应用顺序返回[(键, 补丁路径)]。"""
    text = KAP.read_text(encoding="utf-8")
    m = re.search(r"^KPATCHES=\($(.*?)^\)", text, re.S | re.M)
    if not m:
        die(f"无法从{KAP}解析KPATCHES")
    out, seen = [], set()
    for rel in re.findall(r"^\s+(kernel/[a-z0-9-]+/\d{4}-\S+\.patch)", m.group(1), re.M):
        sub = rel.split("/")[1]
        key = f"{sub}/{slug_of(rel)}"
        if key in seen:
            die(f"KPATCHES里有重复的键：{key}")
        seen.add(key)
        out.append((key, REPO / "patches" / rel))
    return out


def build_series(strict=True):
    """返回(补丁链, 附加信息)。补丁链每项为dict(key, path, source, subject)。"""
    drop = {c[0]: c[1] if len(c) > 1 else "" for _, c in read_tsv("drop.tsv")}
    override = {}
    for n, c in read_tsv("override.tsv"):
        if len(c) < 2:
            die(f"override.tsv第{n}行缺少sha256")
        override[c[0]] = c[1]
    adds = defaultdict(list)
    for n, c in read_tsv("add.tsv"):
        if len(c) < 2:
            die(f"add.tsv第{n}行缺少文件")
        f = HERE / c[1]
        if not f.is_file():
            die(f"add.tsv第{n}行的文件不存在：{c[1]}")
        sub = Path(c[1]).parent.name
        adds[c[0]].append({"key": f"add/{sub}/{slug_of(f)}", "path": f, "source": "add"})

    info = {"dropped": [], "stale": [], "dangling": [], "unknown_after": []}
    items = []

    def push(it):
        it["subject"] = subject_of(it["path"])
        if not it["subject"]:
            die(f"无法读取Subject：{it['path']}")
        items.append(it)

    def push_adds(after):
        for a in adds.pop(after, []):
            push(dict(a))

    for f in sorted((HERE / "base").glob("*.patch")):
        key = f"base/{slug_of(f)}"
        if key in drop:
            info["dropped"].append(key)
        else:
            push({"key": key, "path": f, "source": "base"})
        push_adds(key)
    push_adds("^")

    dev_keys = set()
    for key, path in dev_entries():
        dev_keys.add(key)
        if not path.is_file():
            die(f"KPATCHES登记的补丁不存在：{path}")
        if key in drop:
            info["dropped"].append(key)
        elif key in override:
            sub, slug = key.split("/", 1)
            op = HERE / "override" / sub / f"{slug}.patch"
            if not op.is_file():
                die(f"override.tsv登记了{key}，但{op.relative_to(HERE)}不存在")
            if sha256(path) != override[key]:
                info["stale"].append(key)
            push({"key": key, "path": op, "source": "override"})
        else:
            push({"key": key, "path": path, "source": "dev"})
        push_adds(key)

    base_keys = {f"base/{slug_of(f)}" for f in (HERE / "base").glob("*.patch")}
    for key in list(drop) + list(override):
        if key not in dev_keys and key not in base_keys:
            info["dangling"].append(key)
    info["unknown_after"] = sorted(adds)

    subjects = defaultdict(list)
    for it in items:
        subjects[it["subject"]].append(it["key"])
    dup = {s: k for s, k in subjects.items() if len(k) > 1}
    if dup:
        die("补丁链里有重复的Subject，record无法区分：" + "; ".join(f"{s} ← {k}" for s, k in dup.items()))
    if strict and info["unknown_after"]:
        die("add.tsv引用了补丁链里不存在的键：" + ", ".join(info["unknown_after"]))
    if strict and info["stale"]:
        die("以下override所基于的dev补丁已改动，需要重新解决（或用--allow-stale照旧使用）：\n  "
            + "\n  ".join(info["stale"]))
    return items, info


# ---------- 内核树 ----------

class Tree:
    def __init__(self, path):
        self.path = Path(path).resolve()
        r = subprocess.run(["git", "rev-parse", "--is-inside-work-tree"], cwd=self.path,
                           capture_output=True, text=True)
        if r.returncode or r.stdout.strip() != "true":
            die(f"{self.path}不是git工作树")
        name = subprocess.run(["git", "config", "user.name"], cwd=REPO, capture_output=True, text=True).stdout.strip()
        mail = subprocess.run(["git", "config", "user.email"], cwd=REPO, capture_output=True, text=True).stdout.strip()
        self.env = dict(os.environ, GIT_COMMITTER_NAME=name or "Akisaira",
                        GIT_COMMITTER_EMAIL=mail or "akisaira@outlook.com")
        self.state_path = Path(self.git("rev-parse", "--git-path", STATE_NAME))
        if not self.state_path.is_absolute():
            self.state_path = self.path / self.state_path

    def git(self, *args, check=True):
        r = subprocess.run(["git", *args], cwd=self.path, capture_output=True, text=True, env=self.env)
        if check and r.returncode:
            die(f"git {' '.join(args)}失败：{r.stderr.strip()}")
        return r.stdout.strip()

    def run(self, *args):
        return subprocess.run(["git", *args], cwd=self.path, capture_output=True, text=True, env=self.env)

    def head(self):
        return self.git("rev-parse", "HEAD")

    def has_commit(self, sha):
        return self.run("cat-file", "-e", f"{sha}^{{commit}}").returncode == 0

    def am_in_progress(self):
        p = Path(self.git("rev-parse", "--git-path", "rebase-apply"))
        if not p.is_absolute():
            p = self.path / p
        return p.exists()

    def reset_to(self, commit):
        if self.am_in_progress():
            die("内核树里有未结束的git am，先git am --abort或完成它")
        if self.git("status", "--porcelain", "--untracked-files=no"):
            die("内核树有未提交的改动")
        self.git("checkout", "-q", "--detach", commit)
        self.git("reset", "-q", "--hard", commit)

    def am(self, path):
        """打一个补丁。返回(状态, 冲突文件, 错误摘要)；状态为clean、noop或fail。"""
        pre = self.head()
        r = self.run("am", "-q", "-3", "--committer-date-is-author-date", str(path))
        if r.returncode == 0:
            return ("clean" if self.head() != pre else "noop"), [], ""
        files = self.git("diff", "--name-only", "--diff-filter=U", check=False).split()
        msg = next((l for l in (r.stderr + r.stdout).splitlines() if l.startswith(("error", "fatal"))), "")
        return "fail", files, msg


def load_state(tree):
    if not tree.state_path.exists():
        die(f"没有进度记录（{tree.state_path}），先运行tree")
    return json.loads(tree.state_path.read_text(encoding="utf-8"))


def save_state(tree, st):
    tree.state_path.write_text(json.dumps(st, ensure_ascii=False, indent=1), encoding="utf-8")


def cmd_prime(args):
    conf = read_conf()
    tree = Tree(args.tree)
    if not tree.has_commit(conf["OLD_BASE_COMMIT"]):
        die(f"内核树里没有{conf['OLD_BASE_TAG']}（{conf['OLD_BASE_COMMIT'][:12]}），先fetch")
    tree.reset_to(conf["OLD_BASE_COMMIT"])
    pats = sorted((HERE / "base").glob("*.patch")) + [p for _, p in dev_entries()]
    bad = []
    for p in pats:
        st, _, msg = tree.am(p)
        if st == "fail":
            tree.run("am", "--abort")
            bad.append(f"{p.name}：{msg}")
    tree.git("update-ref", PRIME_REF, tree.head())
    print(f"已在{conf['OLD_BASE_TAG']}上打完{len(pats) - len(bad)}/{len(pats)}个补丁，结果保存在{PRIME_REF}")
    for b in bad:
        print(f"  ✗ {b}")


def cmd_tree(args):
    conf = read_conf()
    tree = Tree(args.tree)
    if args.resume:
        st = load_state(tree)
        if tree.am_in_progress():
            die("git am尚未结束：解决冲突后git add并git am --continue，或git am --skip放弃这个补丁")
        if st.get("pending_pre") is None:
            die("没有等待处理的补丁")
        cur = st["items"][st["next"]]
        res = "resolved" if tree.head() != st["pending_pre"] else "skipped"
        st["results"][cur["key"]] = {"status": res}
        print(f"{cur['key']}：{'已手动解决' if res == 'resolved' else '已跳过'}")
        st["next"] += 1
        st["pending_pre"] = None
    else:
        items, info = build_series(strict=not args.allow_stale)
        if not tree.has_commit(conf["BASE_COMMIT"]):
            die(f"内核树里没有{conf['BASE_TAG']}（{conf['BASE_COMMIT'][:12]}），先fetch")
        tree.reset_to(conf["BASE_COMMIT"])
        st = {"base": conf["BASE_COMMIT"], "base_tag": conf["BASE_TAG"], "survey": args.survey,
              "items": [{"key": i["key"], "path": str(i["path"]), "source": i["source"],
                         "subject": i["subject"], "sha256": sha256(i["path"])} for i in items],
              "dropped": info["dropped"], "next": 0, "pending_pre": None, "results": {}}
        if info["stale"]:
            print("⚠ 以下override所基于的dev补丁已改动，本次照旧使用：" + ", ".join(info["stale"]))

    while st["next"] < len(st["items"]):
        it = st["items"][st["next"]]
        pre = tree.head()
        status, files, msg = tree.am(it["path"])
        if status == "fail":
            if st["survey"]:
                tree.run("am", "--abort")
                st["results"][it["key"]] = {"status": "conflict", "files": files, "error": msg}
            else:
                st["pending_pre"] = pre
                save_state(tree, st)
                print(f"\n✗ 第{st['next'] + 1}/{len(st['items'])}个补丁打不上：{it['key']}（来源：{it['source']}）")
                print(f"  {it['path']}")
                if files:
                    print("  冲突文件：" + " ".join(files))
                elif msg:
                    print(f"  {msg}")
                print(f"  解决后git add并git am --continue；要放弃这个补丁就git am --skip。然后：\n"
                      f"  {sys.argv[0]} tree {tree.path} --resume")
                sys.exit(2)
        else:
            st["results"][it["key"]] = {"status": status}
        st["next"] += 1
    save_state(tree, st)
    summarize(st)


def summarize(st):
    by = defaultdict(list)
    for it in st["items"]:
        r = st["results"].get(it["key"], {"status": "?"})
        by[r["status"]].append((it, r))
    names = {"clean": "打上", "noop": "空操作", "resolved": "手动解决", "skipped": "跳过", "conflict": "冲突"}
    print(f"\n基线{st['base_tag']}，补丁链{len(st['items'])}个（另有{len(st['dropped'])}个按drop.tsv丢弃）：")
    print("  " + "，".join(f"{names.get(k, k)}{len(v)}" for k, v in by.items()))
    if by.get("noop"):
        print("\n空操作（上游已包含，可加入drop.tsv）：")
        for it, _ in by["noop"]:
            print(f"  {it['key']}")
    if by.get("skipped"):
        print("\n跳过（如确属上游已有，应加入drop.tsv）：")
        for it, _ in by["skipped"]:
            print(f"  {it['key']}")
    if by.get("conflict"):
        print("\n冲突：")
        for it, r in by["conflict"]:
            print(f"  {it['key']}  {' '.join(r['files']) or r['error']}")
    if by.get("resolved"):
        print("\n手动解决的补丁需要运行record写回暂存区。")


# ---------- record ----------

def same_result(tree, sha, patch):
    """把来源补丁打到提交的父提交上（临时索引，不碰工作树），结果是否与该提交完全相同。
    同一组改动，git可能生成增删行排列不同的diff，只比增删行会误判。"""
    idx = Path(tree.git("rev-parse", "--git-path", "migration-7.4.index"))
    if not idx.is_absolute():
        idx = tree.path / idx
    env = dict(tree.env, GIT_INDEX_FILE=str(idx))
    try:
        def g(*a):
            return subprocess.run(["git", *a], cwd=tree.path, capture_output=True, text=True, env=env)
        if g("read-tree", f"{sha}^").returncode:
            return False
        if g("apply", "--cached", "--3way", str(patch)).returncode:
            return False
        t = g("write-tree")
        return t.returncode == 0 and t.stdout.strip() == tree.git("rev-parse", f"{sha}^{{tree}}")
    finally:
        idx.unlink(missing_ok=True)


def export_commit(tree, sha):
    r = tree.run("format-patch", "-1", "--stdout", "-N", "--no-signature", "--zero-commit", sha)
    if r.returncode:
        die(f"git format-patch {sha}失败：{r.stderr.strip()}")
    text = r.stdout  # 不能strip：补丁末尾可能是只有一个空格的上下文行
    lines, out, header = text.splitlines(), [], True
    for i, line in enumerate(lines):
        if i == 0 and re.match(r"^From 0{40} ", line):
            continue
        if header and line.startswith("Date: "):
            continue
        if header and not line.strip():
            header = False
        out.append(line)
    return "\n".join(out) + "\n"


def cmd_record(args):
    tree = Tree(args.tree)
    st = load_state(tree)
    if tree.am_in_progress():
        die("git am尚未结束")
    items, info = build_series(strict=False)
    by_subject = {it["subject"]: it for it in items}
    dev_path = {k: p for k, p in dev_entries()}
    commits = tree.git("rev-list", "--reverse", "--no-merges", f"{st['base']}..HEAD").split()
    ov = {c[0]: c for _, c in read_tsv("override.tsv")}
    changed, unknown, same_as_dev = [], [], []
    for sha in commits:
        subj = tree.git("log", "-1", "--format=%s", sha)
        it = by_subject.get(subj)
        if not it:
            unknown.append((sha, subj))
            continue
        text = export_commit(tree, sha)
        key = it["key"]
        # 来源补丁打到父提交上能得到同一棵树，就不需要写回；打不上（冲突后手动解决的）
        # 或结果不同（并入了fixup）才写回。只比增删行会把"只是上下文变了"的补丁漏掉。
        if same_result(tree, sha, it["path"]):
            continue
        if it["source"] in ("dev", "override"):
            sub, slug = key.split("/", 1)
            dp = dev_path[key]
            if it["source"] == "override" and same_result(tree, sha, dp):
                same_as_dev.append(key)
                continue
            op = HERE / "override" / sub / f"{slug}.patch"
            op.parent.mkdir(parents=True, exist_ok=True)
            op.write_text(text, encoding="utf-8")
            note = ov.get(key, [key, "", ""])[2] if len(ov.get(key, [])) > 2 else ""
            ov[key] = [key, sha256(dp), note]
            changed.append(f"{key} → override/{sub}/{slug}.patch")
        else:
            Path(it["path"]).write_text(text, encoding="utf-8")
            changed.append(f"{key} → {Path(it['path']).relative_to(HERE)}")
    header = [l for l in (HERE / "override.tsv").read_text(encoding="utf-8").splitlines() if l.startswith("#")]
    (HERE / "override.tsv").write_text(
        "\n".join(header + ["\t".join(ov[k]) for k in sorted(ov)]) + "\n", encoding="utf-8")
    print(f"写回{len(changed)}个补丁：")
    for c in changed:
        print(f"  {c}")
    if same_as_dev:
        print("\n以下补丁在树里与dev版本一致，override可删除（同时删掉override.tsv里的行）：")
        for k in same_as_dev:
            print(f"  {k}")
    if unknown:
        print("\n以下提交的Subject不在补丁链里（新提交请放进add/并登记add.tsv，修正请用fixup并入对应补丁）：")
        for sha, subj in unknown:
            print(f"  {sha[:12]} {subj}")


# ---------- check / series ----------

def cmd_check(args):
    items, info = build_series(strict=False)
    ok = True
    if info["stale"]:
        ok = False
        print("override所基于的dev补丁已改动，需要在新基线上重新解决：")
        for k in info["stale"]:
            print(f"  {k}")
    if info["dangling"]:
        ok = False
        print("drop.tsv或override.tsv里的键在dev和base层中都已不存在，可删除：")
        for k in info["dangling"]:
            print(f"  {k}")
    if info["unknown_after"]:
        ok = False
        print("add.tsv引用了不存在的键：" + ", ".join(info["unknown_after"]))
    src = defaultdict(int)
    for it in items:
        src[it["source"]] += 1
    print(f"补丁链{len(items)}个：" + "，".join(f"{k} {v}" for k, v in sorted(src.items()))
          + f"；丢弃{len(info['dropped'])}个")
    print("✓ 暂存区与dev一致" if ok else "✗ 有需要处理的项目")
    sys.exit(0 if ok else 1)


def cmd_series(args):
    items, _ = build_series(strict=False)
    for i, it in enumerate(items, 1):
        print(f"{i:3d}  {it['source']:8s}  {it['key']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("prime"); p.add_argument("tree"); p.set_defaults(fn=cmd_prime)
    p = sp.add_parser("tree"); p.add_argument("tree")
    p.add_argument("--survey", action="store_true", help="打不上的补丁记下后跳过，不停下")
    p.add_argument("--resume", action="store_true", help="解决冲突后继续")
    p.add_argument("--allow-stale", action="store_true", help="override所基于的dev补丁已改动时照旧使用")
    p.set_defaults(fn=cmd_tree)
    p = sp.add_parser("record"); p.add_argument("tree"); p.set_defaults(fn=cmd_record)
    p = sp.add_parser("check"); p.set_defaults(fn=cmd_check)
    p = sp.add_parser("series"); p.set_defaults(fn=cmd_series)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
