use std::ptr;

#[inline(always)]
fn occupied(block: u64) -> i32 {
    let a = block as u8;
    let b = (block >> 8) as u8;
    let bits = block >> 16;
    const LANES: u64 = 0x2492_4924_9249;
    let low = bits & LANES;
    let mid = (bits >> 1) & LANES;
    let high = (bits >> 2) & LANES;
    let zero = if a > b {
        if b > 0 { return 1; }
        low & !mid & !high
    } else if a > 0 {
        !low & mid & high
    } else if b > 0 {
        (!low & !mid & !high) | (!low & mid & high)
    } else {
        return ((low & mid & high) != 0) as i32;
    };
    ((zero & LANES) != LANES) as i32
}

#[no_mangle]
pub extern "C" fn CoverageKernelVersion() -> i32 { 1 }

#[no_mangle]
pub unsafe extern "C" fn BuildCoveragePrefix(
    input: *const u8, input_len: usize, width: u32, height: u32,
    output: *mut i32, output_len: usize,
) -> i32 {
    let blocks = u64::from(width) * u64::from(height);
    let entries = (u64::from(width) + 1) * (u64::from(height) + 1);
    if input.is_null() || output.is_null() || width == 0 || height == 0
        || width > 8192 || height > 8192 || blocks > 4 * 1024 * 1024
        || blocks * 8 != input_len as u64 || entries != output_len as u64 {
        return -1;
    }
    let stride = width as usize + 1;
    ptr::write_bytes(output, 0, stride);
    for y in 0..height as usize {
        let row = output.add((y + 1) * stride);
        let above = output.add(y * stride);
        row.write(0);
        let mut sum = 0;
        for x in 0..width as usize {
            let block = u64::from_le(ptr::read_unaligned(input.add((y * width as usize + x) * 8).cast::<u64>()));
            sum += occupied(block);
            row.add(x + 1).write(sum + above.add(x + 1).read());
        }
    }
    0
}
