#!/usr/bin/env python3
"""R2 桶的滚动清理：按保留规则删掉过期的发版产物。默认只打印计划，加 --go 才删。

    r2-prune.py [--go] [--only official|staging|installer] [--bucket gaokun-android]
                [--keep-official 2] [--keep-staging 2] [--keep-installer 2]

凭据同 r2-upload.py（R2_ENDPOINT / R2_ACCESS_KEY_ID / R2_SECRET_ACCESS_KEY，只从环境变量读）。
release.sh 在发版末尾自动跑它（正式发版：全部三类；--stage-only：只滚 staging）。

保留规则（用户 2026-10-11 定）：
  official   一个"版本"= builds/<名>.zip（连同 .zip.sha256sum）+ install/<名>/。
             ★ OTA 清单 ota/gaokun3.json 指向的那一版（= Updater 与图形安装器联网安装取的那一版，
               Windows 安装器的 installer/windows/lib/backend/net.dart）永远保留，另留按日期排在它前面的 --keep-official 版（默认 2）。
             比当前版还新的（清单还没指过去的）一律不动。
  staging    staging/<名>/ 一组算一份，按组里最新的 LastModified 排序，留最新 --keep-staging 组（默认 2）。
  installer  installer/<版本>/ 同 staging，留最新 --keep-installer 个（默认 2）。installer/ 根下的文件
             （variants.txt）不动。
  另外三类都保留【最新正式 release】与【最新 pre-release】的 GitHub 发布页正文里链接到的分组
  （正文里的 ota.072172.xyz/<前缀>/<名> 链接）—— 贴出去的下载链接在它还是"最新"的时候不能 404。
  其余前缀（ota/、relnotes/、不认识的）一概不碰。

失败即停（fail-safe）：GitHub API 取不到、OTA 清单读不到、清单指向的 zip 在 builds/ 里不存在 ——
任何一条都不删任何东西、退出码 1。删完回查当前版的 zip 仍是 HTTP 200。
⚠️ 删掉的是对外可见的下载链接（更旧的 GitHub 发布页里的 R2 链接会 404），撤不回。
"""

import argparse
import importlib.util
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('r2upload', os.path.join(HERE, 'r2-upload.py'))
r2 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(r2)

GH_API = 'https://api.github.com/repos/vahiru/gaokun-android/releases?per_page=50'
HOST_RE = re.compile(r'ota\.072172\.xyz/(builds|install|staging|installer)/([A-Za-z0-9._-]+)')
DATE_RE = re.compile(r'-(\d{8})-')


def official_name(key):
    """builds/<名>.zip[.sha256sum] 或 install/<名>/… → <名>；别的返回 None。"""
    parts = key.split('/')
    if parts[0] == 'builds' and len(parts) == 2:
        m = re.match(r'(.+?)\.zip(\.sha256sum)?$', parts[1])
        return m.group(1) if m else None
    if parts[0] == 'install' and len(parts) >= 3:
        return parts[1]
    return None


def github_refs():
    """最新正式 release 与最新 pre-release 正文里链接到的 {(类, 名)}。取不到就抛异常（调用方据此整体停手）。"""
    req = urllib.request.Request(GH_API, headers={'Accept': 'application/vnd.github+json',
                                                  'User-Agent': 'gaokun-android-r2-prune'})
    with urllib.request.urlopen(req, timeout=60) as resp:
        rels = json.load(resp)
    rels = [r for r in rels if not r.get('draft')]
    rels.sort(key=lambda r: r.get('published_at') or '', reverse=True)
    picked = []
    for want_pre in (False, True):
        for r in rels:
            if bool(r.get('prerelease')) == want_pre:
                picked.append(r)
                break
    refs = set()
    for r in picked:
        for prefix, name in HOST_RE.findall(r.get('body') or ''):
            if prefix == 'builds':
                name = re.sub(r'\.zip(\.sha256sum)?$', '', name)
            kind = 'official' if prefix in ('builds', 'install') else prefix
            refs.add((kind, name))
    return [r['tag_name'] for r in picked], refs


def current_official(bucket):
    code, body = r2._send('GET', '/%s/ota/gaokun3.json' % bucket, {}, want_body=True)
    if code != 200:
        raise SystemExit('✗ 读不到 ota/gaokun3.json（HTTP %s）—— 不删任何东西' % code)
    fn = json.loads(body)['response'][0]['filename']
    return re.sub(r'\.zip$', '', fn)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--go', action='store_true', help='真删（默认只打印计划）')
    ap.add_argument('--only', choices=('official', 'staging', 'installer'))
    ap.add_argument('--bucket', default=os.environ.get('BUCKET', 'gaokun-android'))
    ap.add_argument('--keep-official', type=int, default=2)
    ap.add_argument('--keep-staging', type=int, default=2)
    ap.add_argument('--keep-installer', type=int, default=2)
    a = ap.parse_args()

    try:
        tags, refs = github_refs()
    except Exception as e:
        print('✗ 取不到 GitHub 发布页（%s）—— 不知道哪些链接还贴在外面，不删任何东西' % e)
        return 1
    cur = current_official(a.bucket)
    objs = r2.list_objects(a.bucket, with_mtime=True)

    groups = {}                                  # (类, 名) -> [(key, size, mtime)]
    for k, sz, mt in objs:
        p = k.split('/')
        if official_name(k):
            g = ('official', official_name(k))
        elif p[0] in ('staging', 'installer') and len(p) >= 3:
            g = (p[0], p[1])
        else:
            continue                             # ota/、relnotes/、installer/variants.txt、不认识的：不碰
        groups.setdefault(g, []).append((k, sz, mt))

    if ('official', cur) not in groups or not any(k == 'builds/%s.zip' % cur for k, _, _ in groups[('official', cur)]):
        print('✗ OTA 清单指向 %s.zip，但 builds/ 里没有它 —— 桶的状态不对，不删任何东西' % cur)
        return 1

    newest = lambda g: max(mt for _, _, mt in groups[g])
    plan_keep, plan_del = {}, {}

    def decide(ordered, keep_set):
        for g in ordered:
            (plan_keep if g in keep_set or g in refs else plan_del)[g] = groups[g]

    if a.only in (None, 'official'):
        def date_of(g):
            m = DATE_RE.search(g[1])
            return m.group(1) if m else newest(g)[:10].replace('-', '')
        off = sorted((g for g in groups if g[0] == 'official'), key=lambda g: (date_of(g), g[1]))
        i = off.index(('official', cur))
        keep = set(off[max(0, i - a.keep_official):])   # 当前、它前面 N 版、以及比它新的
        decide(off, keep)
    for kind, n in (('staging', a.keep_staging), ('installer', a.keep_installer)):
        if a.only in (None, kind):
            gs = sorted((g for g in groups if g[0] == kind), key=newest, reverse=True)
            decide(gs, set(gs[:n]))

    hs = r2._human
    print('当前版（OTA 清单）：%s · GitHub 最新发布页：%s · 桶 %s' % (cur, ' / '.join(tags), a.bucket))
    for title, plan in (('保留', plan_keep), ('删除', plan_del)):
        tot = sum(sz for v in plan.values() for _, sz, _ in v)
        print('── %s：%d 组，%s ──' % (title, len(plan), hs(tot)))
        for g in sorted(plan):
            why = ''
            if title == '保留':
                why = ('  ← OTA 清单' if g == ('official', cur) else
                       '  ← GitHub 发布页引用' if g in refs else '')
            print('  %-9s %-52s %4d 个 %10s%s' % (g[0], g[1], len(plan[g]),
                                                 hs(sum(sz for _, sz, _ in plan[g])), why))
    if not plan_del:
        print('✓ 没有要删的')
        return 0
    if not a.go:
        print('（只打印计划；加 --go 才删）')
        return 0

    bad = 0
    for g in sorted(plan_del):
        for k, _, _ in sorted(plan_del[g]):
            assert official_name(k) != cur, k    # 双保险：当前版永远不进删除集
            if not r2.delete(a.bucket, k):
                bad += 1
    code, _ = r2._send('HEAD', '/%s/builds/%s.zip' % (a.bucket, cur), {}, want_headers=True)
    if code != 200:
        print('✗ 删完回查：当前版 builds/%s.zip 是 HTTP %s！' % (cur, code))
        return 1
    print('✓ 删完回查：当前版 builds/%s.zip 仍是 HTTP 200' % cur)
    if bad:
        print('✗ %d 个对象没删掉（见上）' % bad)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
