#!/usr/bin/env python3
"""Let the built-in microphone PCM open with four channels.

Rewrites the gaokun3 audio topology in place: the capabilities of "MultiMedia4 Capture"
(hw:0,3) go from channels_max 2 to 4, one byte. The file this machine ships is the
ThinkPad X13s topology, byte for byte: audioreach-topology fe7d4c4,
prebuilt/qcom/sc8280xp/LENOVO/21BX/audioreach-tplg.bin. In its source, audioreach.conf in
the same directory, the byte is channels_max of SectionPCMCapabilities."MultiMedia4 Capture".

Usage: tplg-four-channel-capture.py device/huawei/gaokun3/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
Runs once; a file that already allows four channels is left alone.
"""
import hashlib
import os
import sys

TWO_CHANNELS = "ee911c748deadc0191af50074b26ad177c8a2e78ff00e9578504b71d5d27b08a"
FOUR_CHANNELS = "8f5f040b181eab482576b8c48c2bd5d55d8c9b4dfc364bc7f067fc07e3e10765"
OFFSET = 19056


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    data = bytearray(open(path, "rb").read())
    digest = hashlib.sha256(data).hexdigest()
    if digest == FOUR_CHANNELS:
        print(f"{path}: already four channels")
        return
    if digest != TWO_CHANNELS or data[OFFSET] != 2:
        sys.exit(f"{path}: not the topology this script knows (sha256 {digest})")
    data[OFFSET] = 4
    assert hashlib.sha256(data).hexdigest() == FOUR_CHANNELS
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    print(f"{path}: MultiMedia4 Capture now allows four channels (sha256 {FOUR_CHANNELS})")


if __name__ == "__main__":
    main()
