#!/usr/bin/env python3
"""Upload release artifacts to a Cloudflare R2 bucket (S3 API, SigV4).

Standard library only -- no boto3, no requests, no awscli. This runs on a bare
Debian build machine as-is, which is the point: the artifacts are 2.4 GiB and
uploading them cloud-to-cloud from the build host is far faster than pulling
them down to a laptop first and pushing them back up.

Credentials come from the environment, never from arguments or a file in the
repo:

    R2_ENDPOINT             https://<account-id>.r2.cloudflarestorage.com
    R2_ACCESS_KEY_ID
    R2_SECRET_ACCESS_KEY

Only the S3 keys are needed. Do NOT put a Cloudflare account API token on a
build machine to do this -- an account token can rewrite DNS and delete
buckets, while these keys can only touch object storage.

    r2-upload.py <bucket> <local-file> <key> [content-type]
    r2-upload.py --delete <bucket> <key>

Notes
  * Signs with x-amz-content-sha256: UNSIGNED-PAYLOAD. R2 accepts that over
    HTTPS and it avoids reading a multi-GB file twice (once to hash, once to
    send).
  * Single PUT, no multipart. R2's single-object PUT limit is 5 GiB, and the
    largest thing here is ~1.3 GiB.
  * Retries on transient TLS/connection failures, which this project sees
    regularly against both Cloudflare and Azure.
"""

import datetime
import hashlib
import hmac
import os
import sys
import time
import urllib.error
import urllib.request

UNSIGNED = 'UNSIGNED-PAYLOAD'
REGION = 'auto'
SERVICE = 's3'


def _sign(key, msg):
    return hmac.new(key, msg.encode('utf-8'), hashlib.sha256).digest()


def _auth_headers(method, host, canon_uri, extra, canon_qs=''):
    ak = os.environ['R2_ACCESS_KEY_ID']
    sk = os.environ['R2_SECRET_ACCESS_KEY']

    now = datetime.datetime.now(datetime.timezone.utc)
    amzdate = now.strftime('%Y%m%dT%H%M%SZ')
    datestamp = now.strftime('%Y%m%d')

    headers = {'host': host, 'x-amz-content-sha256': UNSIGNED, 'x-amz-date': amzdate}
    headers.update(extra)

    signed = ';'.join(sorted(headers))
    canon_headers = ''.join('%s:%s\n' % (k, headers[k]) for k in sorted(headers))
    # SigV4 的规范请求第 3 行是【规范查询串】。
    # 原来这里写死成空行，所以只能签无 query 的请求；
    # ListObjectsV2 必须带 ?list-type=2。
    # ⚠️ canon_headers 自己已以换行结尾，而规范要求它后面再加一个换行
    # （于是那里是一个空行），少了它签名不匹配。
    canon_req = chr(10).join([method, canon_uri, canon_qs,
                              canon_headers + chr(10) + signed, UNSIGNED])

    scope = '%s/%s/%s/aws4_request' % (datestamp, REGION, SERVICE)
    to_sign = 'AWS4-HMAC-SHA256\n%s\n%s\n%s' % (
        amzdate, scope, hashlib.sha256(canon_req.encode()).hexdigest())

    k = _sign(('AWS4' + sk).encode(), datestamp)
    for part in (REGION, SERVICE, 'aws4_request'):
        k = _sign(k, part)
    sig = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()

    headers['Authorization'] = (
        'AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s'
        % (ak, scope, signed, sig))
    return headers


def _send(method, canon_uri, extra, body=None, attempts=4, qs='', want_body=False):
    ep = os.environ['R2_ENDPOINT'].rstrip('/')
    host = ep.split('://', 1)[1]
    last = None
    for attempt in range(attempts):
        # Re-sign every attempt: SigV4 signatures expire, and a retry minutes
        # after the first try would be rejected as skewed rather than retried.
        headers = _auth_headers(method, host, canon_uri, extra, qs)
        try:
            url = ep + canon_uri + (('?' + qs) if qs else '')
            req = urllib.request.Request(
                url, data=body() if body else None,
                headers=headers, method=method)
            with urllib.request.urlopen(req, timeout=3600) as resp:
                return resp.status, (resp.read() if want_body else b'')
        except urllib.error.HTTPError as e:
            # A 4xx will not fix itself; fail immediately with the server's
            # explanation rather than retrying three more times.
            return e.code, e.read()[:500].decode('utf-8', 'replace')
        except Exception as e:                      # TLS EOF, reset, timeout
            last = e
            if attempt < attempts - 1:
                time.sleep(5 * (attempt + 1))
    raise last


def put(bucket, path, key, ctype):
    size = os.path.getsize(path)
    code, msg = _send(
        'PUT', '/%s/%s' % (bucket, key),
        {'content-length': str(size), 'content-type': ctype},
        body=lambda: open(path, 'rb'))
    ok = code in (200, 201)
    print('  %-56s %8.1f MiB  HTTP %s%s'
          % (key, size / 1048576.0, code, '' if ok else '  ' + msg))
    return ok


def delete(bucket, key):
    code, msg = _send('DELETE', '/%s/%s' % (bucket, key), {})
    ok = code in (200, 204)
    print('  deleted %-48s HTTP %s%s' % (key, code, '' if ok else '  ' + msg))
    return ok



def list_objects(bucket, prefix=''):
    """列出桶里的对象。返回 [(key, size), ...]。

    ★ 需要 ListObjectsV2，也就是【带查询串】的 GET —— 这正是本文件的 SigV4
      原来签不了的那种请求（规范请求第 3 行以前写死成空行）。
    ⚠️ 规范查询串必须**按键名排序**并逐个百分号编码，顺序错了签名就不匹配。
    """
    import xml.etree.ElementTree as ET
    from urllib.parse import quote

    out, token = [], None
    while True:
        params = {'list-type': '2', 'max-keys': '1000'}
        if prefix:
            params['prefix'] = prefix
        if token:
            params['continuation-token'] = token
        qs = '&'.join('%s=%s' % (quote(k, safe=''), quote(v, safe=''))
                      for k, v in sorted(params.items()))
        code, body = _send('GET', '/%s' % bucket, {}, qs=qs, want_body=True)
        if code != 200:
            raise SystemExit('ListObjectsV2 HTTP %s: %s'
                             % (code, body[:300].decode('utf-8', 'replace')))
        root = ET.fromstring(body)
        tag = lambda e: e.tag.split('}')[-1]      # 去掉 XML 命名空间前缀
        trunc, token = False, None
        for child in root:
            n = tag(child)
            if n == 'Contents':
                k = sz = None
                for f in child:
                    if tag(f) == 'Key':
                        k = f.text
                    elif tag(f) == 'Size':
                        sz = int(f.text)
                if k is not None:
                    out.append((k, sz or 0))
            elif n == 'IsTruncated':
                trunc = (child.text == 'true')
            elif n == 'NextContinuationToken':
                token = child.text
        if not (trunc and token):
            break
    return out


def _human(n):
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if n < 1024 or unit == 'GiB':
            return '%.1f %s' % (n, unit)
        n /= 1024.0


def cmd_list(bucket, prefix=''):
    objs = sorted(list_objects(bucket, prefix))
    for k, sz in objs:
        print('  %-64s %10s' % (k, _human(sz)))
    print('  --- %d 个对象，共 %s ---' % (len(objs), _human(sum(s for _, s in objs))))
    return True


def cmd_du(bucket):
    """按顶层前缀汇总占用 —— 看清存储花在哪里。"""
    objs = list_objects(bucket)
    agg = {}
    for k, sz in objs:
        parts = k.split('/')
        top = parts[0] if len(parts) == 1 else '/'.join(parts[:2])
        agg[top] = agg.get(top, [0, 0])
        agg[top][0] += sz
        agg[top][1] += 1
    for top in sorted(agg, key=lambda t: -agg[t][0]):
        sz, n = agg[top]
        print('  %-52s %10s  (%d 个)' % (top, _human(sz), n))
    print('  === 合计 %s / %d 个对象 ===' % (_human(sum(s for _, s in objs)), len(objs)))
    return True


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(2)
    if args[0] == '--list':
        sys.exit(0 if cmd_list(args[1], args[2] if len(args) > 2 else '') else 1)
    if args[0] == '--du':
        sys.exit(0 if cmd_du(args[1]) else 1)
    if args[0] == '--delete':
        sys.exit(0 if delete(args[1], args[2]) else 1)
    bucket, path, key = args[0], args[1], args[2]
    ctype = args[3] if len(args) > 3 else 'application/octet-stream'
    sys.exit(0 if put(bucket, path, key, ctype) else 1)


if __name__ == '__main__':
    main()
