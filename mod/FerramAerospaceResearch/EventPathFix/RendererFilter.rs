#![no_std]

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop { core::hint::spin_loop(); }
}

// AI-generated comment: The caller owns live Component pointers on Unity's main thread.
// Layout offsets come from an authenticated engine binding, never from user configuration.
#[no_mangle]
pub unsafe extern "C" fn KoeFilterRendererLayers(
    components: *const usize, count: usize, excluded: *mut u8,
    owner_offset: usize, layer_offset: usize,
) -> i32 {
    if count == 0 { return 0; }
    if components.is_null() || excluded.is_null() || count > i32::MAX as usize
        || owner_offset > 4096 || layer_offset > 4096 { return -1; }
    let input = core::slice::from_raw_parts(components, count);
    let output = core::slice::from_raw_parts_mut(excluded, count);
    for (component, flag) in input.iter().copied().zip(output.iter_mut()) {
        if component == 0 { *flag = 0; continue; }
        let Some(owner_address) = component.checked_add(owner_offset) else { return -2; };
        let owner = core::ptr::read_unaligned(owner_address as *const usize);
        if owner == 0 { return -2; }
        let Some(layer_address) = owner.checked_add(layer_offset) else { return -2; };
        let layer = core::ptr::read_unaligned(layer_address as *const i32);
        if !(0..32).contains(&layer) { return -3; }
        *flag = u8::from(layer != 0);
    }
    0
}

#[repr(C)]
pub struct Color { pub r: f32, pub g: f32, pub b: f32, pub a: f32 }
#[repr(C)]
pub struct PropertyApi {
    pub set_color: Option<unsafe extern "C" fn(usize, i32, *const Color, u8)>,
    pub set_block: Option<unsafe extern "C" fn(usize, usize)>,
}

#[no_mangle]
pub unsafe extern "C" fn KoeSetupRendererProperties(
    renderers: *const usize, count: usize, block: usize, property: i32,
    color: *const Color, api: *const PropertyApi,
) -> i32 {
    if count == 0 { return 0; }
    if renderers.is_null() || color.is_null() || api.is_null() || block == 0
        || count > i32::MAX as usize { return -1; }
    let Some(set_color) = (*api).set_color else { return -1; };
    let Some(set_block) = (*api).set_block else { return -1; };
    let renderers = core::slice::from_raw_parts(renderers, count);
    // AI-generated comment: Reject the whole batch before any property write so the original path can preserve partial-failure behavior.
    if renderers.iter().any(|renderer| *renderer == 0) { return -1; }
    for renderer in renderers {
        set_color(block, property, color, 1);
        set_block(*renderer, block);
    }
    0
}

#[cfg(test)]
extern crate std;
#[cfg(test)]
mod tests {
    use super::*;
    #[repr(C)] struct Owner { padding: [u8; 8], layer: i32 }
    #[repr(C)] struct Component { padding: [u8; 16], owner: *const Owner }
    #[test]
    fn layers_and_nulls() {
        for layer in 0..32 {
            let owner = Owner { padding: [0; 8], layer };
            let component = Component { padding: [0; 16], owner: &owner };
            let input = [0, &component as *const Component as usize, 0, &component as *const Component as usize];
            let mut output = [255; 4];
            assert_eq!(unsafe { KoeFilterRendererLayers(input.as_ptr(), 4, output.as_mut_ptr(), 16, 8) }, 0);
            assert_eq!(output, [0, u8::from(layer != 0), 0, u8::from(layer != 0)]);
        }
    }
    #[test]
    fn invalid_input_does_not_become_a_valid_mask() {
        let mut output = [255];
        assert_eq!(unsafe { KoeFilterRendererLayers(core::ptr::null(), 1, output.as_mut_ptr(), 16, 8) }, -1);
        let component = Component { padding: [0; 16], owner: core::ptr::null() };
        let input = [&component as *const Component as usize];
        assert_eq!(unsafe { KoeFilterRendererLayers(input.as_ptr(), 1, output.as_mut_ptr(), 16, 8) }, -2);
        let owner = Owner { padding: [0; 8], layer: 32 };
        let component = Component { padding: [0; 16], owner: &owner };
        let input = [&component as *const Component as usize];
        assert_eq!(unsafe { KoeFilterRendererLayers(input.as_ptr(), 1, output.as_mut_ptr(), 16, 8) }, -3);
        assert_eq!(unsafe { KoeFilterRendererLayers(core::ptr::null(), 0, core::ptr::null_mut(), 0, 0) }, 0);
    }
    static WRITES: std::sync::Mutex<std::vec::Vec<(usize, usize)>> = std::sync::Mutex::new(std::vec::Vec::new());
    unsafe extern "C" fn color_write(block: usize, property: i32, _: *const Color, _: u8) {
        WRITES.lock().unwrap().push((block, property as usize));
    }
    unsafe extern "C" fn block_write(renderer: usize, block: usize) {
        WRITES.lock().unwrap().push((renderer, block));
    }
    #[test]
    fn property_writes_retain_order_and_validate_before_writing() {
        let api = PropertyApi { set_color: Some(color_write), set_block: Some(block_write) };
        let color = Color { r: 0.25, g: -0.0, b: 2.0, a: 1.0 };
        let pointers = [10, 20, 10];
        WRITES.lock().unwrap().clear();
        assert_eq!(unsafe { KoeSetupRendererProperties(pointers.as_ptr(), 3, 100, 123, &color, &api) }, 0);
        assert_eq!(*WRITES.lock().unwrap(), std::vec![(100,123),(10,100),(100,123),(20,100),(100,123),(10,100)]);
        WRITES.lock().unwrap().clear();
        let invalid = [10, 0, 20];
        assert_eq!(unsafe { KoeSetupRendererProperties(invalid.as_ptr(), 3, 100, 123, &color, &api) }, -1);
        assert!(WRITES.lock().unwrap().is_empty());
    }
}
