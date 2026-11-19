# SPDX-License-Identifier: MIT
"""Pack verified DXBC edits from local structural-transform manifests."""

import argparse
from difflib import SequenceMatcher
import json
from pathlib import Path
import struct

from BuildRecipe import recipe


def instructions(path):
    data = Path(path).read_bytes()
    if data[:4] != b"DXBC":
        raise ValueError("Expected DXBC")
    count = struct.unpack_from("<I", data, 28)[0]
    for offset in struct.unpack_from(f"<{count}I", data, 32):
        if data[offset:offset + 4] in (b"SHDR", b"SHEX"):
            size = struct.unpack_from("<I", data, offset + 4)[0]
            if offset + 8 + size != len(data):
                raise ValueError("Shader code must be the final chunk")
            code = data[offset + 8:]
            break
    else:
        raise ValueError("No shader code found")
    tokens = []
    pos = 8
    while pos < len(code):
        token = struct.unpack_from("<I", code, pos)[0]
        words = struct.unpack_from("<I", code, pos + 4)[0] if token & 0x7ff == 53 else token >> 24 & 0x7f
        size = words * 4
        if size == 0 or pos + size > len(code):
            raise ValueError("Invalid instruction length")
        tokens.append(code[pos:pos + size])
        pos += size
    return code, tokens


def entry(source, candidate):
    original, before = instructions(source)
    changed, after = instructions(candidate)
    offsets = [8]
    for instruction in before:
        offsets.append(offsets[-1] + len(instruction))
    changes = [i for action, i, _, _, _ in SequenceMatcher(None, before, after, autojunk=False).get_opcodes()
               if action != "equal"]
    anchors = list(dict.fromkeys(min(offsets[i], len(original) - 32) for i in changes))
    if len(anchors) < 2:
        raise ValueError("Expected separate structural anchors")
    patch = recipe(original, changed)
    return (struct.pack("<II", len(anchors), len(patch)) +
            b"".join(struct.pack("<I", i) + original[i:i + 32] for i in anchors) + patch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifests", nargs="+", type=Path)
    args = parser.parse_args()
    matches = [match for path in args.manifests for match in json.loads(path.read_text())]
    entries = [entry(match["source"], match["candidate"]) for match in matches]
    args.output.write_bytes(b"KOEVIS02" + struct.pack("<I", len(entries)) + b"".join(entries))


if __name__ == "__main__":
    main()
