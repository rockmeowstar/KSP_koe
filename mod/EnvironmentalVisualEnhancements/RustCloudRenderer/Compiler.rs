#![allow(dead_code)]
#[path = "HalfAnalysis.rs"]
mod analysis;
#[path = "ShaderIr.rs"]
mod ir;
#[path = "HalfOptimizer.rs"]
mod optimizer;
#[path = "ShaderProgram.rs"]
mod program;

pub struct Compiled {
    bytes: Vec<u8>,
}

// AI-generated comment: Input and output pointers are owned by the native adapter;
// rejected programs and Rust panics leave the original D3D11 shader in use.
#[no_mangle]
pub unsafe extern "C" fn koe_compile_cloud_half(
    input: *const u8,
    length: usize,
    output: *mut *const u8,
    output_length: *mut usize,
    replacements: *mut usize,
) -> *mut Compiled {
    koe_compile_cloud_half_variant(input, length, 1, output, output_length, replacements)
}

#[no_mangle]
pub unsafe extern "C" fn koe_compile_cloud_half_variant(
    input: *const u8,
    length: usize,
    mode: u32,
    output: *mut *const u8,
    output_length: *mut usize,
    replacements: *mut usize,
) -> *mut Compiled {
    if input.is_null() || output.is_null() || output_length.is_null() || replacements.is_null() {
        return std::ptr::null_mut();
    }
    *output = std::ptr::null();
    *output_length = 0;
    *replacements = 0;
    let result = std::panic::catch_unwind(|| {
        let bytes = std::slice::from_raw_parts(input, length);
        let program = program::Program::parse(bytes).ok()?;
        if program.stage != 0 || !(4..=5).contains(&program.major) {
            return None;
        }
        let instructions = ir::decode(&program).ok()?;
        let temps = instructions
            .iter()
            .find(|op| op.opcode == 104)?
            .words
            .get(1)
            .copied()?;
        if temps == 0 || temps > 4096 {
            return None;
        }
        if !(1..=4).contains(&mode) {
            return None;
        }
        let repair = if mode == 1 || mode == 3 {
            optimizer::Repair::Branch
        } else {
            optimizer::Repair::Select
        };
        let optimized = optimizer::optimize_passes(&program, repair, mode <= 2).ok()?;
        if optimized.stats.replacements == 0 {
            return None;
        }
        Some((
            Compiled {
                bytes: optimized.bytes,
            },
            optimized.stats.replacements,
        ))
    });
    match result {
        Ok(Some((compiled, count))) => {
            let compiled = Box::new(compiled);
            *output = compiled.bytes.as_ptr();
            *output_length = compiled.bytes.len();
            *replacements = count;
            Box::into_raw(compiled)
        }
        _ => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn koe_free_cloud_half(compiled: *mut Compiled) {
    if !compiled.is_null() {
        drop(Box::from_raw(compiled));
    }
}
