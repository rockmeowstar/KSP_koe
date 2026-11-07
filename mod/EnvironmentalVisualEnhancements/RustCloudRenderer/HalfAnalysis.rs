use crate::ir::{Op, Operand};
use std::collections::{HashMap, HashSet};
use std::sync::{OnceLock, RwLock};

#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq, Ord, PartialOrd)]
pub struct Origin {
    pub at: usize,
    pub before: bool,
    pub register: usize,
}
#[derive(Clone, Copy)]
pub struct Leaf {
    pub origin: Origin,
    pub component: usize,
}
#[derive(Clone)]
enum Expression {
    Constant(u32),
    Variable(usize),
    Unary(u16, usize),
    Binary(u16, usize, usize),
    Select(usize, usize, usize),
}
struct Node {
    expression: Expression,
    leaf: Option<usize>,
    mixed: bool,
    bits: [u32; 32],
    zero: u32,
    one: u32,
}
#[derive(Clone, Hash, Eq, PartialEq)]
enum Term {
    Constant(u32),
    Input,
    Other,
    Unary(u16, usize),
    Binary(u16, usize, usize),
    Select(usize, usize, usize),
}
type Proof = (Vec<Term>, u32, bool);
static VERIFIED: OnceLock<RwLock<HashMap<Proof, bool>>> = OnceLock::new();
#[derive(Clone)]
pub struct Match {
    pub instruction: usize,
    pub component: usize,
    pub leaf: Leaf,
    pub shift: u32,
    pub unsigned: bool,
}
pub struct Analysis {
    pub matches: Vec<Match>,
    pub proved: usize,
    pub evaluated: u64,
}

fn apply_unary(op: u16, value: u32) -> u32 {
    match op {
        40 => value.wrapping_neg(),
        59 => !value,
        _ => unreachable!(),
    }
}
fn apply_binary(op: u16, a: u32, b: u32) -> u32 {
    match op {
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
        60 => a | b,
        85 => a.wrapping_shr(b & 31),
        87 => a ^ b,
        _ => unreachable!(),
    }
}
pub fn half_bits(h: u16) -> u32 {
    let sign = ((h as u32) & 0x8000) << 16;
    let exponent = ((h as u32) >> 10) & 31;
    let mut mantissa = (h as u32) & 1023;
    if exponent == 31 {
        return sign | 0x7f800000 | (mantissa << 13);
    }
    if exponent != 0 {
        return sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    if mantissa == 0 {
        return sign;
    }
    let mut exponent = -14i32;
    while mantissa & 1024 == 0 {
        mantissa <<= 1;
        exponent -= 1;
    }
    sign | (((exponent + 127) as u32) << 23) | ((mantissa & 1023) << 13)
}

struct Arena {
    nodes: Vec<Node>,
    leaves: Vec<Leaf>,
    registers: Vec<[Option<usize>; 4]>,
    start: usize,
}
impl Arena {
    fn new(temps: usize) -> Self {
        Self {
            nodes: Vec::new(),
            leaves: Vec::new(),
            registers: vec![[None; 4]; temps],
            start: 0,
        }
    }
    fn push(&mut self, node: Node) -> usize {
        let index = self.nodes.len();
        self.nodes.push(node);
        index
    }
    fn constant(&mut self, value: u32) -> usize {
        self.push(Node {
            expression: Expression::Constant(value),
            leaf: None,
            mixed: false,
            bits: [0; 32],
            zero: !value,
            one: value,
        })
    }
    fn variable(&mut self, leaf: Leaf) -> usize {
        let index = self.leaves.len();
        self.leaves.push(leaf);
        self.push(Node {
            expression: Expression::Variable(index),
            leaf: Some(index),
            mixed: false,
            bits: std::array::from_fn(|i| 1u32 << i),
            zero: 0,
            one: 0,
        })
    }
    fn get(&mut self, register: usize, component: usize) -> Option<usize> {
        if register >= self.registers.len() {
            return None;
        }
        if let Some(value) = self.registers[register][component] {
            return Some(value);
        }
        let value = self.variable(Leaf {
            origin: Origin {
                at: self.start,
                before: true,
                register,
            },
            component,
        });
        self.registers[register][component] = Some(value);
        Some(value)
    }
    fn constant_value(&self, index: usize) -> Option<u32> {
        let node = &self.nodes[index];
        if node.zero | node.one == u32::MAX {
            Some(node.one)
        } else {
            None
        }
    }
    fn source(&mut self, operand: &Operand, lane: usize, integer_negation: bool) -> Option<usize> {
        let value = if let Some(immediate) = operand.immediate {
            self.constant(immediate[lane])
        } else if operand.kind == 0 && operand.relative.is_empty() {
            self.get(operand.index?, operand.swizzle[lane])?
        } else {
            return None;
        };
        match operand.modifier {
            0 => Some(value),
            1 if integer_negation => Some(self.unary(40, value)),
            _ => None,
        }
    }
    fn unary(&mut self, op: u16, a: usize) -> usize {
        if let Some(value) = self.constant_value(a) {
            return self.constant(apply_unary(op, value));
        }
        let source = &self.nodes[a];
        let mut bits = source.bits;
        let (zero, one) = if op == 59 {
            (source.one, source.zero)
        } else {
            let mut prefix = 0;
            for bit in &mut bits {
                prefix |= *bit;
                *bit = prefix;
            }
            (0, 0)
        };
        self.push(Node {
            expression: Expression::Unary(op, a),
            leaf: source.leaf,
            mixed: source.mixed,
            bits,
            zero,
            one,
        })
    }
    fn binary(&mut self, op: u16, a: usize, b: usize) -> usize {
        let av = self.constant_value(a);
        let bv = self.constant_value(b);
        if let (Some(a), Some(b)) = (av, bv) {
            return self.constant(apply_binary(op, a, b));
        }
        if a == b {
            match op {
                1 | 60 => return a,
                32 => return self.constant(u32::MAX),
                87 => return self.constant(0),
                _ => {}
            }
        }
        let (left, right) = (&self.nodes[a], &self.nodes[b]);
        let (zero, one) = match op {
            1 => (left.zero | right.zero, left.one & right.one),
            60 => (left.zero & right.zero, left.one | right.one),
            87 => (
                (left.zero & right.zero) | (left.one & right.one),
                (left.zero & right.one) | (left.one & right.zero),
            ),
            41 => {
                if let Some(shift) = bv {
                    let n = shift & 31;
                    (
                        (left.zero << n) | if n == 0 { 0 } else { u32::MAX >> (32 - n) },
                        left.one << n,
                    )
                } else {
                    (0, 0)
                }
            }
            85 => {
                if let Some(shift) = bv {
                    let n = shift & 31;
                    (
                        (left.zero >> n) | if n == 0 { 0 } else { u32::MAX << (32 - n) },
                        left.one >> n,
                    )
                } else {
                    (0, 0)
                }
            }
            _ => (0, 0),
        };
        if zero | one == u32::MAX {
            return self.constant(one);
        }
        let mut used_left = false;
        let mut used_right = false;
        let mut bits = [0u32; 32];
        let left_all = left.bits.iter().fold(0, |all, &x| all | x);
        let right_all = right.bits.iter().fold(0, |all, &x| all | x);
        for bit in 0..32 {
            if (zero | one) & (1 << bit) != 0 {
                continue;
            }
            let (ld, rd) = match op {
                1 | 60 | 87 => (left.bits[bit], right.bits[bit]),
                30 => (
                    left.bits[..=bit].iter().fold(0, |all, &x| all | x),
                    right.bits[..=bit].iter().fold(0, |all, &x| all | x),
                ),
                32 => (left_all, right_all),
                41 => {
                    if let Some(n) = bv {
                        let n = (n & 31) as usize;
                        (if bit >= n { left.bits[bit - n] } else { 0 }, 0)
                    } else {
                        (
                            left.bits[..=bit].iter().fold(0, |all, &x| all | x),
                            right.bits[..5].iter().fold(0, |all, &x| all | x),
                        )
                    }
                }
                85 => {
                    if let Some(n) = bv {
                        let n = (n & 31) as usize;
                        (if bit + n < 32 { left.bits[bit + n] } else { 0 }, 0)
                    } else {
                        (
                            left.bits[bit..].iter().fold(0, |all, &x| all | x),
                            right.bits[..5].iter().fold(0, |all, &x| all | x),
                        )
                    }
                }
                _ => unreachable!(),
            };
            used_left |= ld != 0;
            used_right |= rd != 0;
            bits[bit] = ld | rd;
        }
        let mixed = (used_left && left.mixed)
            || (used_right && right.mixed)
            || (used_left && used_right && left.leaf != right.leaf);
        let leaf = if used_left {
            left.leaf
        } else if used_right {
            right.leaf
        } else {
            None
        };
        self.push(Node {
            expression: Expression::Binary(op, a, b),
            leaf,
            mixed,
            bits,
            zero,
            one,
        })
    }
    fn select(&mut self, c: usize, a: usize, b: usize) -> usize {
        if a == b {
            return a;
        }
        if let Some(condition) = self.constant_value(c) {
            return if condition != 0 { a } else { b };
        }
        let (condition, left, right) = (&self.nodes[c], &self.nodes[a], &self.nodes[b]);
        let zero = left.zero & right.zero;
        let one = left.one & right.one;
        if zero | one == u32::MAX {
            return self.constant(one);
        }
        let condition_bits = condition.bits.iter().fold(0, |all, &x| all | x);
        let bits = std::array::from_fn(|i| {
            if (zero | one) & (1 << i) == 0 {
                condition_bits | left.bits[i] | right.bits[i]
            } else {
                0
            }
        });
        let leaves: [Option<usize>; 3] = [condition.leaf, left.leaf, right.leaf];
        let leaf = leaves.iter().copied().flatten().next();
        let mixed = condition.mixed
            || left.mixed
            || right.mixed
            || leaves
                .iter()
                .copied()
                .flatten()
                .any(|other| Some(other) != leaf);
        self.push(Node {
            expression: Expression::Select(c, a, b),
            leaf,
            mixed,
            bits,
            zero,
            one,
        })
    }
    fn plan(&self, root: usize, leaf: usize) -> Vec<Term> {
        fn visit(arena: &Arena, index: usize, seen: &mut HashSet<usize>) {
            if !seen.insert(index) {
                return;
            }
            match arena.nodes[index].expression {
                Expression::Unary(_, a) => visit(arena, a, seen),
                Expression::Binary(_, a, b) => {
                    visit(arena, a, seen);
                    visit(arena, b, seen);
                }
                Expression::Select(c, a, b) => {
                    visit(arena, c, seen);
                    visit(arena, a, seen);
                    visit(arena, b, seen);
                }
                _ => {}
            }
        }
        let mut seen = HashSet::new();
        visit(self, root, &mut seen);
        let mut indices: Vec<_> = seen.into_iter().collect();
        indices.sort_unstable();
        let mapping: HashMap<_, _> = indices
            .iter()
            .enumerate()
            .map(|(local, &global)| (global, local))
            .collect();
        indices
            .iter()
            .map(|&index| match self.nodes[index].expression {
                Expression::Constant(value) => Term::Constant(value),
                Expression::Variable(value) => {
                    if value == leaf {
                        Term::Input
                    } else {
                        Term::Other
                    }
                }
                Expression::Unary(op, a) => Term::Unary(op, mapping[&a]),
                Expression::Binary(op, a, b) => Term::Binary(op, mapping[&a], mapping[&b]),
                Expression::Select(c, a, b) => Term::Select(mapping[&c], mapping[&a], mapping[&b]),
            })
            .collect()
    }
}

fn evaluate(plan: &[Term], input: u32, values: &mut [u32]) -> u32 {
    for (index, term) in plan.iter().enumerate() {
        values[index] = match *term {
            Term::Constant(value) => value,
            Term::Input => input,
            Term::Other => 0,
            Term::Unary(op, a) => apply_unary(op, values[a]),
            Term::Binary(op, a, b) => apply_binary(op, values[a], values[b]),
            Term::Select(c, a, b) => {
                if values[c] != 0 {
                    values[a]
                } else {
                    values[b]
                }
            }
        };
    }
    *values.last().unwrap()
}

pub fn analyze(ops: &[Op], temps: usize) -> Analysis {
    let mut arena = Arena::new(temps);
    let mut matches = Vec::new();
    let mut proved = 0;
    let mut evaluated = 0;
    let mut proofs: HashMap<Proof, bool> = HashMap::new();
    for (instruction, op) in ops.iter().enumerate() {
        if op.destinations == 0 && (88..=106).contains(&op.opcode) {
            arena.start = instruction + 1;
        }
        if matches!(op.opcode, 2 | 3 | 7 | 8 | 18 | 21 | 22 | 31 | 48 | 62 | 63) {
            arena.registers.fill([None; 4]);
            arena.start = instruction + 1;
            continue;
        }
        let integer = matches!(
            op.opcode,
            1 | 30 | 32 | 40 | 41 | 54 | 55 | 59 | 60 | 85 | 87
        ) && op.destinations == 1;
        let destination = op.operands.first();
        let mut results = [None; 4];
        if integer {
            if let Some(destination) = destination {
                for lane in 0..4 {
                    if destination.mask & (1 << lane) == 0 {
                        continue;
                    }
                    let args: Option<Vec<_>> = op.operands[1..]
                        .iter()
                        .map(|operand| arena.source(operand, lane, matches!(op.opcode, 30 | 40)))
                        .collect();
                    if let Some(args) = args {
                        results[lane] = Some(match op.opcode {
                            54 => args[0],
                            40 | 59 => arena.unary(op.opcode, args[0]),
                            55 => arena.select(args[0], args[1], args[2]),
                            _ => arena.binary(op.opcode, args[0], args[1]),
                        });
                    }
                }
            }
        }
        if integer
            && destination.is_some_and(|dst| {
                dst.kind == 0
                    && dst.index.is_some()
                    && dst.end - dst.start == 2
                    && op.words[dst.start] & 15 == 2
            })
        {
            for lane in 0..4 {
                let Some(node) = results[lane] else {
                    continue;
                };
                let info = &arena.nodes[node];
                if info.mixed {
                    continue;
                }
                let Some(leaf) = info.leaf else {
                    continue;
                };
                let mask = info.bits.iter().fold(0, |all, &x| all | x);
                let shift = if mask != 0 && mask & 0xffff0000 == 0 {
                    0
                } else if mask & 0xffff == 0 && mask != 0 {
                    16
                } else {
                    continue;
                };
                let unsigned = mask & (0x8000 << shift) == 0;
                let plan = arena.plan(node, leaf);
                let key = (plan, shift, unsigned);
                let valid = *proofs.entry(key.clone()).or_insert_with(|| {
                    let cache = VERIFIED.get_or_init(|| RwLock::new(HashMap::new()));
                    if let Ok(known) = cache.read() {
                        if let Some(&valid) = known.get(&key) {
                            return valid;
                        }
                    }
                    let valid = (|| {
                        let mut values = vec![0u32; key.0.len()];
                        for h in [
                            0, 0x8000, 1, 0x8001, 0x3c00, 0xbc00, 0x4000, 0x7bff, 0x7c00, 0xfc00,
                            0x7c01, 0x7fff, 0xfc01,
                        ] {
                            evaluated += 1;
                            let expected = half_bits(if unsigned { h & 0x7fff } else { h });
                            if evaluate(&key.0, (h as u32) << shift, &mut values) != expected {
                                return false;
                            }
                        }
                        for h in 0..=u16::MAX {
                            evaluated += 1;
                            let expected = half_bits(if unsigned { h & 0x7fff } else { h });
                            if evaluate(&key.0, (h as u32) << shift, &mut values) != expected {
                                return false;
                            }
                        }
                        proved += 1;
                        true
                    })();
                    if let Ok(mut known) = cache.write() {
                        known.insert(key.clone(), valid);
                    }
                    valid
                });
                if valid {
                    matches.push(Match {
                        instruction,
                        component: lane,
                        leaf: arena.leaves[leaf],
                        shift,
                        unsigned,
                    });
                }
            }
        }
        for (register, mask) in op.writes() {
            if register >= arena.registers.len() {
                continue;
            }
            for lane in 0..4 {
                if mask & (1 << lane) == 0 {
                    continue;
                }
                let value = if op.destinations == 1 {
                    results[lane]
                } else {
                    None
                };
                let value = value.unwrap_or_else(|| {
                    arena.variable(Leaf {
                        origin: Origin {
                            at: instruction,
                            before: false,
                            register,
                        },
                        component: lane,
                    })
                });
                arena.registers[register][lane] = Some(value);
            }
        }
    }
    Analysis {
        matches,
        proved,
        evaluated,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn half_reference_preserves_zero_sign_subnormals_and_nan_payload() {
        assert_eq!(half_bits(0), 0);
        assert_eq!(half_bits(0x8000), 0x80000000);
        assert_eq!(half_bits(1), 0x33800000);
        assert_eq!(half_bits(0x3c00), 0x3f800000);
        assert_eq!(half_bits(0x7c00), 0x7f800000);
        assert_eq!(half_bits(0xfe01), 0xffc02000);
    }
    #[test]
    fn a_second_input_cannot_be_hidden_by_a_hash_match() {
        let mut arena = Arena::new(2);
        let a = arena.get(0, 0).unwrap();
        let b = arena.get(1, 0).unwrap();
        let result = arena.binary(30, a, b);
        assert!(arena.nodes[result].mixed);
    }
    #[test]
    fn constant_bit_masks_remove_only_irrelevant_dependencies() {
        let mut arena = Arena::new(1);
        let a = arena.get(0, 0).unwrap();
        let mask = arena.constant(65535);
        let result = arena.binary(1, a, mask);
        assert_eq!(
            arena.nodes[result].bits.iter().fold(0, |all, &x| all | x),
            65535
        );
    }
}
