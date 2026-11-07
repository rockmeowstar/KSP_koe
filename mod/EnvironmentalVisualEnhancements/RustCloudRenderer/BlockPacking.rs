fn interleave(a: &[u8], b: &[u8], output: &mut [u8]) -> bool {
    if a.len() != b.len() || a.len() % 8 != 0 || a.len().checked_mul(2) != Some(output.len()) {
        return false;
    }
    for ((red, green), block) in a
        .chunks_exact(8)
        .zip(b.chunks_exact(8))
        .zip(output.chunks_exact_mut(16))
    {
        block[..8].copy_from_slice(red);
        block[8..].copy_from_slice(green);
    }
    true
}

// AI-generated comment: The adapter owns valid readable inputs and a disjoint writable output.
#[no_mangle]
pub unsafe extern "C" fn koe_pair_bc4(
    a: *const u8,
    b: *const u8,
    length: usize,
    output: *mut u8,
    output_length: usize,
) -> i32 {
    if a.is_null()
        || b.is_null()
        || output.is_null()
        || length > isize::MAX as usize / 2
        || length.checked_mul(2) != Some(output_length)
    {
        return -1;
    }
    let start = output as usize;
    let Some(end) = start.checked_add(output_length) else {
        return -1;
    };
    for input in [a as usize, b as usize] {
        let Some(input_end) = input.checked_add(length) else {
            return -1;
        };
        if input < end && input_end > start {
            return -2;
        }
    }
    if interleave(
        std::slice::from_raw_parts(a, length),
        std::slice::from_raw_parts(b, length),
        std::slice::from_raw_parts_mut(output, output_length),
    ) {
        0
    } else {
        -3
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn every_endpoint_and_index_byte_survives() {
        let a: Vec<_> = (0..65536).map(|x| (x * 17) as u8).collect();
        let b: Vec<_> = (0..65536).map(|x| (x * 131 + 71) as u8).collect();
        let mut output = vec![0; a.len() * 2];
        assert!(interleave(&a, &b, &mut output));
        for i in 0..a.len() / 8 {
            assert_eq!(&output[i * 16..i * 16 + 8], &a[i * 8..i * 8 + 8]);
            assert_eq!(&output[i * 16 + 8..i * 16 + 16], &b[i * 8..i * 8 + 8]);
        }
    }
    #[test]
    fn truncated_or_aliased_payloads_are_rejected() {
        assert!(!interleave(&[0; 7], &[0; 7], &mut [0; 14]));
        assert!(!interleave(&[0; 8], &[0; 16], &mut [0; 16]));
        let mut a = [0u8; 32];
        let b = [0u8; 16];
        assert_eq!(
            unsafe { koe_pair_bc4(a.as_ptr(), b.as_ptr(), 16, a.as_mut_ptr(), 32) },
            -2
        );
    }
}
