use crate::program::Program;

#[derive(Clone, Debug)]
pub struct Operand {
    pub start: usize,
    pub end: usize,
    pub kind: u32,
    pub mask: u8,
    pub swizzle: [usize; 4],
    pub index: Option<usize>,
    pub immediate: Option<[u32; 4]>,
    pub modifier: u32,
    pub relative: Vec<(usize, u8)>,
    pub positions: Vec<(usize, usize)>,
}
#[derive(Clone)]
pub struct Op {
    pub words: Vec<u32>,
    pub opcode: u16,
    pub operands: Vec<Operand>,
    pub destinations: usize,
    pub pure: bool,
}

fn operand(words: &[u32], position: &mut usize) -> Result<Operand, String> {
    let start = *position;
    let token = *words.get(*position).ok_or("operand token missing")?;
    *position += 1;
    let mut extension = token;
    let mut modifier = 0;
    while extension >> 31 != 0 {
        extension = *words.get(*position).ok_or("operand extension missing")?;
        *position += 1;
        if extension & 63 == 1 {
            modifier = (extension >> 6) & 255;
        } else {
            return Err("unknown operand extension".into());
        }
    }
    let components = token & 3;
    let selection = (token >> 2) & 3;
    let mask = if components == 1 {
        1
    } else if components == 2 && selection == 0 {
        ((token >> 4) & 15) as u8
    } else {
        15
    };
    let swizzle = if components == 1 {
        [0; 4]
    } else if selection == 1 {
        std::array::from_fn(|i| ((token >> (4 + i * 2)) & 3) as usize)
    } else if selection == 2 {
        [((token >> 4) & 3) as usize; 4]
    } else {
        [0, 1, 2, 3]
    };
    let kind = (token >> 12) & 255;
    let dimensions = ((token >> 20) & 3) as usize;
    let mut indices = Vec::new();
    let mut relative = Vec::new();
    let mut positions = Vec::new();
    for dimension in 0..dimensions {
        let representation = (token >> (22 + dimension * 3)) & 7;
        let index = match representation {
            0 | 3 => {
                let x = *words.get(*position).ok_or("index missing")?;
                if kind == 0 && dimensions == 1 && representation == 0 {
                    positions.push((*position, x as usize));
                }
                *position += 1;
                Some(x as usize)
            }
            1 | 4 => {
                *position = position.checked_add(2).ok_or("index overflow")?;
                None
            }
            2 => None,
            _ => return Err("unknown index representation".into()),
        };
        if representation >= 2 {
            let nested = operand(words, position)?;
            if nested.kind == 0 {
                if let Some(index) = nested.index {
                    relative.push((index, nested.source_mask(1)));
                }
            }
            relative.extend(nested.relative);
            positions.extend(nested.positions);
        }
        indices.push(index);
    }
    let immediate = if kind == 4 {
        let count = if components == 1 {
            1
        } else if components == 2 {
            4
        } else {
            return Err("immediate width unsupported".into());
        };
        let data = words
            .get(*position..*position + count)
            .ok_or("immediate truncated")?;
        *position += count;
        Some(if count == 1 {
            [data[0]; 4]
        } else {
            data.try_into().unwrap()
        })
    } else {
        None
    };
    if kind == 5 {
        return Err("64-bit immediate unsupported".into());
    }
    if *position > words.len() {
        return Err("operand exceeds instruction".into());
    }
    Ok(Operand {
        start,
        end: *position,
        kind,
        mask,
        swizzle,
        index: if dimensions == 1 { indices[0] } else { None },
        immediate,
        modifier,
        relative,
        positions,
    })
}
impl Operand {
    pub fn source_mask(&self, active: u8) -> u8 {
        (0..4)
            .filter(|&lane| active & (1 << lane) != 0)
            .fold(0, |mask, lane| mask | (1 << self.swizzle[lane]))
    }
}

impl Op {
    pub fn decode(words: Vec<u32>) -> Result<Self, String> {
        let opcode = (words[0] & 0x7ff) as u16;
        if opcode == 53 || (88..=106).contains(&opcode) || opcode == 155 {
            return Ok(Self {
                words,
                opcode,
                operands: Vec::new(),
                destinations: 0,
                pure: false,
            });
        }
        let (destinations, sources, pure) = match opcode {
            0 | 1 | 14 | 15 | 16 | 17 | 24 | 29 | 30 | 32 | 33 | 34 | 36 | 37 | 39 | 41 | 42
            | 49 | 51 | 52 | 56 | 57 | 60 | 61 | 79 | 80 | 83 | 84 | 85 | 87 => (1, 2, true),
            11 | 12 | 25 | 26 | 27 | 28 | 40 | 43 | 47 | 54 | 59 | 64 | 65 | 66 | 67 | 68 | 75
            | 86 | 131 => (1, 1, true),
            35 | 50 | 55 | 82 => (1, 3, true),
            38 | 78 | 81 => (2, 2, true),
            77 => (2, 1, true),
            45 => (1, 2, false),
            46 => (1, 3, false),
            69 => (1, 3, false),
            70 | 71 | 72 | 74 => (1, 4, false),
            73 => (1, 5, false),
            3 | 8 | 13 | 31 | 63 => (0, 1, false),
            2 | 7 | 18 | 21 | 22 | 48 | 58 | 62 => (0, 0, false),
            _ => return Err(format!("unsupported opcode {opcode}")),
        };
        let mut position = 1;
        let mut ext = words[0];
        while ext >> 31 != 0 {
            ext = *words.get(position).ok_or("instruction extension missing")?;
            position += 1;
        }
        let mut operands = Vec::new();
        for _ in 0..destinations + sources {
            operands.push(operand(&words, &mut position)?);
        }
        if position != words.len() {
            return Err(format!(
                "opcode {opcode}: {} trailing words",
                words.len() - position
            ));
        }
        Ok(Self {
            words,
            opcode,
            operands,
            destinations,
            pure,
        })
    }
    pub fn writes(&self) -> Vec<(usize, u8)> {
        self.operands[..self.destinations]
            .iter()
            .filter(|operand| operand.kind == 0)
            .filter_map(|operand| operand.index.map(|index| (index, operand.mask)))
            .collect()
    }
    pub fn reads(&self) -> Vec<(usize, u8)> {
        let active = self.operands[..self.destinations]
            .iter()
            .fold(0, |mask, operand| mask | operand.mask);
        let active = match self.opcode {
            15 => 3,
            16 => 7,
            17 => 15,
            3 | 8 | 13 | 31 | 63 => 1,
            45 | 46 | 69..=74 | 61 => 15,
            _ => active,
        };
        let mut result = Vec::new();
        for operand in self.operands.iter().skip(self.destinations) {
            if operand.kind == 0 {
                if let Some(index) = operand.index {
                    result.push((index, operand.source_mask(active)));
                }
            }
            result.extend_from_slice(&operand.relative);
        }
        for operand in &self.operands[..self.destinations] {
            result.extend_from_slice(&operand.relative);
        }
        result
    }
}

pub fn decode(program: &Program) -> Result<Vec<Op>, String> {
    program
        .instructions
        .iter()
        .map(|instruction| {
            let words = program
                .instruction_bytes(instruction)
                .chunks_exact(4)
                .map(|x| u32::from_le_bytes(x.try_into().unwrap()))
                .collect();
            let op = Op::decode(words)?;
            if op.opcode != instruction.opcode {
                return Err("instruction identity changed during decoding".into());
            }
            Ok(op)
        })
        .collect()
}

pub fn successors(ops: &[Op]) -> Result<Vec<Vec<usize>>, String> {
    let mut result: Vec<Vec<usize>> = (0..ops.len())
        .map(|i| {
            if i + 1 < ops.len() {
                vec![i + 1]
            } else {
                Vec::new()
            }
        })
        .collect();
    let mut branches = Vec::new();
    let mut loops: Vec<(usize, Vec<usize>, Vec<usize>)> = Vec::new();
    for (index, op) in ops.iter().enumerate() {
        match op.opcode {
            31 => branches.push((index, None)),
            18 => {
                let top = branches.last_mut().ok_or("else without if")?;
                top.1 = Some(index);
            }
            21 => {
                let (start, otherwise) = branches.pop().ok_or("endif without if")?;
                result[start].push(otherwise.map_or(index + 1, |value| value + 1));
                if let Some(otherwise) = otherwise {
                    result[otherwise] = vec![index + 1];
                }
            }
            48 => loops.push((index, Vec::new(), Vec::new())),
            2 | 3 => loops.last_mut().ok_or("break outside loop")?.1.push(index),
            7 | 8 => loops
                .last_mut()
                .ok_or("continue outside loop")?
                .2
                .push(index),
            22 => {
                let (start, breaks, continues) = loops.pop().ok_or("endloop without loop")?;
                result[index] = vec![start + 1];
                for jump in breaks {
                    if ops[jump].opcode == 2 {
                        result[jump].clear();
                    }
                    result[jump].push(index + 1);
                }
                for jump in continues {
                    if ops[jump].opcode == 7 {
                        result[jump].clear();
                    }
                    result[jump].push(start + 1);
                }
            }
            62 => result[index].clear(),
            _ => {}
        }
    }
    if !branches.is_empty() || !loops.is_empty() {
        return Err("unbalanced shader flow".into());
    }
    for edges in &mut result {
        edges.retain(|&index| index < ops.len());
    }
    Ok(result)
}

pub fn remove_dead(ops: Vec<Op>, temps: usize) -> Result<(Vec<Op>, usize), String> {
    let edges = successors(&ops)?;
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
            let dead = ops[index].pure
                && !writes[index].is_empty()
                && ops[index].operands[..ops[index].destinations]
                    .iter()
                    .all(|x| x.kind == 0 || x.kind == 13)
                && writes[index].iter().all(|&(register, mask)| {
                    state.get(register).is_some_and(|live| live & mask == 0)
                });
            if !dead {
                for &(register, mask) in &writes[index] {
                    let value = state
                        .get_mut(register)
                        .ok_or("temporary index exceeds declaration")?;
                    *value &= !mask;
                }
                for &(register, mask) in &reads[index] {
                    let value = state
                        .get_mut(register)
                        .ok_or("read index exceeds declaration")?;
                    *value |= mask;
                }
            }
            if state != live[index] {
                live[index] = state;
                changed = true;
            }
        }
    }
    let mut kept = Vec::new();
    let mut removed = 0;
    for (index, op) in ops.into_iter().enumerate() {
        let mut output = vec![0u8; temps];
        for &next in &edges[index] {
            for (dst, &src) in output.iter_mut().zip(&live[next]) {
                *dst |= src;
            }
        }
        let dead = op.pure
            && !writes[index].is_empty()
            && op.operands[..op.destinations]
                .iter()
                .all(|x| x.kind == 0 || x.kind == 13)
            && writes[index]
                .iter()
                .all(|&(register, mask)| output[register] & mask == 0);
        if dead {
            removed += 1;
        } else {
            kept.push(op);
        }
    }
    Ok((kept, removed))
}

pub fn remove_empty_branches(ops: Vec<Op>) -> Result<(Vec<Op>, usize), String> {
    let mut branches = Vec::new();
    let mut removed = vec![false; ops.len()];
    for (index, op) in ops.iter().enumerate() {
        match op.opcode {
            31 => branches.push(index),
            21 => {
                let start = branches.pop().ok_or("endif without if")?;
                if (start + 1..index).all(|at| removed[at] || matches!(ops[at].opcode, 18 | 58)) {
                    removed[start..=index].fill(true);
                }
            }
            _ => {}
        }
    }
    if !branches.is_empty() {
        return Err("if without endif".into());
    }
    let count = removed.iter().filter(|&&value| value).count();
    Ok((
        ops.into_iter()
            .zip(removed)
            .filter_map(|(op, remove)| (!remove).then_some(op))
            .collect(),
        count,
    ))
}
