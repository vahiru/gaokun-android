#!/usr/bin/env python3
"""把patches/android/下的补丁应用到crDroid源码树中device/huawei/gaokun3以外的项目。

    python3 <本仓库>/scripts/crdroid-tree-fixes.py <源码树>

patches/android/<目录>/对应源码树中的一个项目，对应关系见PROJECTS。对每个项目：

  * 工作区已等于“清单版本＋目录中的全部补丁”时不做任何改动；
  * 否则把项目恢复到清单版本（repo维护的refs/remotes/m/<清单分支>，执行
    git checkout -f --detach与git clean -fd），再按编号依次git apply目录中的补丁。
    **项目中的本地改动与未跟踪文件会被清除，HEAD停在清单版本上，本地提交不再参与构建。**
    恢复后内容没有变化的文件改回原来的修改时间，增量构建不会因此重新编译；
    所以执行期间不要同时构建。

repo sync因本地改动无法检出某个项目时，执行本脚本也会把该项目恢复到清单版本。
补丁无法应用（上游代码有变化）或项目缺失时报错，全部项目处理完后以1退出。

新增补丁：放进对应目录，编号接在末尾。新项目：新建目录，并在PROJECTS中登记项目路径。
某个目录的补丁全部删除后，PROJECTS中的条目先保留，已应用过补丁的树才会恢复到清单版本。
各补丁的原因与作用写在补丁说明中，目录一览见patches/README.md。
"""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

# patches/android/下的目录 -> 源码树中的项目路径
PROJECTS = {
    "build-make": "build/make",
    "drm-hwcomposer": "external/drm_hwcomposer",
    "frameworks-av": "frameworks/av",
    "frameworks-base": "frameworks/base",
    "frameworks-opt-net-wifi": "frameworks/opt/net/wifi",
    "hardware-interfaces": "hardware/interfaces",
    "hexagonrpc": "external/hexagonrpc",
    "minigbm": "external/minigbm",
    "packages-modules-wifi": "packages/modules/Wifi",
    "system-core": "system/core",
    "tinyalsa": "external/tinyalsa_new",
    "v4l2-codec2": "external/v4l2_codec2",
    "vendor-gapps": "vendor/gapps",
    "vendor-lineage": "vendor/lineage",
    "webrtc": "external/webrtc",
}
# 未安装GApps时不同步vendor/gapps，此时跳过
OPTIONAL = {"vendor-gapps"}

PATCHES = pathlib.Path(__file__).resolve().parent.parent / "patches" / "android"


class Failed(Exception):
    pass


def git(proj, *args, index=None):
    env = None
    if index:
        env = dict(os.environ, GIT_INDEX_FILE=str(index))
    r = subprocess.run(["git", "-C", str(proj), *args], capture_output=True, text=True, env=env)
    if r.returncode:
        msg = (r.stderr.strip() or r.stdout.strip() or "?").splitlines()[0]
        raise Failed(f"git {args[0]}失败：{msg}")
    return r.stdout.strip()


def manifest_branch(tree):
    """repo init -b指定的清单分支，取法与repo相同：.repo/manifests.git中的branch.default.merge。

    返回(分支, None)，取不到时返回(None, 原因)。
    """
    cfg = tree / ".repo" / "manifests.git" / "config"
    if not cfg.exists():
        return None, "未找到.repo/manifests.git"
    r = subprocess.run(["git", "config", "--file", str(cfg), "branch.default.merge"],
                       capture_output=True, text=True)
    branch = r.stdout.strip()
    if not branch:
        return None, ".repo/manifests.git中没有branch.default.merge"
    return (branch[len("refs/heads/"):] if branch.startswith("refs/heads/") else branch), None


def manifest_ref(proj, branch, why):
    """repo为每个项目维护的清单版本引用refs/remotes/m/<清单分支>。

    切换过清单分支的树里可能留有旧分支的m/引用，所以按清单分支取，不能只看有几个。
    取不到清单分支时（例如不是repo检出的树），要求恰好有一个m/引用。
    """
    if branch:
        ref = f"refs/remotes/m/{branch}"
        r = subprocess.run(["git", "-C", str(proj), "rev-parse", "-q", "--verify", ref + "^{commit}"],
                           capture_output=True, text=True)
        if r.returncode:
            raise Failed(f"缺少清单版本引用{ref}，请先执行repo sync")
        return ref
    refs = git(proj, "for-each-ref", "--format=%(refname)", "refs/remotes/m/").split()
    if len(refs) != 1:
        raise Failed(f"无法确定清单版本：{why}，refs/remotes/m/下有{len(refs)}个引用")
    return refs[0]


def changed_paths(proj, base, want, added_only=False):
    """清单版本与预期的树之间有差别的路径（added_only时只列新增的路径）。"""
    args = ["diff-tree", "-r", "-z", "--no-renames", "--name-only"]
    if added_only:
        args.append("--diff-filter=A")
    return [p for p in git(proj, *args, base, want).split("\0") if p]


def expected_tree(proj, base, patches, tmp):
    """在临时索引中算出“清单版本＋全部补丁”的树，不改动工作区。"""
    index = tmp / "expected"
    git(proj, "read-tree", base, index=index)
    for p in patches:
        try:
            git(proj, "apply", "--cached", str(p), index=index)
        except Failed as e:
            raise Failed(f"{p.name}无法应用到清单版本（{e}）")
    return git(proj, "write-tree", index=index)


def worktree_tree(proj, created, tmp):
    """工作区（含未跟踪文件，不含被忽略的文件）对应的树。

    复制项目自己的索引再git add -A：索引中的文件状态信息仍然有效，只有改动过的文件需要重新读取。
    复制时保留索引文件的修改时间，git才能识别与索引同一秒写入的改动。
    补丁新建的文件即使被忽略规则匹配，也要计入。
    """
    index = tmp / "worktree"
    real = proj / git(proj, "rev-parse", "--git-path", "index")  # 相对于项目目录，或已是绝对路径
    if real.exists():
        shutil.copy2(real, index)
    else:
        git(proj, "read-tree", "HEAD", index=index)
    git(proj, "add", "-A", index=index)
    present = [c for c in created if (proj / c).is_file()]
    if present:
        git(proj, "add", "-f", "--", *present, index=index)
    return git(proj, "write-tree", index=index)


def snapshot(proj, paths):
    """记下这些文件的修改时间与内容摘要。"""
    snap = {}
    for rel in set(paths):
        f = proj / rel
        if f.is_file():
            snap[rel] = (f.stat().st_mtime_ns, hashlib.sha256(f.read_bytes()).digest())
    return snap


def keep_mtimes(proj, snap):
    """内容与恢复前相同的文件，改回原来的修改时间。"""
    for rel, (mtime, digest) in snap.items():
        f = proj / rel
        if f.is_file() and hashlib.sha256(f.read_bytes()).digest() == digest:
            os.utime(f, ns=(f.stat().st_atime_ns, mtime))


def fix_project(proj, branch, why, patches):
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        base = manifest_ref(proj, branch, why)
        want = expected_tree(proj, base, patches, tmp)
        created = changed_paths(proj, base, want, added_only=True)
        if worktree_tree(proj, created, tmp) == want:
            return f"已是最新（{len(patches)}个补丁）"
        snap = snapshot(proj, changed_paths(proj, base, want))
        # 先刷新索引中的文件状态（与repo的做法相同）：树被复制过或文件属性变过时状态信息已过期，
        # checkout -f会重写内容并未变化的文件
        subprocess.run(["git", "-C", str(proj), "update-index", "-q", "--unmerged", "--ignore-missing",
                        "--refresh"], capture_output=True)
        git(proj, "checkout", "-q", "-f", "--detach", base)
        git(proj, "clean", "-q", "-f", "-d")
        for c in created:  # 被忽略规则匹配时git clean不会删除，git apply会因文件已存在而失败
            if (proj / c).is_file():
                (proj / c).unlink()
        for p in patches:
            try:
                git(proj, "apply", str(p))
            except Failed as e:
                raise Failed(f"{p.name}无法应用（{e}）")
        keep_mtimes(proj, snap)
        if worktree_tree(proj, created, tmp) != want:
            raise Failed("应用补丁后的工作区与预期不一致")
        return f"已恢复到清单版本并应用{len(patches)}个补丁"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = pathlib.Path(sys.argv[1]).expanduser().resolve()
    if not (tree / "build/envsetup.sh").exists():
        sys.exit(f"✗ {tree}不是Android源码树")
    print(f"源码树：{tree}")
    branch, why = manifest_branch(tree)
    failed = []
    unknown = sorted(d.name for d in PATCHES.iterdir() if d.is_dir() and d.name not in PROJECTS)
    for d in unknown:
        print(f"  ✗ patches/android/{d}：未在PROJECTS中登记")
        failed.append(d)
    for name, path in PROJECTS.items():
        patches = sorted((PATCHES / name).glob("*.patch"))
        proj = tree / path
        label = f"  {path}（patches/android/{name}）："
        if not proj.exists() and name in OPTIONAL:
            print(label + "跳过（未同步）")
            continue
        if not (proj / ".git").exists():
            print(label + "✗ 项目不存在或不是git仓库")
            failed.append(name)
            continue
        try:
            print(label + fix_project(proj, branch, why, patches))
        except Failed as e:
            print(label + f"✗ {e}")
            failed.append(name)
    if failed:
        print(f"✗ {len(failed)}个项目失败：" + "、".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
