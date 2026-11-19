# SPDX-License-Identifier: MIT
"""Limit wetness by the geometric normal in EVE/GBufferWetEffect."""

import re

SHADER_NAME = "EVE/GBufferWetEffect"

# Projective and camera-relative reconstruction, respectively.
POSITION = (
    re.compile(
        r"mad (r\d+)\.xyzw, cb2\[20\]\.xyzw, -r\d+\.zzzz, r\d+\.xyzw\n"
        r"add \1\.xyzw, \1\.xyzw, cb2\[21\]\.xyzw\n"
        r"div \1\.xyz, \1\.xyzx, \1\.wwww"
    ),
    re.compile(
        r"mad (r\d+)\.xyz, cb0\[22\]\.xyzx, \1\.zzzz, \1\.xywx\n"
        r"add \1\.xyz, \1\.xyzx, cb0\[23\]\.xyzx\n"
        r"dp3 \1\.w, \1\.xyzx, \1\.xyzx\n"
        r"sqrt \1\.w, \1\.w"
    ),
)
NORMAL = re.compile(
    r"sample_l (r\d+)\.xyzw, v0\.xyxx, t4\.xyzw, s1, l\(0\.000000\)\n"
    r"mad (r\d+)\.(xyz|yzw), \1\.(?:xyzx|xxyz), l\([^\n]+\), l\([^\n]+\)\n"
    r"dp3 (r\d+\.[xyzw]), \2\.(?:xyzx|yzwy), (cb0\[0\]\.xyzx)"
)


def transform(assembly):
    lines = assembly.splitlines()
    code = "\n".join(line.strip() for line in lines)
    if not re.search(r"^ps_(?:4_[01]|5_0)$", code, re.M):
        raise ValueError("Expected a pixel shader")
    declarations = list(re.finditer(r"^dcl_temps (\d+)$", code, re.M))
    positions = [match for pattern in POSITION for match in pattern.finditer(code)]
    normals = list(NORMAL.finditer(code))
    if len(declarations) != 1 or len(positions) != 1 or len(normals) != 1:
        raise ValueError("Unknown wet-surface reconstruction")
    position, normal = positions[0], normals[0]
    if not declarations[0].end() < position.start() < normal.start():
        raise ValueError("Unexpected wet-surface instruction order")

    normal_component, up = normal.group(4, 5)
    tail = code[normal.end():]
    threshold = re.compile(
        rf"(?m)^add {re.escape(normal_component)}, {re.escape(normal_component)}, "
        r"l\(-0\.985000\)\nmul_sat "
    )
    cutoff = threshold.search(tail)
    if not cutoff:
        raise ValueError("Expected the wet-surface slope threshold")
    if f"min {normal_component}, {normal_component}, |" in tail[:cutoff.start()]:
        raise ValueError("Wet-surface slope is already limited")

    first = int(declarations[0][1])
    dx, dy, cross = (f"r{i}" for i in range(first, first + 3))
    point = position[1]
    geometric_slope = [
        f"deriv_rtx {dx}.xyz, {point}.xyzx",
        f"deriv_rty {dy}.xyz, {point}.xyzx",
        f"mul {cross}.xyz, {dx}.yzxy, {dy}.zxyz",
        f"mad {cross}.xyz, -{dx}.zxyz, {dy}.yzxy, {cross}.xyzx",
        f"dp3 {dx}.w, {cross}.xyzx, {cross}.xyzx",
        f"max {dx}.w, {dx}.w, l(0.00000000000000000001)",
        f"rsq {dx}.w, {dx}.w",
        f"mul {cross}.xyz, {cross}.xyzx, {dx}.wwww",
        f"dp3 {dx}.w, {cross}.xyzx, {up}",
    ]
    normal_index = code.count("\n", 0, normal.end())
    position_index = code.count("\n", 0, position.end())
    declaration_index = code.count("\n", 0, declarations[0].start())
    lines[normal_index + 1:normal_index + 1] = [
        f"min {normal_component}, {normal_component}, |{dx}.w|"
    ]
    lines[position_index + 1:position_index + 1] = geometric_slope
    lines[declaration_index] = f"dcl_temps {first + 3}"
    return "\n".join(lines) + "\n"
