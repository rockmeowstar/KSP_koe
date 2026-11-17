# SPDX-License-Identifier: MIT
"""Build a hash-checked shader recipe from an externally supplied original DXBC."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import struct
import subprocess
import tempfile


def recipe(source, target):
    index = {}
    for offset in range(max(0, len(source)-15)):
        offsets = index.setdefault(source[offset:offset+16], [])
        if len(offsets) < 32:
            offsets.append(offset)
    operations = []
    literal = bytearray()
    position = 0
    while position < len(target):
        best_offset = best_length = 0
        for offset in index.get(target[position:position+16], []):
            length = 16
            limit = min(len(source)-offset, len(target)-position)
            while length+16 <= limit and source[offset+length:offset+length+16] == target[position+length:position+length+16]:
                length += 16
            while length < limit and source[offset+length] == target[position+length]:
                length += 1
            if length > best_length:
                best_offset, best_length = offset, length
        if best_length:
            if literal:
                operations.append(b'\1'+struct.pack('<I', len(literal))+literal)
                literal = bytearray()
            operations.append(b'\0'+struct.pack('<II', best_offset, best_length))
            position += best_length
        else:
            literal.append(target[position])
            position += 1
    if literal:
        operations.append(b'\1'+struct.pack('<I', len(literal))+literal)
    return (b'KOEPATCH'+struct.pack('<IIII', 1, len(source), len(target), len(operations))
            + hashlib.sha256(source).digest()+hashlib.sha256(target).digest()+b''.join(operations))


def build(folder):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--assembler', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=folder/'native-assets')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('shader_transform', folder/'ShaderTransform.py')
    transform = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transform)
    source = args.source.read_bytes()
    if hashlib.sha256(source).hexdigest() != transform.SOURCE_SHA256:
        raise ValueError('Unsupported original shader; no patch generated')
    with tempfile.TemporaryDirectory(prefix='koe-shader-') as temporary:
        work = Path(temporary)
        original = work/'original.dxbc'
        original.write_bytes(source)
        subprocess.run([str(args.assembler.resolve()), '-d', str(original)], check=True, capture_output=True)
        assembly = original.with_suffix('.asm').read_text()
        candidate = work/'candidate.asm'
        candidate.write_text(transform.transform(assembly))
        subprocess.run([str(args.assembler.resolve()), '-a', '--copy-reflection', str(original), str(candidate)], check=True, capture_output=True)
        binaries = list(work.glob('candidate*.shdr'))
        if len(binaries) != 1:
            raise ValueError('Ambiguous assembler output')
        target = binaries[0].read_bytes()
    if hashlib.sha256(target).hexdigest() != transform.EXPECTED_SHA256:
        raise ValueError('Build differs from the GPU-validated shader')
    patch = recipe(source, target)
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.output/(folder.name+'.koepatch')
    path.write_bytes(patch)
    metadata = dict(shader=transform.SHADER_NAME, sourceSha256=transform.SOURCE_SHA256,
                    targetSha256=transform.EXPECTED_SHA256, recipeSha256=hashlib.sha256(patch).hexdigest(),
                    sourceBytes=len(source), targetBytes=len(target), recipeBytes=len(patch),
                    format='KOEPATCH version 1', releaseIntegration='hash-checked runtime replacement')
    (args.output/'ShaderPatch.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(folder.name, 'validated shader reproduced;', len(patch), 'recipe bytes')
