use crate::ir::Op;
use crate::{analysis, ir, program::Program};
use std::collections::{BTreeMap, HashSet};

pub struct Stats {
    pub before: usize,
    pub after: usize,
    pub replacements: usize,
    pub proofs: usize,
    pub evaluations: u64,
    pub removed: usize,
    pub temps_before: usize,
    pub temps_after: usize,
}
pub struct Optimized {
    pub bytes: Vec<u8>,
    pub stats: Stats,
}
#[derive(Clone, Copy)]
pub enum Repair {
    Select,
    Branch,
}

fn destination(register: usize, mask: u8) -> Vec<u32> {
    vec![0x00100002 | ((mask as u32) << 4), register as u32]
}
fn source(register: usize, swizzle: [usize; 4]) -> Vec<u32> {
    let bits = swizzle
        .iter()
        .enumerate()
        .fold(0u32, |bits, (lane, &component)| {
            bits | ((component as u32) << (4 + lane * 2))
        });
    vec![0x00100006 | bits, register as u32]
}
fn identity(register: usize) -> Vec<u32> {
    source(register, [0, 1, 2, 3])
}
fn immediate(value: [u32; 4]) -> Vec<u32> {
    let mut words = vec![0x00004002];
    words.extend(value);
    words
}
fn instruction(opcode: u16, args: &[Vec<u32>]) -> Result<Op, String> {
    let mut words = vec![opcode as u32];
    for operand in args {
        words.extend(operand);
    }
    words[0] |= (words.len() as u32) << 24;
    Op::decode(words)
}

fn conversion(
    register: usize,
    mask: u8,
    capture: usize,
    swizzle: [usize; 4],
    shifts: [u32; 4],
    masks: [u32; 4],
    work: [usize; 3],
    repair: Repair,
) -> Result<Vec<Op>, String> {
    let dst = destination(register, mask);
    let a = destination(work[0], mask);
    let b = destination(work[1], mask);
    let c = destination(work[2], mask);
    let sa = identity(work[0]);
    let sb = identity(work[1]);
    let sc = identity(work[2]);
    let mut code = vec![
        instruction(
            85,
            &[a.clone(), source(capture, swizzle), immediate(shifts)],
        )?,
        instruction(1, &[a.clone(), sa.clone(), immediate(masks)])?,
        instruction(131, &[dst.clone(), sa.clone()])?,
        instruction(1, &[b.clone(), sa.clone(), immediate([0x7fff; 4])])?,
        instruction(79, &[b.clone(), immediate([0x7c00; 4]), sb.clone()])?,
    ];
    if matches!(repair, Repair::Branch) {
        let mut lanes = (0..4).filter(|lane| mask & (1 << lane) != 0);
        let first = lanes.next().ok_or("empty conversion mask")?;
        code.push(instruction(
            54,
            &[destination(work[2], 1), source(work[1], [first; 4])],
        )?);
        for lane in lanes {
            code.push(instruction(
                60,
                &[
                    destination(work[2], 1),
                    source(work[2], [0; 4]),
                    source(work[1], [lane; 4]),
                ],
            )?);
        }
        let mut condition = instruction(31, &[source(work[2], [0; 4])])?;
        condition.words[0] |= 1 << 18;
        code.push(condition);
    }
    code.extend(vec![
        instruction(41, &[c.clone(), sa.clone(), immediate([16; 4])])?,
        instruction(1, &[c, sc.clone(), immediate([0x80000000; 4])])?,
        instruction(41, &[a.clone(), sa.clone(), immediate([13; 4])])?,
        instruction(1, &[a.clone(), sa.clone(), immediate([0x007fe000; 4])])?,
        instruction(60, &[a.clone(), sa.clone(), immediate([0x7f800000; 4])])?,
        instruction(60, &[a, sa.clone(), sc])?,
        instruction(55, &[dst, sb, sa, identity(register)])?,
    ]);
    if matches!(repair, Repair::Branch) {
        code.push(instruction(21, &[])?);
    }
    Ok(code)
}

fn compact(mut ops: Vec<Op>, temps: usize) -> Result<(Vec<Op>, usize), String> {
    let edges = ir::successors(&ops)?;
    let reads: Vec<_> = ops.iter().map(Op::reads).collect();
    let writes: Vec<_> = ops.iter().map(Op::writes).collect();
    let mut live = vec![vec![0u8; temps]; ops.len()];
    let mut changed = true;
    while changed {
        changed = false;
        for index in (0..ops.len()).rev() {
            let mut state = vec![0u8; temps];
            for &next in &edges[index] {
                for (dst, &src) in state.iter_mut().zip(&live[next]) {
                    *dst |= src;
                }
            }
            for &(register, mask) in &writes[index] {
                state[register] &= !mask;
            }
            for &(register, mask) in &reads[index] {
                state[register] |= mask;
            }
            if state != live[index] {
                live[index] = state;
                changed = true;
            }
        }
    }
    let mut graph = vec![vec![false; temps]; temps];
    let mut used = vec![false; temps];
    for index in 0..ops.len() {
        for &(register, _) in reads[index].iter().chain(&writes[index]) {
            used[register] = true;
        }
        let active: Vec<_> = live[index]
            .iter()
            .enumerate()
            .filter(|&(_, &mask)| mask != 0)
            .collect();
        for (i, &(a, &am)) in active.iter().enumerate() {
            for &(b, &bm) in &active[i + 1..] {
                if am & bm != 0 {
                    graph[a][b] = true;
                    graph[b][a] = true;
                }
            }
        }
        let mut output = vec![0u8; temps];
        for &next in &edges[index] {
            for (dst, &src) in output.iter_mut().zip(&live[next]) {
                *dst |= src;
            }
        }
        for &(a, am) in &writes[index] {
            for (b, &bm) in output.iter().enumerate() {
                if a != b && am & bm != 0 {
                    graph[a][b] = true;
                    graph[b][a] = true;
                }
            }
            for &(b, bm) in &writes[index] {
                if a != b && am & bm != 0 {
                    graph[a][b] = true;
                    graph[b][a] = true;
                }
            }
        }
    }
    let mut colors = vec![None; temps];
    let mut count = 0;
    while let Some(next) = (0..temps)
        .filter(|&node| used[node] && colors[node].is_none())
        .max_by_key(|&node| {
            let saturation: HashSet<_> = (0..temps)
                .filter(|&other| graph[node][other])
                .filter_map(|other| colors[other])
                .collect();
            (
                saturation.len(),
                graph[node].iter().filter(|&&edge| edge).count(),
                std::cmp::Reverse(node),
            )
        })
    {
        let occupied: HashSet<_> = (0..temps)
            .filter(|&other| graph[next][other])
            .filter_map(|other| colors[other])
            .collect();
        let color = (0..).find(|color| !occupied.contains(color)).unwrap();
        colors[next] = Some(color);
        count = count.max(color + 1);
    }
    for op in &mut ops {
        if op.opcode == 104 {
            op.words[1] = count as u32;
            continue;
        }
        let mut words = op.words.clone();
        for operand in &op.operands {
            for &(position, register) in &operand.positions {
                words[position] = colors[register].ok_or("uncolored temporary")? as u32;
            }
        }
        *op = Op::decode(words)?;
    }
    Ok((ops, count))
}

pub fn optimize(program: &Program, repair: Repair) -> Result<Optimized, String> {
    optimize_passes(program, repair, true)
}

pub fn optimize_passes(
    program: &Program,
    repair: Repair,
    compact_registers: bool,
) -> Result<Optimized, String> {
    if program.stage != 0 || !(4..=5).contains(&program.major) {
        return Err("only DX11 pixel programs are supported".into());
    }
    let original = ir::decode(program)?;
    let temps = original
        .iter()
        .find(|op| op.opcode == 104)
        .ok_or("temporary declaration missing")?
        .words[1] as usize;
    let analysis = analysis::analyze(&original, temps);
    let before = original.len();
    let mut stats = Stats {
        before,
        after: before,
        replacements: analysis.matches.len(),
        proofs: analysis.proved,
        evaluations: analysis.evaluated,
        removed: 0,
        temps_before: temps,
        temps_after: temps,
    };
    if analysis.matches.is_empty() {
        return Ok(Optimized {
            bytes: program.original.to_vec(),
            stats,
        });
    }
    let mut captures: BTreeMap<analysis::Origin, (usize, u8)> = BTreeMap::new();
    let mut next = temps + 3;
    for found in &analysis.matches {
        let entry = captures.entry(found.leaf.origin).or_insert_with(|| {
            let register = next;
            next += 1;
            (register, 0)
        });
        entry.1 |= 1 << found.leaf.component;
    }
    let mut before_capture: BTreeMap<usize, Vec<Op>> = BTreeMap::new();
    let mut after_capture: BTreeMap<usize, Vec<Op>> = BTreeMap::new();
    for (origin, &(register, mask)) in &captures {
        let instruction = instruction(
            54,
            &[destination(register, mask), identity(origin.register)],
        )?;
        let target = if origin.before {
            &mut before_capture
        } else {
            &mut after_capture
        };
        target.entry(origin.at).or_default().push(instruction);
    }
    let mut rewritten = Vec::new();
    for (index, op) in original.into_iter().enumerate() {
        if let Some(capture) = before_capture.remove(&index) {
            rewritten.extend(capture);
        }
        let found: Vec<_> = analysis
            .matches
            .iter()
            .filter(|found| found.instruction == index)
            .collect();
        if found.is_empty() {
            rewritten.push(op);
        } else {
            let destination_register = op.operands[0]
                .index
                .ok_or("non-temporary match destination")?;
            let mask = found
                .iter()
                .fold(0u8, |mask, found| mask | (1 << found.component));
            let remaining = op.operands[0].mask & !mask;
            if remaining != 0 {
                let mut words = op.words.clone();
                let position = op.operands[0].start;
                words[position] = (words[position] & !0xf0) | ((remaining as u32) << 4);
                rewritten.push(Op::decode(words)?);
            }
            let mut groups: BTreeMap<usize, Vec<_>> = BTreeMap::new();
            for found in found {
                groups
                    .entry(captures[&found.leaf.origin].0)
                    .or_default()
                    .push(found);
            }
            for (capture, group) in groups {
                let mut mask = 0;
                let mut swizzle = [0; 4];
                let mut shifts = [0; 4];
                let mut masks = [65535; 4];
                for found in group {
                    let lane = found.component;
                    mask |= 1 << lane;
                    swizzle[lane] = found.leaf.component;
                    shifts[lane] = found.shift;
                    masks[lane] = if found.unsigned { 32767 } else { 65535 };
                }
                rewritten.extend(conversion(
                    destination_register,
                    mask,
                    capture,
                    swizzle,
                    shifts,
                    masks,
                    [temps, temps + 1, temps + 2],
                    repair,
                )?);
            }
        }
        if let Some(capture) = after_capture.remove(&index) {
            rewritten.extend(capture);
        }
    }
    if !before_capture.is_empty() || !after_capture.is_empty() {
        return Err("capture point outside program".into());
    }
    for op in &mut rewritten {
        if op.opcode == 104 {
            op.words[1] = next as u32;
        }
    }
    let (mut rewritten, mut removed) = ir::remove_dead(rewritten, next)?;
    loop {
        let (shortened, branches) = ir::remove_empty_branches(rewritten)?;
        rewritten = shortened;
        if branches == 0 {
            break;
        }
        let (shortened, dead) = ir::remove_dead(rewritten, next)?;
        rewritten = shortened;
        removed += branches + dead;
    }
    let (rewritten, count) = if compact_registers {
        compact(rewritten, next)?
    } else {
        (rewritten, next)
    };
    stats.after = rewritten.len();
    stats.removed = removed;
    stats.temps_after = count;
    let mut code = vec![(program.stage as u32) << 16 | 5 << 4, 0];
    for op in rewritten {
        code.extend(op.words);
    }
    code[1] = code.len() as u32;
    let code: Vec<u8> = code.into_iter().flat_map(u32::to_le_bytes).collect();
    let mut chunks = Vec::new();
    for chunk in &program.chunks {
        if &chunk.kind == b"SHDR" || &chunk.kind == b"SHEX" {
            let mut data = b"SHEX".to_vec();
            data.extend((code.len() as u32).to_le_bytes());
            data.extend_from_slice(&code);
            chunks.push(data);
        } else if !matches!(&chunk.kind, b"STAT" | b"SDBG" | b"SPDB") {
            chunks.push(program.original[chunk.bytes.clone()].to_vec());
        }
    }
    let header = 32 + chunks.len() * 4;
    let mut output = vec![0u8; header];
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
    Program::parse(&output).map_err(|error| format!("rewritten container invalid: {error:?}"))?;
    Ok(Optimized {
        bytes: output,
        stats,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn evaluate(ops: &[Op], inputs: &[[u32; 4]]) -> [u32; 4] {
        let mut registers = vec![[0u32; 4]; 64];
        let mut output = [0; 4];
        let mut pc = 0;
        let mut steps = 0;
        while pc < ops.len() {
            steps += 1;
            assert!(steps < 10000, "test program did not terminate");
            let op = &ops[pc];
            let read = |operand: &ir::Operand| -> [u32; 4] {
                let values = match operand.kind {
                    0 => registers[operand.index.unwrap()],
                    1 => inputs[operand.index.unwrap()],
                    4 => operand.immediate.unwrap(),
                    _ => panic!("unexpected test source"),
                };
                std::array::from_fn(|lane| values[operand.swizzle[lane]])
            };
            if op.opcode == 104 || op.opcode == 21 || op.opcode == 48 {
                pc += 1;
                continue;
            }
            if op.opcode == 62 {
                break;
            }
            if op.opcode == 31 {
                let nonzero = read(&op.operands[0])[0] != 0;
                let take = if op.words[0] & (1 << 18) != 0 {
                    nonzero
                } else {
                    !nonzero
                };
                if !take {
                    let mut depth = 0;
                    loop {
                        pc += 1;
                        match ops[pc].opcode {
                            31 => depth += 1,
                            21 if depth > 0 => depth -= 1,
                            18 | 21 if depth == 0 => break,
                            _ => {}
                        }
                    }
                }
                pc += 1;
                continue;
            }
            if op.opcode == 18 {
                let mut depth = 0;
                loop {
                    pc += 1;
                    match ops[pc].opcode {
                        31 => depth += 1,
                        21 if depth > 0 => depth -= 1,
                        21 => break,
                        _ => {}
                    }
                }
                pc += 1;
                continue;
            }
            if op.opcode == 7 || op.opcode == 22 {
                pc = ops[..pc].iter().rposition(|op| op.opcode == 48).unwrap() + 1;
                continue;
            }
            if op.opcode == 3 && read(&op.operands[0])[0] != 0 {
                pc = ops[pc..].iter().position(|op| op.opcode == 22).unwrap() + pc + 1;
                continue;
            }
            if op.opcode == 3 {
                pc += 1;
                continue;
            }
            let args: Vec<_> = op.operands[op.destinations..].iter().map(read).collect();
            let result: [u32; 4] = std::array::from_fn(|lane| {
                let a = args[0][lane];
                let b = args.get(1).map_or(0, |x| x[lane]);
                match op.opcode {
                    1 => a & b,
                    30 => a.wrapping_add(b),
                    32 => {
                        if a == b {
                            u32::MAX
                        } else {
                            0
                        }
                    }
                    41 => a.wrapping_shl(b & 31),
                    54 => a,
                    55 => {
                        if a != 0 {
                            b
                        } else {
                            args[2][lane]
                        }
                    }
                    60 => a | b,
                    79 => {
                        if a < b {
                            u32::MAX
                        } else {
                            0
                        }
                    }
                    85 => a.wrapping_shr(b & 31),
                    87 => a ^ b,
                    _ => panic!("unexpected test opcode {}", op.opcode),
                }
            });
            let dst = &op.operands[0];
            let target = if dst.kind == 2 {
                &mut output
            } else {
                &mut registers[dst.index.unwrap()]
            };
            for lane in 0..4 {
                if dst.mask & (1 << lane) != 0 {
                    target[lane] = result[lane];
                }
            }
            pc += 1;
        }
        output
    }

    fn input(index: usize) -> Vec<u32> {
        let mut operand = identity(index);
        operand[0] |= 1 << 12;
        operand
    }
    fn output() -> Vec<u32> {
        let mut operand = destination(0, 15);
        operand[0] |= 2 << 12;
        operand
    }
    fn seed_registers(temps: usize) -> Vec<Op> {
        let mut ops = vec![Op::decode(vec![(2 << 24) | 104, temps as u32]).unwrap()];
        for index in 0..temps {
            ops.push(instruction(54, &[destination(index, 15), input(index)]).unwrap());
        }
        ops
    }
    fn transformed(ops: Vec<Op>, temps: usize) -> Vec<Op> {
        let (ops, _) = ir::remove_dead(ops, temps).unwrap();
        compact(ops, temps).unwrap().0
    }
    #[test]
    fn register_coloring_and_dead_code_preserve_random_masked_programs() {
        let mut state = 0x38db7e1599u64;
        let mut random = || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state as u32
        };
        for _ in 0..1024 {
            let inputs: Vec<[u32; 4]> =
                (0..12).map(|_| std::array::from_fn(|_| random())).collect();
            let mut ops = seed_registers(12);
            for _ in 0..48 {
                let opcode = [1, 30, 41, 60, 85, 87][random() as usize % 6];
                let dst = random() as usize % 12;
                let a = random() as usize % 12;
                let b = random() as usize % 12;
                let mask = 1 + (random() % 15) as u8;
                let swizzle = std::array::from_fn(|_| random() as usize % 4);
                ops.push(
                    instruction(
                        opcode,
                        &[destination(dst, mask), source(a, swizzle), identity(b)],
                    )
                    .unwrap(),
                );
            }
            ops.push(instruction(54, &[output(), identity(11)]).unwrap());
            assert_eq!(
                evaluate(&ops, &inputs),
                evaluate(&transformed(ops.clone(), 12), &inputs)
            );
        }
    }
    #[test]
    fn loop_carried_values_and_both_branches_survive_compaction() {
        let mut ops = seed_registers(8);
        ops.push(instruction(54, &[destination(7, 1), immediate([3; 4])]).unwrap());
        ops.push(instruction(48, &[]).unwrap());
        let mut condition = instruction(31, &[source(0, [0; 4])]).unwrap();
        condition.words[0] |= 1 << 18;
        ops.push(condition);
        ops.push(instruction(30, &[destination(5, 5), identity(5), identity(1)]).unwrap());
        ops.push(instruction(18, &[]).unwrap());
        ops.push(instruction(87, &[destination(5, 10), identity(5), identity(2)]).unwrap());
        ops.push(instruction(21, &[]).unwrap());
        ops.push(
            instruction(
                30,
                &[
                    destination(7, 1),
                    source(7, [0; 4]),
                    immediate([u32::MAX; 4]),
                ],
            )
            .unwrap(),
        );
        ops.push(
            instruction(
                32,
                &[destination(6, 1), source(7, [0; 4]), immediate([0; 4])],
            )
            .unwrap(),
        );
        let mut stop = instruction(3, &[source(6, [0; 4])]).unwrap();
        stop.words[0] |= 1 << 18;
        ops.push(stop);
        ops.push(instruction(22, &[]).unwrap());
        ops.push(instruction(54, &[output(), identity(5)]).unwrap());
        let optimized = transformed(ops.clone(), 8);
        for branch in 0..2 {
            let mut inputs = vec![[0x351324af; 4]; 8];
            inputs[0][0] = branch;
            assert_eq!(evaluate(&ops, &inputs), evaluate(&optimized, &inputs));
        }
    }
    #[test]
    fn dead_branch_removal_releases_conditions_but_keeps_output_writes() {
        let mut ops = seed_registers(4);
        let mut test = instruction(31, &[source(0, [0; 4])]).unwrap();
        test.words[0] |= 1 << 18;
        ops.extend([
            test.clone(),
            test.clone(),
            instruction(54, &[destination(3, 15), identity(1)]).unwrap(),
            instruction(18, &[]).unwrap(),
            instruction(54, &[destination(3, 15), identity(2)]).unwrap(),
            instruction(21, &[]).unwrap(),
            instruction(21, &[]).unwrap(),
            test,
            instruction(54, &[output(), identity(1)]).unwrap(),
            instruction(18, &[]).unwrap(),
            instruction(54, &[output(), identity(2)]).unwrap(),
            instruction(21, &[]).unwrap(),
        ]);
        let (dead, _) = ir::remove_dead(ops.clone(), 4).unwrap();
        let (shortened, removed) = ir::remove_empty_branches(dead).unwrap();
        assert_eq!(removed, 5);
        assert_eq!(shortened.iter().filter(|op| op.opcode == 31).count(), 1);
        let optimized = transformed(shortened, 4);
        for condition in 0..2 {
            let mut inputs = vec![[0x12131415; 4]; 4];
            inputs[0][0] = condition;
            inputs[2] = [0xfedcba98; 4];
            assert_eq!(evaluate(&ops, &inputs), evaluate(&optimized, &inputs));
        }
    }
}
