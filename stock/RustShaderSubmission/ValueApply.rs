use crate::abi::{Array, ConstantBuffer, ValueParameter};
use std::arch::x86_64::*;
use std::ptr;

#[inline]
unsafe fn update_bits(
    buffer: &mut ConstantBuffer,
    destination: *mut u8,
    source: *const u8,
    size: usize,
    force: bool,
) {
    let equal = !force
        && (0..size / 8).all(|i| {
            ptr::read_unaligned(destination.add(i * 8).cast::<u64>())
                == ptr::read_unaligned(source.add(i * 8).cast::<u64>())
        });
    if !equal {
        ptr::copy_nonoverlapping(source, destination, size);
        buffer.dirty = 1;
    }
}

pub unsafe fn apply_values(
    parameters: &Array<ValueParameter>,
    mut input: *const u8,
    end: *const u8,
    constant: *mut ConstantBuffer,
) -> Option<*const u8> {
    if !parameters.valid() {
        return None;
    }
    loop {
        if (end as usize).checked_sub(input as usize)? < 4 {
            return None;
        }
        let header = ptr::read_unaligned(input.cast::<u32>());
        input = input.add(4);
        let index = (header & 0xffff) as usize;
        if index == 0xffff {
            return Some(input);
        }
        if index >= parameters.len {
            return None;
        }
        let parameter = &*parameters.data.add(index);
        let count = (header >> 16) as usize;
        let matrix = parameter.flags & 1 != 0;
        let scalar = !matrix && parameter.columns == 1;
        let width = if matrix {
            64
        } else if scalar {
            4
        } else {
            16
        };
        let size = count.checked_mul(width)?;
        if size > (end as usize).checked_sub(input as usize)? {
            return None;
        }
        if !constant.is_null() && count != 0 {
            let buffer = &mut *constant;
            let offset = usize::try_from(parameter.offset).ok()?;
            let columns = parameter.columns as usize;
            if columns > 4 || buffer.data.is_null() {
                return None;
            }
            let target_size = if matrix {
                size
            } else {
                (count - 1).checked_mul(16)?.checked_add(columns * 4)?
            };
            if offset.checked_add(target_size)? > buffer.size as usize {
                return None;
            }
            let target = buffer.data.add(offset);
            let target_end = (target as usize).checked_add(target_size)?;
            if (target as usize) < (input as usize).checked_add(size)?
                && (input as usize) < target_end
            {
                return None;
            }
            if matrix {
                update_bits(buffer, target, input, size, count > 4);
            } else if scalar {
                for i in 0..count {
                    let source = input.add(i * 4);
                    let destination = target.add(i * 16);
                    if parameter.scalar_type == 0 {
                        let value = ptr::read_unaligned(source.cast::<u32>());
                        if ptr::read_unaligned(destination.cast::<u32>()) != value {
                            ptr::write_unaligned(destination.cast::<u32>(), value);
                            buffer.dirty = 1;
                        }
                    } else {
                        let value = _mm_cvttss_si32(_mm_load_ss(source.cast::<f32>()));
                        if ptr::read_unaligned(destination.cast::<i32>()) != value {
                            ptr::write_unaligned(destination.cast::<i32>(), value);
                            buffer.dirty = 1;
                        }
                    }
                }
            } else {
                for i in 0..count {
                    let source = input.add(i * 16);
                    let destination = target.add(i * 16);
                    if parameter.scalar_type == 0 && columns == 4 {
                        update_bits(buffer, destination, source, 16, count > 16);
                        continue;
                    }
                    let converted;
                    let bytes = if parameter.scalar_type == 0 {
                        source
                    } else {
                        let mut words = [0i32; 4];
                        _mm_storeu_si128(
                            words.as_mut_ptr().cast::<__m128i>(),
                            _mm_cvttps_epi32(_mm_loadu_ps(source.cast::<f32>())),
                        );
                        converted = words;
                        converted.as_ptr().cast::<u8>()
                    };
                    let changed = count > 16
                        || (0..columns).any(|j| {
                            ptr::read_unaligned(destination.add(j * 4).cast::<u32>())
                                != ptr::read_unaligned(bytes.add(j * 4).cast::<u32>())
                        });
                    if changed {
                        ptr::copy_nonoverlapping(bytes, destination, columns * 4);
                        buffer.dirty = 1;
                    }
                }
            }
        }
        input = input.add(size);
    }
}
