use std::collections::BTreeMap;
use std::ops::Range;

#[derive(Debug, PartialEq)]
pub enum Error {
    Header,
    Size,
    ChunkTable,
    ChunkBounds,
    OverlappingChunks,
    MissingCode,
    CodeSize,
    InstructionSize,
}

pub struct Chunk {
    pub kind: [u8; 4],
    pub bytes: Range<usize>,
}
pub struct Instruction {
    pub opcode: u16,
    pub words: Range<usize>,
}
pub struct Program<'a> {
    pub original: &'a [u8],
    pub chunks: Vec<Chunk>,
    pub code: Range<usize>,
    pub stage: u16,
    pub major: u8,
    pub minor: u8,
    pub instructions: Vec<Instruction>,
}

fn word(bytes: &[u8], offset: usize) -> Result<u32, Error> {
    let end = offset.checked_add(4).ok_or(Error::Size)?;
    let data = bytes.get(offset..end).ok_or(Error::Size)?;
    Ok(u32::from_le_bytes(data.try_into().unwrap()))
}

impl<'a> Program<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, Error> {
        if bytes.len() < 32 || bytes.get(..4) != Some(b"DXBC") {
            return Err(Error::Header);
        }
        if word(bytes, 24)? as usize != bytes.len() {
            return Err(Error::Size);
        }
        let count = word(bytes, 28)? as usize;
        let header = 32usize
            .checked_add(count.checked_mul(4).ok_or(Error::ChunkTable)?)
            .ok_or(Error::ChunkTable)?;
        if header > bytes.len() {
            return Err(Error::ChunkTable);
        }
        let mut chunks = Vec::new();
        let mut code = None;
        for index in 0..count {
            let start = word(bytes, 32 + index * 4)? as usize;
            if start < header {
                return Err(Error::ChunkBounds);
            }
            let payload = start.checked_add(8).ok_or(Error::ChunkBounds)?;
            let size = word(bytes, start.checked_add(4).ok_or(Error::ChunkBounds)?)? as usize;
            let end = payload.checked_add(size).ok_or(Error::ChunkBounds)?;
            if end > bytes.len() {
                return Err(Error::ChunkBounds);
            }
            let kind: [u8; 4] = bytes
                .get(start..start + 4)
                .ok_or(Error::ChunkBounds)?
                .try_into()
                .unwrap();
            if chunks
                .iter()
                .any(|chunk: &Chunk| start < chunk.bytes.end && chunk.bytes.start < end)
            {
                return Err(Error::OverlappingChunks);
            }
            if &kind == b"SHEX" || &kind == b"SHDR" {
                if code.replace(payload..end).is_some() {
                    return Err(Error::MissingCode);
                }
            }
            chunks.push(Chunk {
                kind,
                bytes: start..end,
            });
        }
        let code = code.ok_or(Error::MissingCode)?;
        if code.len() < 8 || code.len() % 4 != 0 {
            return Err(Error::CodeSize);
        }
        let version = word(bytes, code.start)?;
        let length = word(bytes, code.start + 4)? as usize;
        if length != code.len() / 4 {
            return Err(Error::CodeSize);
        }
        let mut instructions = Vec::new();
        let mut position = 2;
        while position < length {
            let token = word(bytes, code.start + position * 4)?;
            let opcode = (token & 0x7ff) as u16;
            let size = if opcode == 53 {
                if position + 1 >= length {
                    return Err(Error::InstructionSize);
                }
                let size = word(bytes, code.start + (position + 1) * 4)? as usize;
                if size < 2 {
                    return Err(Error::InstructionSize);
                }
                size
            } else {
                ((token >> 24) & 0x7f) as usize
            };
            let end = position.checked_add(size).ok_or(Error::InstructionSize)?;
            if size == 0 || end > length {
                return Err(Error::InstructionSize);
            }
            instructions.push(Instruction {
                opcode,
                words: position..end,
            });
            position = end;
        }
        Ok(Self {
            original: bytes,
            chunks,
            code,
            stage: (version >> 16) as u16,
            major: ((version >> 4) & 15) as u8,
            minor: (version & 15) as u8,
            instructions,
        })
    }
    pub fn histogram(&self) -> BTreeMap<u16, usize> {
        let mut result = BTreeMap::new();
        for instruction in &self.instructions {
            *result.entry(instruction.opcode).or_insert(0) += 1;
        }
        result
    }
    pub fn instruction_bytes(&self, instruction: &Instruction) -> &[u8] {
        &self.original[self.code.start + instruction.words.start * 4
            ..self.code.start + instruction.words.end * 4]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn container(tokens: &[u32]) -> Vec<u8> {
        let size = 52 + tokens.len() * 4;
        let mut bytes = vec![0u8; size];
        bytes[..4].copy_from_slice(b"DXBC");
        for (offset, value) in [
            (20, 1),
            (24, size as u32),
            (28, 1),
            (32, 36),
            (40, (8 + tokens.len() * 4) as u32),
            (44, 0x50),
            (48, (tokens.len() + 2) as u32),
        ] {
            bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        bytes[36..40].copy_from_slice(b"SHEX");
        for (index, token) in tokens.iter().enumerate() {
            bytes[52 + index * 4..56 + index * 4].copy_from_slice(&token.to_le_bytes());
        }
        bytes
    }
    #[test]
    fn preserves_each_instruction_bit_for_bit() {
        let bytes = container(&[(1 << 24) | 62]);
        let program = Program::parse(&bytes).unwrap();
        assert_eq!(program.stage, 0);
        assert_eq!(program.major, 5);
        assert_eq!(program.minor, 0);
        assert_eq!(program.instructions.len(), 1);
        assert_eq!(program.instructions[0].opcode, 62);
        assert_eq!(
            program.instruction_bytes(&program.instructions[0]),
            &bytes[52..]
        );
    }
    #[test]
    fn rejects_truncated_containers_and_instructions() {
        let bytes = container(&[(1 << 24) | 62]);
        for size in 0..bytes.len() {
            assert!(Program::parse(&bytes[..size]).is_err());
        }
        assert!(matches!(
            Program::parse(&container(&[62])),
            Err(Error::InstructionSize)
        ));
        assert!(matches!(
            Program::parse(&container(&[(3 << 24) | 62])),
            Err(Error::InstructionSize)
        ));
    }
    #[test]
    fn custom_data_uses_its_explicit_word_length() {
        let bytes = container(&[53, 4, 0xdeadbeef, 0xabcdef01, (1 << 24) | 62]);
        let program = Program::parse(&bytes).unwrap();
        assert_eq!(program.instructions.len(), 2);
        assert_eq!(program.instructions[0].words, 2..6);
    }
    #[test]
    fn damaged_words_never_escape_their_container() {
        let bytes = container(&[(1 << 24) | 62]);
        for position in 0..bytes.len() {
            for value in [0, 1, 127, 255] {
                let mut damaged = bytes.clone();
                damaged[position] = value;
                if let Ok(program) = Program::parse(&damaged) {
                    for instruction in &program.instructions {
                        assert!(!program.instruction_bytes(instruction).is_empty());
                    }
                }
            }
        }
    }
}
