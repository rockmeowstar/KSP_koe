"""Assemble local candidates selected by shader name and instruction structure."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile


def build(extracted, output, assembler, transform):
    programs = json.loads((extracted / "programs.json").read_text())
    matches = []
    for key, program in programs.items():
        if program["shader"] != transform.SHADER_NAME:
            continue
        original = (extracted / (key + ".dxbc")).resolve()
        with tempfile.TemporaryDirectory() as directory:
            candidate = Path(directory) / "candidate.asm"
            local_original = Path(directory) / "original.dxbc"
            local_original.write_bytes(original.read_bytes())
            subprocess.run([assembler, "-d", str(local_original)], check=True, capture_output=True)
            assembly = local_original.with_suffix(".asm").read_text()
            try:
                patched = transform.transform(assembly)
            except ValueError as error:
                print(f"skip {key[:12]} {error}")
                continue
            candidate.write_text(patched)
            subprocess.run(
                [assembler, "-a", "--copy-reflection", str(local_original), str(candidate)],
                check=True,
                capture_output=True,
            )
            result, = candidate.parent.glob("candidate*.shdr")
            subprocess.run([assembler, "-d", str(result)], check=True, capture_output=True)
            target = output / (key + ".dxbc")
            target.write_bytes(result.read_bytes())
            matches.append(dict(shader=program["shader"], source=str(original), candidate=str(target)))
    return matches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extracted", type=Path)
    parser.add_argument("transform", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--assembler", required=True, type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("shader_transform", args.transform)
    transform = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transform)
    args.output.mkdir(parents=True, exist_ok=True)
    matches = build(args.extracted, args.output, str(args.assembler.resolve()), transform)
    (args.output / "matches.json").write_text(json.dumps(matches, indent=2) + "\n")
    print(f"Assembled {len(matches)} structurally matched programs")
    if not matches:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
