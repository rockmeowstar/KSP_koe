#![no_std]

use core::arch::x86_64::{_mm_cmpeq_epi8, _mm_loadu_si128, _mm_movemask_epi8, _mm_storeu_si128};
use core::ptr::{addr_of, addr_of_mut, read_unaligned};

#[repr(C)]
pub struct Buffer {
    dirty: u8,
    padding: [u8; 7],
    bytes: *mut u8,
}

#[no_mangle]
#[link_section = ".text$koe_rust"]
pub unsafe extern "C" fn rust_uniform_update(
    buffer: *mut Buffer,
    offset: i32,
    source: *const u8,
    blocks: u64,
    force: u8,
) {
    let mut destination = addr_of!((*buffer).bytes).read().offset(offset as isize);
    let mut source = source;
    let mut remaining = blocks;

    if force == 0 {
        if remaining == 0 {
            return;
        }
        if remaining == 1 {
            if read_unaligned(destination.cast::<u64>()) == read_unaligned(source.cast::<u64>())
                && read_unaligned(destination.add(8).cast::<u64>())
                    == read_unaligned(source.add(8).cast::<u64>())
            {
                return;
            }
            let value = _mm_loadu_si128(source.cast());
            _mm_storeu_si128(destination.cast(), value);
            addr_of_mut!((*buffer).dirty).write(1);
            return;
        }
        if read_unaligned(destination.cast::<u64>()) == read_unaligned(source.cast::<u64>()) {
            loop {
                let value = _mm_loadu_si128(source.cast());
                let previous = _mm_loadu_si128(destination.cast());
                if _mm_movemask_epi8(_mm_cmpeq_epi8(value, previous)) != 0xffff {
                    _mm_storeu_si128(destination.cast(), value);
                    destination = destination.add(16);
                    source = source.add(16);
                    remaining -= 1;
                    break;
                }
                destination = destination.add(16);
                source = source.add(16);
                remaining -= 1;
                if remaining == 0 {
                    return;
                }
            }
        }
    }
    while remaining != 0 {
        let value = _mm_loadu_si128(source.cast());
        _mm_storeu_si128(destination.cast(), value);
        destination = destination.add(16);
        source = source.add(16);
        remaining -= 1;
    }
    addr_of_mut!((*buffer).dirty).write(1);
}
