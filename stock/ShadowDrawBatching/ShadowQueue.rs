use std::alloc::{alloc, dealloc, Layout};
use std::{ptr, slice};

#[repr(C)]
pub struct Snapshot {
    vertices: *const u8,
    vertex_bytes: usize,
    stride: usize,
    position: usize,
    normal: usize,
    indices: *const u8,
    index_count: usize,
    index_stride: usize,
    index_bias: i64,
    constants: *const u8,
    constant_bytes: usize,
}
#[repr(C)]
pub struct View {
    vertices: *const u8,
    vertex_bytes: usize,
    indices: *const u32,
    index_count: usize,
    constants: *const u8,
    constant_bytes: usize,
    objects: usize,
}
pub struct Queue {
    vertices: Vec<u8>,
    indices: Vec<u32>,
    constants: Vec<u8>,
    bank_bytes: usize,
    max_objects: usize,
    vertex_budget: usize,
    index_budget: usize,
    state: Option<u64>,
    objects: usize,
}
fn reserve<T>(data: &mut Vec<T>, required: usize, limit: usize) -> bool {
    if required > limit { return false; }
    if required <= data.capacity() { return true; }
    let target = required.checked_next_power_of_two().unwrap_or(limit).min(limit);
    data.try_reserve_exact(target - data.len()).is_ok()
}

#[repr(C)]
pub struct IndexRange { minimum: u32, maximum: u32 }

unsafe fn range<const STEP: usize>(indices: *const u8, count: usize) -> IndexRange {
    let read = |i| if STEP == 2 {
        u16::from_le(ptr::read_unaligned(indices.add(i * STEP).cast::<u16>())) as u32
    } else { u32::from_le(ptr::read_unaligned(indices.add(i * STEP).cast::<u32>())) };
    let (mut minimum, mut maximum) = (u32::MAX, 0);
    let mut i = 0;
    while i + 1 < count {
        let a = read(i);let b = read(i + 1);
        minimum = minimum.min(a.min(b));maximum = maximum.max(a.max(b));i += 2;
    }
    if i < count { let value = read(i);minimum = minimum.min(value);maximum = maximum.max(value); }
    IndexRange { minimum, maximum }
}

#[no_mangle]
pub unsafe extern "C" fn ShadowIndexRange(indices: *const u8, count: usize, stride: usize, output: *mut IndexRange) -> i32 {
    if indices.is_null() || output.is_null() || count == 0 || !matches!(stride, 2 | 4)
        || count.checked_mul(stride).map_or(true, |bytes| bytes > isize::MAX as usize) { return -1; }
    output.write(if stride == 2 {range::<2>(indices,count)} else {range::<4>(indices,count)});
    0
}

unsafe fn append_indices<const STEP: usize>(target: &mut Vec<u32>, source: *const u8, count: usize, bias: i64, base: usize) {
    let start = target.len();let output = target.as_mut_ptr().add(start);
    for i in 0..count {
        let value = if STEP == 2 {u16::from_le(ptr::read_unaligned(source.add(i * STEP).cast::<u16>())) as i64}
                    else {u32::from_le(ptr::read_unaligned(source.add(i * STEP).cast::<u32>())) as i64};
        output.add(i).write((base + (value + bias) as usize) as u32);
    }
    target.set_len(start + count);
}

impl Queue {
    unsafe fn push(&mut self, state: u64, source: &Snapshot) -> Result<bool, ()> {
        if self.objects != 0 && self.state != Some(state) { return Ok(false); }
        if self.objects == self.max_objects { return Ok(false); }
        if source.vertices.is_null() || source.indices.is_null() || source.constants.is_null()
            || source.stride == 0 || source.vertex_bytes == 0 || source.vertex_bytes % source.stride != 0
            || source.vertex_bytes > isize::MAX as usize || source.constant_bytes != self.bank_bytes
            || !matches!(source.index_stride, 2 | 4) || source.index_count == 0 || source.index_count % 3 != 0
            || source.position.checked_add(12).ok_or(())? > source.stride
            || source.normal.checked_add(12).ok_or(())? > source.stride {
            return Err(());
        }
        let count = source.vertex_bytes / source.stride;
        let added = count.checked_mul(28).ok_or(())?;
        let end = self.vertices.len().checked_add(added).ok_or(())?;
        let index_end = self.indices.len().checked_add(source.index_count).ok_or(())?;
        if end > self.vertex_budget || index_end > self.index_budget { return Ok(false); }
        let base = self.vertices.len() / 28;
        if base.checked_add(count).ok_or(())? > u32::MAX as usize { return Err(()); }
        let bytes = source.index_count.checked_mul(source.index_stride).ok_or(())?;
        if bytes > isize::MAX as usize { return Err(()); }
        let extent = if source.index_stride == 2 {range::<2>(source.indices,source.index_count)}
                     else {range::<4>(source.indices,source.index_count)};
        let minimum = i64::from(extent.minimum).checked_add(source.index_bias).ok_or(())?;
        let maximum = i64::from(extent.maximum).checked_add(source.index_bias).ok_or(())?;
        if minimum < 0 || maximum as u64 >= count as u64 { return Err(()); }
        let constant_end = self.constants.len().checked_add(self.bank_bytes).ok_or(())?;
        if !reserve(&mut self.vertices, end, self.vertex_budget)
            || !reserve(&mut self.indices, index_end, self.index_budget)
            || !reserve(&mut self.constants, constant_end, self.max_objects * self.bank_bytes) {
            return Err(());
        }
        let vertices = slice::from_raw_parts(source.vertices, source.vertex_bytes);
        for vertex in vertices.chunks_exact(source.stride) {
            self.vertices.extend_from_slice(&vertex[source.position..source.position + 12]);
            self.vertices.extend_from_slice(&vertex[source.normal..source.normal + 12]);
            self.vertices.extend_from_slice(&(self.objects as u32).to_le_bytes());
        }
        if source.index_stride == 2 {append_indices::<2>(&mut self.indices,source.indices,source.index_count,source.index_bias,base);}
        else {append_indices::<4>(&mut self.indices,source.indices,source.index_count,source.index_bias,base);}
        self.constants.extend_from_slice(slice::from_raw_parts(source.constants, source.constant_bytes));
        self.state = Some(state);
        self.objects += 1;
        Ok(true)
    }
}

#[no_mangle]
pub unsafe extern "C" fn ShadowQueueCreate(bank_bytes: usize, max_objects: usize, vertex_budget: usize, index_budget: usize) -> *mut Queue {
    if bank_bytes == 0 || bank_bytes % 16 != 0 || bank_bytes > 65536 || max_objects == 0
        || max_objects > 65536 / bank_bytes || vertex_budget < 28 || index_budget < 12 { return ptr::null_mut(); }
    let memory = alloc(Layout::new::<Queue>()).cast::<Queue>();
    if memory.is_null() { return memory; }
    memory.write(Queue { vertices: Vec::new(), indices: Vec::new(), constants: Vec::new(), bank_bytes,
        max_objects, vertex_budget, index_budget: index_budget / 4, state: None, objects: 0 });
    memory
}

// AI-generated comment: Snapshot pointers must stay readable and unchanged during this call; a successful push owns copies.
#[no_mangle]
pub unsafe extern "C" fn ShadowQueuePush(queue: *mut Queue, state: u64, source: *const Snapshot) -> i32 {
    if queue.is_null() || source.is_null() { return -1; }
    match (*queue).push(state, &*source) { Ok(true) => 1, Ok(false) => 0, Err(()) => -1 }
}
#[no_mangle]
pub unsafe extern "C" fn ShadowQueueView(queue: *const Queue, output: *mut View) -> i32 {
    if queue.is_null() || output.is_null() { return -1; }
    let q = &*queue;
    output.write(View { vertices: q.vertices.as_ptr(), vertex_bytes: q.vertices.len(), indices: q.indices.as_ptr(),
        index_count: q.indices.len(), constants: q.constants.as_ptr(), constant_bytes: q.constants.len(), objects: q.objects });
    0
}
#[no_mangle]
pub unsafe extern "C" fn ShadowQueueClear(queue: *mut Queue) {
    if let Some(q) = queue.as_mut() { q.vertices.clear();q.indices.clear();q.constants.clear();q.objects=0;q.state=None; }
}
#[no_mangle]
pub unsafe extern "C" fn ShadowQueueFree(queue: *mut Queue) {
    if !queue.is_null() { ptr::drop_in_place(queue);dealloc(queue.cast(),Layout::new::<Queue>()); }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn index_extrema_match_independent_scalar_reference() {unsafe {
        let mut state=81u32;
        for stride in [2,4] {for count in 1..1025 {
            let mut bytes=vec![0xa5];let mut reference=Vec::new();
            for i in 0..count {
                state=state.wrapping_mul(1664525).wrapping_add(1013904223);
                let value=if i==0 {0} else if i==count-1 {u32::MAX} else {state};
                let value=if stride==2 {value&65535} else {value};
                bytes.extend_from_slice(&value.to_le_bytes()[..stride]);reference.push(value);
            }
            let mut result=IndexRange{minimum:99,maximum:99};
            assert_eq!(ShadowIndexRange(bytes.as_ptr().add(1),count,stride,&mut result),0);
            assert_eq!(result.minimum,*reference.iter().min().unwrap());assert_eq!(result.maximum,*reference.iter().max().unwrap());
        }}
        let mut result=IndexRange{minimum:99,maximum:99};let bytes=[0u8;4];
        for (count,stride) in [(0,4),(1,3),(usize::MAX,4)] {
            assert_eq!(ShadowIndexRange(bytes.as_ptr(),count,stride,&mut result),-1);assert_eq!(result.minimum,99);
        }
    }}
    #[test] fn overflow_and_negative_index_bias_leave_queue_empty() {unsafe {
        let q=ShadowQueueCreate(16,2,168,24);let v=vec![1;72];let i=[0,u32::MAX,1];let b=[2;16];
        let mut s=snapshot(&v,&i,&b);
        for bias in [i64::MAX,-1,i64::MIN] {s.index_bias=bias;assert_eq!(ShadowQueuePush(q,1,&s),-1);assert_eq!((*q).objects,0);}
        ShadowQueueFree(q);
    }}
    fn snapshot<'a>(vertices: &'a [u8], indices: &'a [u32], bank: &'a [u8]) -> Snapshot {
        Snapshot { vertices: vertices.as_ptr(), vertex_bytes: vertices.len(), stride:24, position:0,normal:12,
            indices:indices.as_ptr().cast(),index_count:indices.len(),index_stride:4,index_bias:0,constants:bank.as_ptr(),constant_bytes:bank.len() }
    }
    #[test] fn owns_sources_after_return() { unsafe {
        let q=ShadowQueueCreate(16,2,168,24);assert!(!q.is_null());
        let mut v=vec![7;72];let mut i=vec![0,1,2];let mut b=vec![9;16];
        assert_eq!(ShadowQueuePush(q,1,&snapshot(&v,&i,&b)),1);
        v.fill(0);i.fill(99);b.fill(0);drop(v);drop(i);drop(b);
        assert_eq!((*q).constants,vec![9;16]);assert_eq!(&(&(*q).vertices)[..24],&[7;24]);assert_eq!((*q).indices,vec![0,1,2]);
        ShadowQueueFree(q);
    }}
    #[test] fn boundaries_leave_pending_commands_intact() { unsafe {
        let q=ShadowQueueCreate(16,2,168,24);let v=vec![1;72];let i=[0,1,2];let b=[2;16];let s=snapshot(&v,&i,&b);
        assert_eq!(ShadowQueuePush(q,1,&s),1);assert_eq!(ShadowQueuePush(q,2,&s),0);assert_eq!((*q).objects,1);
        assert_eq!(ShadowQueuePush(q,1,&s),1);assert_eq!(ShadowQueuePush(q,1,&s),0);assert_eq!((*q).indices,vec![0,1,2,3,4,5]);ShadowQueueFree(q);
    }}
    #[test] fn invalid_index_does_not_append_partial_data() { unsafe {
        let q=ShadowQueueCreate(16,2,168,24);let v=vec![1;72];let i=[0,1,3];let b=[2;16];
        assert_eq!(ShadowQueuePush(q,1,&snapshot(&v,&i,&b)),-1);assert_eq!((*q).objects,0);assert!((*q).vertices.is_empty());ShadowQueueFree(q);
    }}
    #[test] fn clear_reuses_storage_and_resets_ids() { unsafe {
        let q=ShadowQueueCreate(16,2,168,24);let v=vec![1;72];let i=[0,1,2];let b=[2;16];let s=snapshot(&v,&i,&b);
        assert_eq!(ShadowQueuePush(q,1,&s),1);let cap=(*q).vertices.capacity();ShadowQueueClear(q);
        assert_eq!(ShadowQueuePush(q,2,&s),1);assert_eq!((*q).vertices.capacity(),cap);assert_eq!(&(&(*q).vertices)[24..28],&[0;4]);ShadowQueueFree(q);
    }}
    #[test] fn oversized_mesh_requires_flush_without_writes() { unsafe {
        let q=ShadowQueueCreate(16,2,28,12);let v=vec![1;72];let i=[0,1,2];let b=[2;16];
        assert_eq!(ShadowQueuePush(q,1,&snapshot(&v,&i,&b)),0);assert!((*q).vertices.is_empty());ShadowQueueFree(q);
    }}
}
