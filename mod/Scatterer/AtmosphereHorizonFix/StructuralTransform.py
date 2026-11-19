# SPDX-License-Identifier: MIT
"""Blend the scattering lookup across its ground-tangent discontinuity."""

import re

SHADER_NAME = "Scatterer/DepthBufferScattering"
RADIUS_OFFSET = "add r2.y, r7.w, -cb0[20].x"
ZERO = "l(0.000000, 0.000000, 0.000000, 0.000000)"
TEMP = re.compile(r"\br(\d+)(?:\.([xyzw]+))?")
OPERATIONS = set(
    "add and div div_sat exp frc ge iadd itof log lt mad max min mov movc mul "
    "not or round_ni rsq sample_l sqrt".split()
)


def live_inputs(block):
    defined = set()
    live = {}
    for line in block:
        instruction = re.fullmatch(r"(\w+) (r\d+\.[xyzw]+), (.*)", line)
        if not instruction or instruction[1] not in OPERATIONS:
            raise ValueError("Expected a straight-line scattering lookup")
        _, dest, sources = instruction.groups()
        for match in TEMP.finditer(sources):
            register = int(match[1])
            for lane in match[2] or "xyzw":
                if (register, lane) not in defined:
                    live.setdefault(register, set()).add(lane)
        match = TEMP.fullmatch(dest)
        defined.update((int(match[1]), lane) for lane in match[2])
    return live


def transform(assembly):
    lines = assembly.splitlines()
    code = [line.strip() for line in lines]
    if code.count(RADIUS_OFFSET) != 1:
        raise ValueError("Expected one finite-segment scattering lookup")
    start = code.index(RADIUS_OFFSET)
    if code[start + 2] == "add r0.y, r0.y, l(-2.000000)":
        radius, end_cos, sun_cos, ray_cos, result = "r0.z", "r1.w", "r0.x", "r1.y", "r3"
        source = "r9"
    elif code[start + 2] == "add r2.w, r4.y, l(-2.000000)":
        radius, end_cos, sun_cos, ray_cos, result = "r4.z", "r3.x", "r1.w", "r1.x", "r6"
        source = "r6"
    else:
        raise ValueError("Unknown finite-segment geometry layout")

    last = f"max {result}.xyzw, r2.xyzw, {ZERO}"
    endpoints = [i for i in range(start, len(code)) if code[i] == last]
    if len(endpoints) != 1:
        raise ValueError("Ambiguous finite-segment output")
    end = endpoints[0]
    block = code[start:end]
    if (
        code[end - 1] != f"mad r2.xyzw, -r2.xyzw, r5.yzwy, {source}.xyzw"
        or f"div {end_cos}, {end_cos}, {radius}" not in code[:start]
        or f"div r2.x, r2.x, {radius}" not in code[:start]
        or f"mul r2.y, {sun_cos}, l(5.79788351)" not in block
        or f"max r2.y, {ray_cos}, l(-1.000000)" not in block
        or not re.search(r"div r\d+\.[xyzw], l\(0\.500000\), cb0\[23\]\.y", "\n".join(block))
    ):
        raise ValueError("Unrecognized scattering lookup or geometry inputs")
    decl = [i for i, line in enumerate(code[:start]) if re.fullmatch(r"dcl_temps \d+", line)]
    if len(decl) != 1:
        raise ValueError("Unexpected temporary declarations")
    temporary_count = int(code[decl[0]].split()[1])
    if code[end + 1].startswith("if_nz "):
        raise ValueError("Unrecognized code immediately after the scattering lookup")

    first_copy = temporary_count
    seam, distance, negative = (f"r{i}" for i in range(2 * temporary_count, 2 * temporary_count + 3))
    live = live_inputs(block)
    snapshot = [
        f"mov r{first_copy + register}.{''.join(lane for lane in 'xyzw' if lane in lanes)}, r{register}.xyzw"
        for register, lanes in sorted(live.items())
    ]
    before = [
        f"add {seam}.x, r7.w, -cb0[20].x",
        f"max {seam}.x, {seam}.x, l(0.000000)",
        f"add {seam}.y, r7.w, cb0[20].x",
        f"mul {seam}.x, {seam}.x, {seam}.y",
        f"sqrt {seam}.x, {seam}.x",
        f"div {seam}.x, -{seam}.x, r7.w",
        f"div {seam}.y, l(0.500000), cb0[23].y",
        f"add {seam}.z, r7.z, -{seam}.x",
        f"lt {seam}.w, |{seam}.z|, {seam}.y",
        f"if_nz {seam}.w",
        f"mul {distance}.x, {radius}, {end_cos}",
        f"mad {distance}.x, -r7.w, r7.z, {distance}.x",
        f"div {seam}.z, {seam}.z, {seam}.y",
        f"mad_sat {seam}.z, {seam}.z, l(0.500000), l(0.500000)",
        *snapshot,
    ]

    def mapped(match):
        return f"r{first_copy + int(match[1])}" + (f".{match[2]}" if match[2] else "")

    def reg(name):
        return TEMP.sub(mapped, name)

    # Keep the captured lookup isolated from the original register file.
    lookup = [reg(line) for line in block]

    def anchor(sign):
        return [
            f"add {reg('r7.z')}, {seam}.x, {sign}{seam}.y",
            f"mul {distance}.y, r7.w, r7.w",
            f"mad {distance}.y, {distance}.x, {distance}.x, {distance}.y",
            f"add {distance}.w, r7.w, r7.w",
            f"mul {distance}.w, {distance}.w, {distance}.x",
            f"mad {distance}.y, {distance}.w, {reg('r7.z')}, {distance}.y",
            f"max {distance}.y, {distance}.y, l(0.000000)",
            f"sqrt {reg(radius)}, {distance}.y",
            f"mad {reg(end_cos)}, r7.w, {reg('r7.z')}, {distance}.x",
            f"div {reg(end_cos)}, {reg(end_cos)}, {reg(radius)}",
            f"mul {distance}.z, r7.w, {sun_cos}",
            f"mad {reg('r2.x')}, {distance}.x, {ray_cos}, {distance}.z",
            f"div {reg('r2.x')}, {reg('r2.x')}, {reg(radius)}",
        ]

    after = [
        *anchor("-"), *lookup,
        f"mov {negative}.xyzw, {reg('r2.xyzw')}",
        *snapshot, *anchor(""), *lookup,
        f"endif",
    ]
    # Preserve every side effect of the original lookup, then replace its result.
    finish = [
        f"if_nz {seam}.w",
        f"add r2.xyzw, {reg('r2.xyzw')}, -{negative}.xyzw",
        f"mad r2.xyzw, {seam}.zzzz, r2.xyzw, {negative}.xyzw",
        last,
        "endif",
    ]
    lines[end + 1:end + 1] = finish
    lines[start:start] = before + after
    lines[decl[0]] = f"dcl_temps {2 * temporary_count + 3}"
    return "\n".join(lines) + "\n"
