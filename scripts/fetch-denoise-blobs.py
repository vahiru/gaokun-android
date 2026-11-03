#!/usr/bin/env python3
"""Fetch the blobs the microphone noise suppression service needs (device/huawei/gaokun3/denoise/)
into device/huawei/gaokun3/denoise/prebuilt/, which git ignores.

None of them may go into this repository (denoise/prebuilt/README.md says why), so every build
fetches them from their public sources, pinned by sha256:

  * Qualcomm AI Runtime (QAIRT) 2.43.0.260128, the public "Community" download: the QNN HTP runtime
    for Android (libQnnHtp.so, libQnnHtpV68Stub.so, libQnnSystem.so, the v68 skel), its LICENSE.pdf
    and NOTICE.txt, and the QNN headers the service is compiled against.  Only these members are
    read, with HTTP range requests (about 10 MB of the 1.5 GB zip).  Downloading means accepting
    Qualcomm's licence in LICENSE.pdf.
  * fastrpc_shell_unsigned_3, the CDSP's unsigned PD shell, and the libc++.so.1 / libc++abi.so.1 the
    skel needs on the DSP, from qcsubsys_ext_cdsp8280.cab of the uup-drivers-sc8280xp release the
    firmware comes from (firmware/README.md).  Needs cabextract,
    which warns about extra bytes at the end of the cab: that is its Authenticode signature.
  * The FastRPC user library's sources (github.com/qualcomm/fastrpc, BSD-3-Clause) at a pinned
    commit: src/, inc/ and LICENSE.txt, built by denoise/Android.bp as libcdsprpc.

usage: python3 scripts/fetch-denoise-blobs.py [--check] [OUTDIR]
  --check  only verify what is there; exit 1 if anything is missing or different
"""
import fnmatch
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

QAIRT_URL = ("https://softwarecenter.qualcomm.com/api/download/software/sdks/"
             "Qualcomm_AI_Runtime_Community/All/2.43.0.260128/v2.43.0.260128.zip")
QAIRT_ROOT = "qairt/2.43.0.260128/"
QAIRT_FILES = {
    "lib/aarch64-android/libQnnHtp.so": "37b5235668e86b4a471d6e7e84670aa02cf4b2b8d4f2c55b67867a99115af39e",
    "lib/aarch64-android/libQnnHtpV68Stub.so": "59d095dceb861f7b4457213257c19e637c1b884ce30e66bff8eb8413f5278836",
    "lib/aarch64-android/libQnnSystem.so": "74417e815eebc0cf6e2d2150c3264fb21bd90370efe333847f6c2d6e6d98b2e2",
    "lib/hexagon-v68/unsigned/libQnnHtpV68Skel.so": "4f8dd0574acfe540bfcf9961faed043907d0e04f3bac2f3a812536e2a807e35c",
    "LICENSE.pdf": "ec1dccfdcba5c6e64126e84199b8362bf4999107bfa567ebe831dbb4c461692b",
    "NOTICE.txt": "c8a22fa8b9ed3e9266c4a3366965474cef5cb147ee2a1f33e488bbb24a071c09",
}
# The headers, as one hash over "sha256  path" lines sorted by path (see tree_hash()).
QAIRT_HEADERS = "include/QNN/*"
QAIRT_HEADERS_HASH = "bf89ac3470ec5a56229098b5be4862b6214f3f58f8eebf71c45ce68aa239acf3"

UUP_URL = ("https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/"
           "200.0.10.0/200.0.10.0.zip")
UUP_CAB = "qcsubsys_ext_cdsp8280.cab"
UUP_CAB_SHA = "5bae66febba91f116cfbf2a304ec1c6b78e824bb2d0a864059bb7cb05ce685b3"
# Members of the cab (all under CDSP/) -> dsp/<name>
DSP_FILES = {
    "fastrpc_shell_unsigned_3": "5e1fc09d57b14f49548d3eed0b5167622a896d74b8eb4a2caf528fc7e3c9c237",
    "libc++.so.1": "9549bbbb5bbe6beaa1856e94f44316ea32fce15c5d6c0d1e8347fe49228b06a4",
    "libc++abi.so.1": "f3cd64e087fe581dc4dd531b8eced8673ab143a76bf8839cd18ba7b9087c7fef",
}

FASTRPC_COMMIT = "a56e9d4de3614a8d8f4b0e15ff561350c4b686e3"
FASTRPC_URL = f"https://github.com/qualcomm/fastrpc/archive/{FASTRPC_COMMIT}.tar.gz"
# GitHub regenerates its archives, so the tarball itself is not pinned: the extracted trees are.
FASTRPC_TREES = {
    "src": "13eda7ea2cf1f929abe135c4574ded2ebb8ed53b6a2db4224d8516c7054ab791",
    "inc": "a5c8604c2645f32c0cd445093b1edab727ce7c0b71e2a600a11cd835bc96d7e1",
}
FASTRPC_LICENSE_SHA = "683e80f5436e6b0761139e626d2c764870ea0ed19b14916ef4c82772b99c4d44"


class HttpRangeFile(io.RawIOBase):
    """A remote file read with HTTP range requests, enough for zipfile."""

    def __init__(self, url):
        req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(req, timeout=120) as r:
            self.url = r.geturl()  # follow the redirect once
            cr = r.headers.get("Content-Range")
            if not cr:
                raise RuntimeError(url + ": the server ignores Range")
            self.size = int(cr.rsplit("/", 1)[1])
        self.pos = 0
        self.fetched = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = {0: off, 1: self.pos + off, 2: self.size + off}[whence]
        return self.pos

    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0:
            return 0
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{self.pos + n - 1}"})
        with urllib.request.urlopen(req, timeout=300) as r:
            # A server that ignores the range answers 200 with the whole 1.5 GB: refuse it.
            cr = r.headers.get("Content-Range", "")
            if r.status != 206 or not cr.startswith(f"bytes {self.pos}-"):
                raise OSError(f"{self.url}: no ranged answer (HTTP {r.status}, Content-Range {cr!r})")
            data = r.read(n)
        if len(data) != n:
            raise OSError(f"{self.url}: short read ({len(data)} of {n} bytes)")
        b[: len(data)] = data
        self.pos += len(data)
        self.fetched += len(data)
        return len(data)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree_hash(root, rel):
    lines = []
    base = os.path.join(root, rel)
    for d, _, files in os.walk(base):
        for name in files:
            p = os.path.join(d, name)
            lines.append(f"{sha256(p)}  {os.path.relpath(p, root)}")
    return hashlib.sha256("\n".join(sorted(lines, key=lambda l: l.split("  ", 1)[1])).encode()).hexdigest()


def remote_zip(url):
    f = HttpRangeFile(url)
    return f, zipfile.ZipFile(io.BufferedReader(f, buffer_size=1 << 20))


def extract(z, member, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with z.open(member) as src, open(dest + ".part", "wb") as out:
        shutil.copyfileobj(src, out, 1 << 20)
    os.replace(dest + ".part", dest)


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    check_only = "--check" in sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.abspath(args[0]) if args else os.path.join(here, "..", "device", "huawei", "gaokun3",
                                                              "denoise", "prebuilt")
    qdir = os.path.join(out, "qairt")
    ddir = os.path.join(out, "dsp")
    fdir = os.path.join(out, "fastrpc")

    def sums():
        lines = [f"{QAIRT_FILES[p]}  qairt/{p}\n" for p in sorted(QAIRT_FILES)]
        lines.append(f"{tree_hash(qdir, 'include/QNN')}  qairt/{QAIRT_HEADERS} (tree)\n")
        lines += [f"{DSP_FILES[n]}  dsp/{n}\n" for n in sorted(DSP_FILES)]
        lines.append(f"{FASTRPC_LICENSE_SHA}  fastrpc/LICENSE.txt\n")
        lines += [f"{FASTRPC_TREES[t]}  fastrpc/{t}/* (tree)\n" for t in sorted(FASTRPC_TREES)]
        return "".join(lines)

    def state():
        bad = [p for p, h in QAIRT_FILES.items()
               if not os.path.isfile(os.path.join(qdir, p)) or sha256(os.path.join(qdir, p)) != h]
        if not os.path.isdir(os.path.join(qdir, "include", "QNN")) or (
                QAIRT_HEADERS_HASH and tree_hash(qdir, "include/QNN") != QAIRT_HEADERS_HASH):
            bad.append(QAIRT_HEADERS)
        bad += ["dsp/" + n for n, h in DSP_FILES.items()
                if not os.path.isfile(os.path.join(ddir, n)) or sha256(os.path.join(ddir, n)) != h]
        if not all(os.path.isdir(os.path.join(fdir, t)) and tree_hash(fdir, t) == h for t, h in FASTRPC_TREES.items()) \
                or not os.path.isfile(os.path.join(fdir, "LICENSE.txt")) \
                or sha256(os.path.join(fdir, "LICENSE.txt")) != FASTRPC_LICENSE_SHA:
            bad.append("fastrpc/")
        if not bad:  # the list sync-device-tree.sh compares against
            try:
                with open(os.path.join(out, "SHA256SUMS")) as f:
                    if f.read() != sums():
                        bad.append("SHA256SUMS")
            except OSError:
                bad.append("SHA256SUMS")
        return bad

    bad = state()
    if not bad or check_only:
        print("denoise blobs: " + ("all present" if not bad else "missing or different: " + ", ".join(bad)))
        return 1 if bad else 0
    bad = [p for p in bad if p != "SHA256SUMS"]  # written below, nothing to fetch for it

    if any(not p.startswith(("dsp/", "fastrpc/")) for p in bad):
        print("fetching QAIRT members from", QAIRT_URL)
        f, z = remote_zip(QAIRT_URL)
        names = z.namelist()
        for p in QAIRT_FILES:
            extract(z, QAIRT_ROOT + p, os.path.join(qdir, p))
        shutil.rmtree(os.path.join(qdir, "include", "QNN"), ignore_errors=True)
        for n in names:
            if fnmatch.fnmatch(n, QAIRT_ROOT + QAIRT_HEADERS) and not n.endswith("/"):
                rel = n[len(QAIRT_ROOT):]
                # fnmatch's * also matches "/" and "..": keep every header inside include/QNN.
                if ".." in rel.split("/") or rel.startswith("/"):
                    sys.exit(f"{n}: unexpected member name in the QAIRT zip")
                extract(z, n, os.path.join(qdir, rel))
        print(f"  {f.fetched / 1e6:.1f} MB of {f.size / 1e6:.0f} MB read")
    if any(p.startswith("dsp/") for p in bad):
        if shutil.which("cabextract") is None:
            sys.exit("cabextract is needed for the CDSP files (apt install cabextract / brew install cabextract)")
        print("fetching", UUP_CAB, "from", UUP_URL)
        f, z = remote_zip(UUP_URL)
        with tempfile.TemporaryDirectory() as tmp:
            cab = os.path.join(tmp, UUP_CAB)
            extract(z, UUP_CAB, cab)
            if sha256(cab) != UUP_CAB_SHA:
                sys.exit(UUP_CAB + ": unexpected sha256")
            os.makedirs(ddir, exist_ok=True)
            for n in DSP_FILES:
                subprocess.run(["cabextract", "-q", "-d", tmp, "-F", "CDSP/" + n, cab], check=True)
                shutil.copyfile(os.path.join(tmp, "CDSP", n), os.path.join(ddir, n))

    if "fastrpc/" in bad:
        print("fetching", FASTRPC_URL)
        with tempfile.TemporaryDirectory() as tmp:
            tgz = os.path.join(tmp, "fastrpc.tar.gz")
            with urllib.request.urlopen(FASTRPC_URL, timeout=300) as r, open(tgz, "wb") as f:
                shutil.copyfileobj(r, f)
            # By hand rather than extractall(filter="data"), which older Pythons (macOS's 3.9) lack:
            # only regular files and directories under the expected top directory.
            with tarfile.open(tgz) as t:
                prefix = f"fastrpc-{FASTRPC_COMMIT}/"
                for m in t.getmembers():
                    parts = m.name.split("/")
                    if not (m.name + "/").startswith(prefix) or ".." in parts or m.name.startswith("/"):
                        sys.exit(f"{m.name}: unexpected member in the fastrpc archive")
                    dest = os.path.join(tmp, *parts)
                    if m.isdir():
                        os.makedirs(dest, exist_ok=True)
                    elif m.isfile():
                        os.makedirs(os.path.dirname(dest), exist_ok=True)
                        with t.extractfile(m) as src, open(dest, "wb") as dst:
                            shutil.copyfileobj(src, dst)
            top = os.path.join(tmp, f"fastrpc-{FASTRPC_COMMIT}")
            shutil.rmtree(fdir, ignore_errors=True)
            os.makedirs(fdir)
            for t in FASTRPC_TREES:
                shutil.copytree(os.path.join(top, t), os.path.join(fdir, t))
            shutil.copyfile(os.path.join(top, "LICENSE.txt"), os.path.join(fdir, "LICENSE.txt"))

    bad = [p for p in state() if p != "SHA256SUMS"]
    if bad:
        sys.exit("denoise blobs: still missing or different after fetching: " + ", ".join(bad))
    with open(os.path.join(out, "SHA256SUMS"), "w") as s:
        s.write(sums())
    print("denoise blobs: all present in", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
