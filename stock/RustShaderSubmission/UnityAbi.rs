use std::ffi::c_void;

pub const STAGES: usize = 7;
pub const SLOTS: usize = 16;
pub const SCRATCH_OFFSET: usize = 8616;
pub const ACTIVE_PROGRAMS_OFFSET: usize = 22976;
pub const ACTIVE_PARAMETERS_OFFSET: usize = 23032;
pub const ACTIVE_CONSTANTS_OFFSET: usize = 10304;

#[repr(C)]
pub struct Array<T> {
    pub data: *mut T,
    pub label: u64,
    pub len: usize,
    pub capacity_tag: usize,
}
impl<T> Array<T> {
    pub fn valid(&self) -> bool {
        self.len <= self.capacity_tag >> 1 && (self.len == 0 || !self.data.is_null())
    }
}

#[repr(C)]
pub struct PropertySheet {
    pub object_header: [u64; 2],
    pub starts: [i32; 7],
    pub alignment: u32,
    pub names: Array<u32>,
    pub descriptions: Array<u32>,
    pub values: Array<u8>,
    pub hash: u32,
    pub layout_hash: u32,
    pub allow_add: u8,
    pub padding: [u8; 7],
}

#[repr(C)]
pub struct ValueParameter {
    pub name: u32,
    pub offset: i32,
    pub array_size: i32,
    pub scalar_type: i32,
    pub rows: u8,
    pub columns: u8,
    pub flags: u8,
    pub padding: u8,
}

#[repr(C)]
pub struct TextureParameter {
    pub name: u32,
    pub texture_unit: i32,
    pub sampler_unit: i32,
    pub multisampled: u8,
    pub padding: [u8; 3],
    pub dimension: i32,
}

#[repr(C)]
pub struct BufferParameter {
    pub name: u32,
    pub binding: i32,
    pub padding: [u8; 8],
}

#[repr(C)]
pub struct ConstantParameter {
    pub name: u32,
    pub padding: u32,
    pub values: Array<ValueParameter>,
    pub structures: Array<c_void>,
    pub size: i32,
    pub binding: i32,
    pub kind: i32,
    pub floats: u32,
    pub vectors: u32,
    pub matrices: u32,
    pub handle: *mut c_void,
}

#[repr(C)]
pub struct ProgramParameters {
    pub floats: u32,
    pub vectors: u32,
    pub matrices: u32,
    pub maximum_bytes: u32,
    pub values: Array<ValueParameter>,
    pub textures: Array<TextureParameter>,
    pub uavs: Array<c_void>,
    pub buffers: Array<BufferParameter>,
    pub samplers: Array<c_void>,
    pub constants: Array<ConstantParameter>,
    pub images: Array<c_void>,
    pub atomic_buffers: Array<c_void>,
    pub builtin_matrices: [u8; 624],
}

#[repr(C)]
pub struct ConstantBuffer {
    pub dirty: u8,
    pub bound: u8,
    pub padding: [u8; 6],
    pub data: *mut u8,
    pub device_buffer: *mut c_void,
    pub size: u32,
    pub alignment: u32,
    pub stereo_versions: [*mut ConstantBuffer; 2],
}

#[repr(C)]
pub struct Applier {
    pub constant: *mut ConstantBuffer,
    pub device: *mut u8,
    pub stage: i32,
    pub padding: u32,
    pub parameters: *const Array<ConstantParameter>,
}

#[repr(C)]
pub struct TextureBinding {
    pub texture_unit: i32,
    pub sampler_unit: i32,
    pub multisampled: i32,
    pub texture: i32,
}

#[repr(C)]
pub struct Api {
    pub resize: unsafe extern "C" fn(*mut Array<u8>, usize, u32),
    pub texture: unsafe extern "C" fn(*const u8, i32, i32, u8, *mut TextureBinding),
    pub prepare:
        unsafe extern "C" fn(*const ProgramParameters, *const PropertySheet, *mut Array<u8>),
    pub real_device: unsafe extern "C" fn() -> *mut u8,
    pub apply_textures: unsafe extern "C" fn(*const u8, *mut u8, i32) -> *const u8,
    pub apply_samplers: unsafe extern "C" fn(*const u8, *mut u8, i32) -> *const u8,
    pub apply_constant: unsafe extern "C" fn(*mut Applier, *const u8),
    pub shader_apply: *const c_void,
}

const _: () = {
    assert!(std::mem::size_of::<Array<u8>>() == 32);
    assert!(std::mem::size_of::<PropertySheet>() == 160);
    assert!(std::mem::size_of::<ValueParameter>() == 20);
    assert!(std::mem::size_of::<TextureParameter>() == 20);
    assert!(std::mem::size_of::<BufferParameter>() == 16);
    assert!(std::mem::size_of::<ConstantParameter>() == 104);
    assert!(std::mem::size_of::<ProgramParameters>() == 896);
    assert!(std::mem::size_of::<ConstantBuffer>() == 48);
    assert!(std::mem::size_of::<Applier>() == 32);
    assert!(std::mem::offset_of!(PropertySheet, names) == 48);
    assert!(std::mem::offset_of!(PropertySheet, descriptions) == 80);
    assert!(std::mem::offset_of!(PropertySheet, values) == 112);
    assert!(std::mem::offset_of!(ProgramParameters, constants) == 176);
};
