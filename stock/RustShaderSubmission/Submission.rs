#[path = "UnityAbi.rs"]
pub mod abi;
#[path = "Parallel.rs"]
mod parallel;
#[path = "PropertyIndex.rs"]
mod property_index;
#[path = "ValueApply.rs"]
mod value_apply;

use abi::*;
use std::arch::x86_64::*;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr;

struct Lookup {
    indexed: bool,
    cache: property_index::Cache,
}

impl Lookup {
    const fn new() -> Self {
        Self {
            indexed: false,
            cache: property_index::Cache::new(),
        }
    }

    unsafe fn prepare(&mut self, properties: &PropertySheet) -> bool {
        if !properties.names.valid()
            || !properties.descriptions.valid()
            || !properties.values.valid()
            || properties.names.len != properties.descriptions.len
            || properties.starts[0] < 0
            || properties.starts[6] as usize > properties.names.len
            || properties.starts.windows(2).any(|pair| pair[0] > pair[1])
        {
            return false;
        }
        self.indexed = properties.names.len > 16;
        if !self.indexed {
            return true;
        }
        self.cache.prepare(properties);
        true
    }

    #[inline]
    unsafe fn find(&self, properties: &PropertySheet, kind: usize, name: u32) -> Option<usize> {
        if self.indexed {
            if let Some(index) = self.cache.find(kind, name) {
                return index;
            }
        }
        let mut index = properties.starts[kind] as usize;
        let end = properties.starts[kind + 1] as usize;
        let target = _mm_set1_epi32(name as i32);
        while index + 4 <= end {
            let names = _mm_loadu_si128(properties.names.data.add(index).cast::<__m128i>());
            let bits = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(names, target))) as u32;
            if bits != 0 {
                return Some(index + bits.trailing_zeros() as usize);
            }
            index += 4;
        }
        while index < end {
            if *properties.names.data.add(index) == name {
                return Some(index);
            }
            index += 1;
        }
        None
    }
}

struct Writer<'a> {
    output: &'a mut Array<u8>,
    api: &'a Api,
}
impl Writer<'_> {
    unsafe fn reserve(&mut self, size: usize) -> Option<*mut u8> {
        let end = self.output.len.checked_add(size)?;
        if end > self.output.capacity_tag >> 1 {
            (self.api.resize)(self.output, end, 0);
        }
        if end > self.output.capacity_tag >> 1 || self.output.data.is_null() {
            return None;
        }
        let start = self.output.data.add(self.output.len);
        self.output.len = end;
        Some(start)
    }
    unsafe fn word(&mut self, value: u32) -> Option<()> {
        ptr::write_unaligned(self.reserve(4)?.cast::<u32>(), value);
        Some(())
    }
    unsafe fn data(&mut self, source: *const u8, size: usize) -> Option<()> {
        if size != 0 {
            ptr::copy_nonoverlapping(source, self.reserve(size)?, size);
        }
        Some(())
    }
}

#[derive(Clone, Copy)]
struct BufferBinding {
    index: u32,
    buffer: u64,
    offset: u32,
    size: u32,
}
struct Scratch {
    lookup: Lookup,
    constants: Vec<BufferBinding>,
    copies: Vec<parallel::CopyRange>,
}
impl Scratch {
    const fn new() -> Self {
        Self {
            lookup: Lookup::new(),
            constants: Vec::new(),
            copies: Vec::new(),
        }
    }
}
thread_local! { static SCRATCH: RefCell<Scratch> = const { RefCell::new(Scratch::new()) }; }

fn separate(output: &Array<u8>, source: *const u8, bytes: usize) -> bool {
    if bytes == 0 || output.capacity_tag >> 1 == 0 {
        return true;
    }
    let Some(output_end) = (output.data as usize).checked_add(output.capacity_tag >> 1) else {
        return false;
    };
    let Some(source_end) = (source as usize).checked_add(bytes) else {
        return false;
    };
    output_end <= source as usize || source_end <= output.data as usize
}

fn separate_array<T>(output: &Array<u8>, array: &Array<T>) -> bool {
    array
        .len
        .checked_mul(std::mem::size_of::<T>())
        .is_some_and(|size| separate(output, array.data.cast::<u8>(), size))
}

unsafe fn property_data(
    properties: &PropertySheet,
    index: usize,
    size: usize,
) -> Option<(*const u8, u32)> {
    let description = *properties.descriptions.data.add(index);
    let offset = (description & 0xfffff) as usize;
    if offset.checked_add(size)? > properties.values.len {
        return None;
    }
    Some((properties.values.data.wrapping_add(offset), description))
}

unsafe fn prepare_values(
    parameters: &Array<ValueParameter>,
    counts: [u32; 3],
    properties: &PropertySheet,
    lookup: &Lookup,
    writer: &mut Writer,
    copies: &mut Vec<parallel::CopyRange>,
    defer: bool,
) -> Option<()> {
    let count = counts
        .iter()
        .try_fold(0usize, |sum, &value| sum.checked_add(value as usize))?;
    if !parameters.valid() || count > parameters.len || count >= 0xffff {
        return None;
    }
    let mut parameter_index = 0;
    for (kind, &amount) in counts.iter().enumerate() {
        let width = [4usize, 16, 64][kind];
        for _ in 0..amount {
            let parameter = &*parameters.data.add(parameter_index);
            if let Some(index) = lookup.find(properties, kind, parameter.name) {
                let (_, description) = property_data(properties, index, 0)?;
                let declared = if parameter.array_size == 0 {
                    1
                } else {
                    parameter.array_size as u16
                };
                let elements = declared.min(((description >> 20) & 1023) as u16);
                let size = elements as usize * width;
                let (source, _) = property_data(properties, index, size)?;
                let destination = writer.reserve(4 + size)?;
                ptr::write_unaligned(
                    destination.cast::<u32>(),
                    parameter_index as u32 | (elements as u32) << 16,
                );
                if size != 0 {
                    if defer {
                        if copies.try_reserve(1).is_err() {
                            return None;
                        }
                        copies.push(parallel::CopyRange {
                            source: source as usize,
                            destination: writer.output.len - size,
                            bytes: size,
                        });
                    } else {
                        ptr::copy_nonoverlapping(source, destination.add(4), size);
                    }
                }
            }
            parameter_index += 1;
        }
    }
    writer.word(0xffff)
}

unsafe fn prepare_program(
    program: &ProgramParameters,
    properties: &PropertySheet,
    writer: &mut Writer,
    scratch: &mut Scratch,
) -> Option<()> {
    if !program.constants.valid() || !program.textures.valid() || !program.buffers.valid() {
        return None;
    }
    if !separate_array(writer.output, &properties.values)
        || !separate_array(writer.output, &properties.names)
        || !separate_array(writer.output, &properties.descriptions)
        || !separate_array(writer.output, &program.values)
        || !separate_array(writer.output, &program.constants)
        || !separate_array(writer.output, &program.textures)
        || !separate_array(writer.output, &program.buffers)
    {
        return None;
    }
    for index in 0..program.constants.len {
        if !separate_array(writer.output, &(*program.constants.data.add(index)).values) {
            return None;
        }
    }
    if scratch.constants.capacity() < program.constants.len
        && scratch
            .constants
            .try_reserve(program.constants.len - scratch.constants.len())
            .is_err()
    {
        return None;
    }
    scratch.constants.clear();
    scratch.copies.clear();
    let defer = program.maximum_bytes as usize >= parallel::pool().threshold();
    prepare_values(
        &program.values,
        [program.floats, program.vectors, program.matrices],
        properties,
        &scratch.lookup,
        writer,
        &mut scratch.copies,
        defer,
    )?;
    for index in 0..program.constants.len {
        let parameter = &*program.constants.data.add(index);
        let mut external = false;
        if let Some(property) = scratch.lookup.find(properties, 4, parameter.name) {
            let (source, _) = property_data(properties, property, 16)?;
            let buffer = ptr::read_unaligned(source.cast::<u64>());
            if buffer != 0 {
                scratch.constants.push(BufferBinding {
                    index: index as u32,
                    buffer,
                    offset: ptr::read_unaligned(source.add(8).cast::<u32>()),
                    size: ptr::read_unaligned(source.add(12).cast::<u32>()),
                });
                external = true;
            }
        }
        if external || parameter.kind == 2 {
            writer.word(0xffff)?;
        } else {
            prepare_values(
                &parameter.values,
                [parameter.floats, parameter.vectors, parameter.matrices],
                properties,
                &scratch.lookup,
                writer,
                &mut scratch.copies,
                defer,
            )?;
        }
    }
    if defer {
        let bytes = scratch.copies.iter().map(|range| range.bytes).sum();
        if !parallel::pool().copy(&scratch.copies, writer.output.data, bytes) {
            for range in &scratch.copies {
                ptr::copy_nonoverlapping(
                    range.source as *const u8,
                    writer.output.data.add(range.destination),
                    range.bytes,
                );
            }
        }
    }
    let count_offset = writer.output.len;
    writer.word(0)?;
    let mut textures = 0u32;
    for index in 0..program.textures.len {
        let parameter = &*program.textures.data.add(index);
        if let Some(property) = scratch.lookup.find(properties, 3, parameter.name) {
            let (source, _) = property_data(properties, property, 24)?;
            let texture = ptr::read_unaligned(source.cast::<u32>());
            let dimension = ptr::read_unaligned(source.add(4).cast::<i32>());
            if texture != 0 && (parameter.dimension == 1 || parameter.dimension == dimension) {
                let mut binding = TextureBinding {
                    texture_unit: 0,
                    sampler_unit: 0,
                    multisampled: 0,
                    texture: 0,
                };
                (writer.api.texture)(
                    source,
                    parameter.texture_unit,
                    parameter.sampler_unit,
                    parameter.multisampled,
                    &mut binding,
                );
                writer.data((&binding as *const TextureBinding).cast::<u8>(), 16)?;
                textures += 1;
            }
        }
    }
    ptr::write_unaligned(writer.output.data.add(count_offset).cast::<u32>(), textures);
    for index in 0..program.buffers.len {
        let parameter = &*program.buffers.data.add(index);
        if let Some(property) = scratch.lookup.find(properties, 5, parameter.name) {
            let (source, _) = property_data(properties, property, 8)?;
            writer.word(index as u32)?;
            writer.data(source, 8)?;
        }
    }
    writer.word(u32::MAX)?;
    writer.word(0)?;
    writer.word(scratch.constants.len() as u32)?;
    for binding in &scratch.constants {
        let destination = writer.reserve(24)?;
        ptr::write_unaligned(destination.cast::<u32>(), binding.index);
        // AI-generated comment: Bytes 4..8 are C++ structure padding, never consumed by a binding.
        ptr::write_unaligned(destination.add(8).cast::<u64>(), binding.buffer);
        ptr::write_unaligned(destination.add(16).cast::<u32>(), binding.offset);
        ptr::write_unaligned(destination.add(20).cast::<u32>(), binding.size);
    }
    Some(())
}

unsafe fn apply_bound_values(
    device: *mut u8,
    stage: i32,
    program: &ProgramParameters,
    output: &Array<u8>,
    api: &Api,
) -> bool {
    if !(0..STAGES as i32).contains(&stage) || !program.constants.valid() {
        return false;
    }
    let end = output.data.add(output.len);
    let Some(mut cursor) =
        value_apply::apply_values(&program.values, output.data, end, ptr::null_mut())
    else {
        return false;
    };
    let mut current_constant = ptr::null_mut();
    for index in 0..program.constants.len {
        let parameter = &*program.constants.data.add(index);
        let constant = if parameter.binding < 0 {
            ptr::null_mut()
        } else {
            if parameter.binding as usize >= SLOTS {
                return false;
            }
            ptr::read_unaligned(
                device
                    .add(
                        ACTIVE_CONSTANTS_OFFSET
                            + stage as usize * 256
                            + parameter.binding as usize * 16,
                    )
                    .cast::<*mut ConstantBuffer>(),
            )
        };
        let Some(next) = value_apply::apply_values(&parameter.values, cursor, end, constant) else {
            return false;
        };
        cursor = next;
        current_constant = constant;
    }
    let mut applier = Applier {
        constant: current_constant,
        device,
        stage,
        padding: 0,
        parameters: &program.constants,
    };
    cursor = (api.apply_textures)(cursor, device, stage);
    loop {
        let index = ptr::read_unaligned(cursor.cast::<i32>());
        cursor = cursor.add(4);
        if index == -1 {
            break;
        }
        let buffer = ptr::read_unaligned(cursor.cast::<u64>());
        cursor = cursor.add(8);
        let parameter = &*program.buffers.data.add(index as usize);
        let table = ptr::read(device.cast::<*const usize>());
        let bind: unsafe extern "C" fn(*mut u8, i32, i32, u64) =
            std::mem::transmute(*table.add(0xa18 / 8));
        bind(device, stage, parameter.binding, buffer);
    }
    cursor = (api.apply_samplers)(cursor, device, stage);
    let count = ptr::read_unaligned(cursor.cast::<u32>());
    cursor = cursor.add(4);
    for _ in 0..count {
        (api.apply_constant)(&mut applier, cursor);
        cursor = cursor.add(24);
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn KoeRustPrepare(
    program: *const ProgramParameters,
    properties: *const PropertySheet,
    output: *mut Array<u8>,
    api: *const Api,
) -> i32 {
    if program.is_null() || properties.is_null() || output.is_null() || api.is_null() {
        return 0;
    }
    SCRATCH.with(|cell| {
        let Ok(mut scratch) = cell.try_borrow_mut() else {
            return 0;
        };
        if !(*output).valid() || !scratch.lookup.prepare(&*properties) {
            return 0;
        }
        let mut writer = Writer {
            output: &mut *output,
            api: &*api,
        };
        prepare_program(&*program, &*properties, &mut writer, &mut scratch).is_some() as i32
    })
}

#[no_mangle]
pub unsafe extern "C" fn KoeRustApplyValues(
    parameters: *const Array<ValueParameter>,
    input: *const u8,
    end: *const u8,
    constant: *mut ConstantBuffer,
) -> *const u8 {
    if parameters.is_null() || input.is_null() || end.is_null() {
        return ptr::null();
    }
    value_apply::apply_values(&*parameters, input, end, constant).unwrap_or(ptr::null())
}

#[no_mangle]
pub unsafe extern "C" fn KoeRustTrySubmit(
    device: *mut u8,
    properties: *const PropertySheet,
    api: *const Api,
    mode: u32,
) -> i32 {
    if device.is_null() || properties.is_null() || api.is_null() {
        return 0;
    }
    let properties = &*properties;
    if properties.names.len == 0 {
        return 1;
    }
    SCRATCH.with(|cell| {
        let Ok(mut scratch) = cell.try_borrow_mut() else {
            return 0;
        };
        if !scratch.lookup.prepare(properties) {
            return 0;
        }
        let api = &*api;
        let output = &mut *device.add(SCRATCH_OFFSET).cast::<Array<u8>>();
        if !output.valid() {
            return 0;
        }
        let real_device = if mode >= 2 {
            (api.real_device)()
        } else {
            ptr::null_mut()
        };
        let mut prepared_stages = 0i32;
        let mut applied_stages = 0i32;
        for stage in 0..STAGES {
            let shader = ptr::read(
                device
                    .add(ACTIVE_PROGRAMS_OFFSET)
                    .cast::<*mut u8>()
                    .add(stage),
            );
            let program = ptr::read(
                device
                    .add(ACTIVE_PARAMETERS_OFFSET)
                    .cast::<*const ProgramParameters>()
                    .add(stage),
            );
            if shader.is_null() || program.is_null() {
                continue;
            }
            output.len = 0;
            let mut writer = Writer { output, api };
            if prepare_program(&*program, properties, &mut writer, &mut scratch).is_none() {
                writer.output.len = 0;
                (api.prepare)(program, properties, writer.output);
            } else {
                prepared_stages |= 1 << stage;
            }
            let table = ptr::read(shader.cast::<*const *const c_void>());
            let entry = *table.add(2);
            let shader_stage = ptr::read_unaligned(shader.add(8).cast::<i32>());
            let applied = mode >= 2
                && real_device == device
                && entry == api.shader_apply
                && apply_bound_values(device, shader_stage, &*program, writer.output, api);
            if !applied {
                let apply: unsafe extern "C" fn(*mut u8, *const ProgramParameters, *const u8, u8) =
                    std::mem::transmute(entry);
                apply(shader, program, writer.output.data, 0);
            } else {
                applied_stages |= 1 << stage;
            }
        }
        1 | (prepared_stages << 1) | (applied_stages << 8)
    })
}
