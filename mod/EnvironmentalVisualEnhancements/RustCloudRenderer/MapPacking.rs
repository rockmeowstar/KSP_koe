use crate::{ir, program};
use ir::Op;

fn encoded(op: &Op, operand: usize) -> &[u32] {
    let value = &op.operands[operand];
    &op.words[value.start..value.end]
}

fn restore(destination: &[u32], packed: u32, lane: u32) -> Result<Vec<Op>, String> {
    if destination.len() != 2 || destination[0] & 0xfff != 0xf2 {
        return Err("sample output must be a full temporary".into());
    }
    let register = destination[1];
    // AI-generated comment: BC5 supplies (red, green, 0, 1); each BC4 result is (channel, 0, 0, 1).
    Ok(vec![Op::decode(vec![
        (5 << 24) | 54,
        0x001000f2,
        register,
        0x00100e86 | (lane << 4),
        packed,
    ])?])
}

fn rewrite(ops: &[Op]) -> Result<(Vec<Op>, usize), String> {
    ir::successors(ops)?;
    let temporary = ops
        .iter()
        .find(|op| op.opcode == 104)
        .ok_or("no temporaries")?
        .words[1];
    if temporary >= 4095 {
        return Err("temporary budget".into());
    }
    let mut scopes = Vec::with_capacity(ops.len());
    let mut scope = Vec::new();
    for (index, op) in ops.iter().enumerate() {
        if op
            .operands
            .iter()
            .any(|v| v.kind == 3 || v.kind == 7 && (v.index.is_none() || !v.relative.is_empty()))
        {
            return Err("indirect resource or temporary access".into());
        }
        scopes.push(scope.clone());
        match op.opcode {
            31 => scope.push((index, false)),
            18 => scope.last_mut().ok_or("else without if")?.1 = true,
            21 => {
                scope.pop().ok_or("endif without if")?;
            }
            _ => {}
        }
    }
    let mut output = Vec::new();
    let mut saved: Option<usize> = None;
    let mut removed = 0;
    for (index, op) in ops.iter().enumerate() {
        if let Some(source) = op
            .operands
            .iter()
            .find(|v| v.kind == 7 && v.index == Some(0))
        {
            if op.opcode != 72
                || source.swizzle != [0, 1, 2, 3]
                || source.modifier != 0
                || op.operands.len() != 5
            {
                return Err("unsupported source-zero access".into());
            }
            let destination = &op.operands[0];
            if destination.kind != 0
                || destination.mask != 15
                || destination.end - destination.start != 2
            {
                return Err("sample destination".into());
            }
            let mut words = op.words.clone();
            words[destination.start + 1] = temporary;
            output.push(Op::decode(words)?);
            output.extend(restore(encoded(op, 0), temporary, 0)?);
            saved = Some(index);
            continue;
        }
        if op.opcode == 72
            && op
                .operands
                .get(2)
                .is_some_and(|v| v.kind == 7 && v.index == Some(1))
        {
            if let Some(at) = saved {
                let first = &ops[at];
                let a = &first.operands;
                let b = &op.operands;
                let reads = op.reads();
                // AI-generated comment: Reuse requires a dominating fetch in the same loop iteration.
                // Neither coordinates nor LOD may be overwritten in any intervening branch.
                let stable = ops[at..index].iter().all(|between| {
                    !between
                        .operands
                        .first()
                        .is_some_and(|v| v.kind == 0 && !v.relative.is_empty())
                        && between.writes().iter().all(|&(r, mask)| {
                            !reads.iter().any(|&(s, used)| r == s && mask & used != 0)
                        })
                });
                let straight = ops[at + 1..index]
                    .iter()
                    .all(|v| !matches!(v.opcode, 2 | 3 | 7 | 8 | 13 | 22 | 48 | 62 | 63));
                let plain = [&a[2], &a[3], &b[2], &b[3]]
                    .iter()
                    .all(|v| v.modifier == 0 && v.relative.is_empty());
                if scopes[index].starts_with(&scopes[at])
                    && stable
                    && straight
                    && plain
                    && b[0].kind == 0
                    && b[0].mask == 15
                    && first.words[0] == op.words[0]
                    && first.words[1..a[0].start] == op.words[1..b[0].start]
                    && encoded(first, 1) == encoded(op, 1)
                    && encoded(first, 4) == encoded(op, 4)
                    && a[3].kind == 6
                    && b[3].kind == 6
                    && a[3].index == Some(0)
                    && b[3].index == Some(1)
                    && a[2].swizzle == b[2].swizzle
                {
                    output.extend(restore(encoded(op, 0), temporary, 1)?);
                    removed += 1;
                    continue;
                }
            }
        }
        output.push(op.clone());
    }
    if removed == 0 {
        return Err("no paired samples".into());
    }
    output.iter_mut().find(|op| op.opcode == 104).unwrap().words[1] += 1;
    ir::successors(&output)?;
    Ok((output, removed))
}

pub fn optimize(bytes: &[u8]) -> Result<(Vec<u8>, usize), String> {
    let source = program::Program::parse(bytes).map_err(|e| format!("{e:?}"))?;
    if source.stage != 0 || !(4..=5).contains(&source.major) {
        return Err("not a supported pixel shader".into());
    }
    let (ops, count) = rewrite(&ir::decode(&source)?)?;
    let mut words = vec![
        u32::from_le_bytes(
            bytes[source.code.start..source.code.start + 4]
                .try_into()
                .unwrap(),
        ),
        0,
    ];
    for op in ops {
        words.extend(op.words);
    }
    words[1] = words.len() as u32;
    let code: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    let mut chunks = Vec::new();
    for chunk in &source.chunks {
        if matches!(&chunk.kind, b"SHDR" | b"SHEX") {
            let mut data = chunk.kind.to_vec();
            data.extend((code.len() as u32).to_le_bytes());
            data.extend(&code);
            chunks.push(data);
        } else if !matches!(&chunk.kind, b"STAT" | b"SDBG" | b"SPDB") {
            chunks.push(bytes[chunk.bytes.clone()].to_vec());
        }
    }
    let mut output = vec![0u8; 32 + 4 * chunks.len()];
    output[..4].copy_from_slice(b"DXBC");
    output[20..24].copy_from_slice(&1u32.to_le_bytes());
    output[28..32].copy_from_slice(&(chunks.len() as u32).to_le_bytes());
    for (index, chunk) in chunks.into_iter().enumerate() {
        let offset = output.len() as u32;
        output[32 + index * 4..36 + index * 4].copy_from_slice(&offset.to_le_bytes());
        output.extend(chunk);
        while output.len() % 4 != 0 {
            output.push(0);
        }
    }
    let size = output.len() as u32;
    output[24..28].copy_from_slice(&size.to_le_bytes());
    program::Program::parse(&output).map_err(|e| format!("{e:?}"))?;
    Ok((output, count))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn sample(destination: u32, coordinates: u32, texture: u32, sampler: u32) -> Op {
        Op::decode(vec![
            (11 << 24) | 72,
            0x001000f2,
            destination,
            0x00100046,
            coordinates,
            0x00107e46,
            texture,
            0x00106000,
            sampler,
            0x00004001,
            0,
        ])
        .unwrap()
    }
    fn fragment(a: Op, b: Op) -> Vec<Op> {
        vec![
            Op::decode(vec![(2 << 24) | 104, 40]).unwrap(),
            a,
            b,
            Op::decode(vec![(1 << 24) | 62]).unwrap(),
        ]
    }
    fn branch() -> Op {
        Op::decode(vec![(3 << 24) | 31, 0x00004001, 1]).unwrap()
    }
    #[test]
    fn one_fetch_restores_two_one_channel_results() {
        let (ops, n) = rewrite(&fragment(sample(31, 30, 0, 0), sample(30, 30, 1, 1))).unwrap();
        assert_eq!(n, 1);
        assert_eq!(ops.iter().filter(|v| v.opcode == 72).count(), 1);
        let moves: Vec<_> = ops.iter().filter(|v| v.opcode == 54).collect();
        assert_eq!(moves.len(), 2);
        assert_eq!(moves[0].operands[1].swizzle, [0, 2, 2, 3]);
        assert_eq!(moves[1].operands[1].swizzle, [1, 2, 2, 3]);
        assert!(moves.iter().all(|op| op.operands[0].mask == 15));
    }
    #[test]
    fn changed_coordinates_and_samplers_are_rejected() {
        assert!(rewrite(&fragment(sample(30, 30, 0, 0), sample(31, 30, 1, 1))).is_err());
        assert!(rewrite(&fragment(sample(31, 30, 0, 0), sample(30, 29, 1, 1))).is_err());
        assert!(rewrite(&fragment(sample(31, 30, 0, 0), sample(30, 30, 1, 2))).is_err());
    }
    #[test]
    fn dominating_fetch_survives_a_density_branch() {
        let mut ops = fragment(sample(31, 30, 0, 0), sample(30, 30, 1, 1));
        ops.insert(2, branch());
        ops.insert(3, Op::decode(vec![(1 << 24) | 18]).unwrap());
        ops.insert(5, Op::decode(vec![(1 << 24) | 21]).unwrap());
        assert_eq!(rewrite(&ops).unwrap().1, 1);
    }
    #[test]
    fn cross_arm_and_overwritten_coordinates_are_rejected() {
        let mut ops = fragment(sample(31, 30, 0, 0), sample(30, 30, 1, 1));
        ops.insert(1, branch());
        ops.insert(3, Op::decode(vec![(1 << 24) | 18]).unwrap());
        ops.insert(5, Op::decode(vec![(1 << 24) | 21]).unwrap());
        assert!(rewrite(&ops).is_err());
        let mut ops = fragment(sample(31, 30, 0, 0), sample(30, 30, 1, 1));
        ops.insert(
            2,
            Op::decode(vec![(5 << 24) | 54, 0x00100012, 30, 0x00004001, 0]).unwrap(),
        );
        assert!(rewrite(&ops).is_err());
    }
}
