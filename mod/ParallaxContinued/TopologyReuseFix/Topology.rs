//! Reusable midpoint expression graph for Parallax's reviewed subdivision rules.
#[path="Certificate.rs"]
mod certificate;
use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};
use std::sync::{
    atomic::{AtomicU64, AtomicUsize, Ordering},
    Arc, Mutex, RwLock,
};

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Triangle {
    pub positions: [[f32; 3]; 3],
    pub normals: [[f32; 3]; 3],
    pub colors: [[f32; 4]; 3],
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Vertex {
    pub position: [f32; 3],
    pub normal: [f32; 3],
    pub color: [f32; 4],
}
impl Vertex {
    fn midpoint(a: Self, b: Self) -> Self {
        Self {
            position: std::array::from_fn(|i| (a.position[i] + b.position[i]) * 0.5),
            normal: std::array::from_fn(|i| (a.normal[i] + b.normal[i]) * 0.5),
            color: std::array::from_fn(|i| (a.color[i] + b.color[i]) * 0.5),
        }
    }
    fn bits(&self) -> [u32; 10] {
        std::array::from_fn(|i| {
            if i < 3 {
                self.position[i].to_bits()
            } else if i < 6 {
                self.normal[i - 3].to_bits()
            } else {
                self.color[i - 6].to_bits()
            }
        })
    }
    fn finite(&self) -> bool {
        self.position
            .iter()
            .chain(self.normal.iter())
            .chain(self.color.iter())
            .all(|v| v.is_finite())
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Plane {
    pub normal: [f32; 3],
    pub distance: f32,
}
#[derive(Clone)]
struct Query {
    epoch: u64,
    target: [f32; 3],
    thresholds: Vec<f32>,
    planes: [Plane; 6],
    bypass_squared: f32,
    depth: u32,
}
struct DistanceSlot {
    position: [f32; 3],
    state: AtomicU64,
}
struct Node {
    vertex: Vertex,
    distance: Arc<DistanceSlot>,
    output_index: u32,
}
#[derive(Default)]
struct GeometryHasher(u64);
impl Hasher for GeometryHasher {
    fn finish(&self) -> u64 {
        let v = self.0 ^ (self.0 >> 32);
        let v = v.wrapping_mul(0xd6e8feb86659fd93);
        v ^ (v >> 32)
    }
    fn write(&mut self, bytes: &[u8]) {
        for chunk in bytes.chunks(8) {
            let mut word = [0u8; 8];
            word[..chunk.len()].copy_from_slice(chunk);
            self.write_u64(u64::from_le_bytes(word));
        }
    }
    fn write_u32(&mut self, v: u32) {
        self.write_u64(v as u64);
    }
    fn write_usize(&mut self, v: usize) {
        self.write_u64(v as u64);
    }
    fn write_u64(&mut self, v: u64) {
        self.0 = (self.0.rotate_left(5) ^ v).wrapping_mul(0x517cc1b727220a95);
    }
}
type Map<K, V> = HashMap<K, V, BuildHasherDefault<GeometryHasher>>;
struct TriangleNode {
    vertices: [u32; 3],
    midpoints: [u32; 3],
    children: [u32; 4],
}
impl TriangleNode {
    fn new(vertices: [u32; 3]) -> Self {
        Self {
            vertices,
            midpoints: [u32::MAX; 3],
            children: [u32::MAX; 4],
        }
    }
}
struct Root {
    nodes: Vec<Node>,
    triangles: Vec<TriangleNode>,
    midpoints: Map<(u32, u32), u32>,
    leaves: Vec<[u32; 3]>,
    epoch: u64,
    stats: Stats,
}
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Stats {
    pub visited: u64,
    pub distances: u64,
    pub midpoints: u64,
    pub leaves: u64,
    pub level_requests: u64,
}
#[repr(C)]
#[derive(Default)]
pub struct Output {
    pub vertices: *const Vertex,
    pub indices: *const u8,
    pub vertex_count: usize,
    pub index_count: usize,
    pub index_width: u32,
    pub revision: u64,
    pub stats: Stats,
}
#[repr(C)]
pub struct Destination {
    pub positions: *mut [f32; 3],
    pub normals: *mut [f32; 3],
    pub colors: *mut [f32; 4],
    pub indices: *mut u8,
    pub vertex_capacity: usize,
    pub index_capacity: usize,
}
#[derive(Clone, Copy)]
struct DirectPlan {
    epoch: u64,
    geometry_version: u64,
    revision: u64,
    vertices: usize,
    indices: usize,
    stats: Stats,
    exact: bool,
}
struct RootView(*const Root);
// AI-generated comment: Root addresses stay fixed for the context lifetime.
// Dereferencing a view requires exclusive query access and a matching geometry version.
unsafe impl Send for RootView {}
#[derive(Default)]
struct Mesh {
    vertices: Vec<Vertex>,
    indices: Vec<u32>,
    short_indices: Vec<u16>,
    revision: u64,
    stats: Stats,
    scratch_vertices: Vec<Vertex>,
    scratch_indices: Vec<u32>,
    scratch_lookup: Map<[u32; 3], u32>,
    direct_plan: Option<DirectPlan>,
    direct_revision: u64,
    direct_roots: Vec<RootView>,
}
pub struct Context {
    roots: Vec<Mutex<Root>>,
    query: RwLock<Option<Arc<Query>>>,
    mesh: Mutex<Mesh>,
    remaining: AtomicUsize,
    output_limit: usize,
    positions: Mutex<Map<[u32; 3], Arc<DistanceSlot>>>,
    geometry_version: AtomicU64,
    certificate: Mutex<Option<certificate::Certificate>>,
}
fn distance(a: [f32; 3], b: [f32; 3]) -> f32 {
    let d = std::array::from_fn::<_, 3, _>(|i| a[i] - b[i]);
    (d[0] * d[0] + d[1] * d[1]) + d[2] * d[2]
}
fn position_key(p: [f32; 3]) -> [u32; 3] {
    p.map(|v| if v == 0. { 0 } else { v.to_bits() })
}
fn distance_slot(
    positions: &mut Map<[u32; 3], Arc<DistanceSlot>>,
    p: [f32; 3],
) -> Arc<DistanceSlot> {
    positions
        .entry(position_key(p))
        .or_insert_with(|| {
            Arc::new(DistanceSlot {
                position: p,
                state: AtomicU64::new(0),
            })
        })
        .clone()
}
impl Root {
    fn new(t: Triangle, positions: &mut Map<[u32; 3], Arc<DistanceSlot>>) -> Option<Self> {
        let nodes: Vec<_> = (0..3)
            .map(|i| Node {
                vertex: Vertex {
                    position: t.positions[i],
                    normal: t.normals[i],
                    color: t.colors[i],
                },
                distance: distance_slot(positions, t.positions[i]),
                output_index: 0,
            })
            .collect();
        if nodes.iter().any(|n| !n.vertex.finite()) {
            return None;
        }
        Some(Self {
            nodes,
            triangles: vec![TriangleNode::new([0, 1, 2])],
            midpoints: Map::default(),
            leaves: Vec::new(),
            epoch: 0,
            stats: Stats::default(),
        })
    }
    fn midpoint(
        &mut self,
        a: u32,
        b: u32,
        budget: &AtomicUsize,
        positions: &Mutex<Map<[u32; 3], Arc<DistanceSlot>>>,
    ) -> Result<u32, ()> {
        let key = (a.min(b), a.max(b));
        if let Some(&id) = self.midpoints.get(&key) {
            return Ok(id);
        }
        let vertex = Vertex::midpoint(self.nodes[a as usize].vertex, self.nodes[b as usize].vertex);
        if !vertex.finite() {
            return Err(());
        }
        self.nodes.try_reserve(1).map_err(|_| ())?;
        self.midpoints.try_reserve(1).map_err(|_| ())?;
        budget
            .try_update(Ordering::Relaxed, Ordering::Relaxed, |v| v.checked_sub(1))
            .map_err(|_| ())?;
        let id = u32::try_from(self.nodes.len()).map_err(|_| ())?;
        let distance = distance_slot(&mut *positions.lock().map_err(|_| ())?, vertex.position);
        self.nodes.push(Node { vertex, distance, output_index: 0 });
        self.midpoints.insert(key, id);
        self.stats.midpoints += 1;
        Ok(id)
    }
    fn edge(
        &mut self,
        node: usize,
        edge: usize,
        a: u32,
        b: u32,
        budget: &AtomicUsize,
        positions: &Mutex<Map<[u32; 3], Arc<DistanceSlot>>>,
    ) -> Result<u32, ()> {
        let existing = self.triangles[node].midpoints[edge];
        if existing != u32::MAX {
            return Ok(existing);
        }
        let value = self.midpoint(a, b, budget, positions)?;
        self.triangles[node].midpoints[edge] = value;
        Ok(value)
    }
    fn level(&mut self, id: u32, q: &Query) -> u32 {
        let slot = &self.nodes[id as usize].distance;
        let state = slot.state.load(Ordering::Relaxed);
        if state >> 8 == q.epoch {
            return (state & 255) as u32;
        }
        let d = distance(slot.position, q.target);
        let mut level = 0;
        while level < q.depth && d <= q.thresholds[level as usize] {
            level += 1;
        }
        slot.state
            .store(q.epoch << 8 | level as u64, Ordering::Relaxed);
        self.stats.distances += 1;
        level
    }
    fn leaf(&mut self, t: [u32; 3], limit: usize) -> Result<(), ()> {
        if self.leaves.len() >= limit {
            return Err(());
        }
        self.leaves.try_reserve(1).map_err(|_| ())?;
        self.leaves.push(t);
        Ok(())
    }
    fn visit(
        &mut self,
        node: usize,
        depth: u32,
        q: &Query,
        budget: &AtomicUsize,
        limit: usize,
        positions: &Mutex<Map<[u32; 3], Arc<DistanceSlot>>>,
    ) -> Result<(), ()> {
        let t = self.triangles[node].vertices;
        self.stats.visited += 1;
        if depth == q.depth {
            return self.leaf(t, limit);
        }
        self.stats.level_requests += 3;
        let [a, b, c] = t;
        let l = t.map(|v| self.level(v, q));
        let two = (l[0] == l[1] && l[2] > l[1] && depth == l[1])
            || (l[1] == l[2] && l[0] > l[2] && depth == l[2])
            || (l[2] == l[0] && l[1] > l[0] && depth == l[0]);
        if two {
            return self.leaf(t, limit);
        }
        let one = (l[0] == l[1] && l[2] < l[1] && depth == l[2])
            || (l[1] == l[2] && l[0] < l[2] && depth == l[0])
            || (l[2] == l[0] && l[1] < l[0] && depth == l[1]);
        if one {
            let pair = if l[0] < l[1] {
                let m = self.edge(node, 1, b, c, budget, positions)?;
                Some([[c, a, m], [a, b, m]])
            } else if l[1] < l[2] {
                let m = self.edge(node, 2, a, c, budget, positions)?;
                Some([[a, b, m], [b, c, m]])
            } else if l[2] < l[0] {
                let m = self.edge(node, 0, a, b, budget, positions)?;
                Some([[c, a, m], [b, c, m]])
            } else {
                None
            };
            if let Some(pair) = pair {
                self.leaf(pair[0], limit)?;
                return self.leaf(pair[1], limit);
            }
        } else if l.iter().all(|&v| v == depth) {
            return self.leaf(t, limit);
        }
        if l.iter().any(|&v| depth as i64 - v as i64 > 1) {
            return self.leaf(t, limit);
        }
        if self.triangles[node].children[0] == u32::MAX {
            let ac = self.edge(node, 2, a, c, budget, positions)?;
            let bc = self.edge(node, 1, c, b, budget, positions)?;
            let ab = self.edge(node, 0, a, b, budget, positions)?;
            self.triangles.try_reserve(4).map_err(|_| ())?;
            budget
                .try_update(Ordering::Relaxed, Ordering::Relaxed, |v| v.checked_sub(4))
                .map_err(|_| ())?;
            let start = u32::try_from(self.triangles.len()).map_err(|_| ())?;
            self.triangles.extend(
                [[ac, bc, c], [a, ab, ac], [ab, b, bc], [ab, bc, ac]].map(TriangleNode::new),
            );
            self.triangles[node].children = [start, start + 1, start + 2, start + 3];
        }
        for child in self.triangles[node].children {
            self.visit(child as usize, depth + 1, q, budget, limit, positions)?;
        }
        Ok(())
    }
    fn evaluate(
        &mut self,
        q: &Query,
        budget: &AtomicUsize,
        limit: usize,
        positions: &Mutex<Map<[u32; 3], Arc<DistanceSlot>>>,
    ) -> Result<(), ()> {
        self.stats = Stats::default();
        self.leaves.clear();
        self.epoch = 0;
        let p: [_; 3] = std::array::from_fn(|i| self.nodes[i].vertex.position);
        let centre = std::array::from_fn(|i| ((p[0][i] + p[1][i]) + p[2][i]) * 0.333);
        let visible = distance(q.target, centre) < q.bypass_squared
            || q.planes.iter().all(|plane| {
                p.iter().any(|v| {
                    ((plane.normal[0] * v[0] + plane.normal[1] * v[1]) + plane.normal[2] * v[2])
                        + plane.distance
                        > 0.
                })
            });
        if visible {
            self.visit(0, 0, q, budget, limit, positions)?;
        } else {
            self.leaf([0, 1, 2], limit)?;
        }
        self.stats.leaves = self.leaves.len() as u64;
        self.epoch = q.epoch;
        Ok(())
    }
}
fn protected(work: impl FnOnce() -> Result<(), ()>) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(work)) {
        Ok(Ok(())) => 1,
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn koe_topology_create(
    triangles: *const Triangle,
    count: usize,
    max_nodes: usize,
    output_limit: usize,
) -> *mut Context {
    if triangles.is_null()
        || count == 0
        || count > max_nodes / 4
        || max_nodes > u32::MAX as usize
        || output_limit == 0
    {
        return std::ptr::null_mut();
    }
    std::panic::catch_unwind(|| {
        let mut positions = Map::default();
        let roots: Option<Vec<_>> = std::slice::from_raw_parts(triangles, count)
            .iter()
            .map(|&t| Root::new(t, &mut positions).map(Mutex::new))
            .collect();
        roots
            .map(|roots| {
                Box::into_raw(Box::new(Context {
                    roots,
                    query: RwLock::new(None),
                    mesh: Mutex::new(Mesh::default()),
                    remaining: AtomicUsize::new(max_nodes - count * 4),
                    output_limit,
                    positions: Mutex::new(positions),
                    geometry_version: AtomicU64::new(0),
                    certificate: Mutex::new(None),
                }))
            })
            .unwrap_or(std::ptr::null_mut())
    })
    .unwrap_or(std::ptr::null_mut())
}
#[no_mangle]
pub unsafe extern "C" fn koe_topology_prepare(
    context: *mut Context,
    target: *const f32,
    depth: u32,
    thresholds: *const f32,
    threshold_count: usize,
    planes: *const Plane,
    bypass_squared: f32,
) -> i32 {
    protected(|| {
        if context.is_null()
            || target.is_null()
            || planes.is_null()
            || depth > 31
            || threshold_count < depth as usize
            || (depth != 0 && thresholds.is_null())
            || !bypass_squared.is_finite()
            || bypass_squared < 0.
        {
            return Err(());
        }
        let context = &*context;
        let target: [f32; 3] = std::slice::from_raw_parts(target, 3)
            .try_into()
            .map_err(|_| ())?;
        let planes: [Plane; 6] = std::slice::from_raw_parts(planes, 6)
            .try_into()
            .map_err(|_| ())?;
        let thresholds = if depth == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(thresholds, depth as usize).to_vec()
        };
        if target
            .iter()
            .chain(thresholds.iter())
            .any(|v| !v.is_finite())
            || planes.iter().any(|p| {
                p.normal
                    .iter()
                    .chain(std::iter::once(&p.distance))
                    .any(|v| !v.is_finite())
            })
        {
            return Err(());
        }
        let mut slot = context.query.write().map_err(|_| ())?;
        *context.certificate.lock().map_err(|_| ())?=None;
        let epoch = slot.as_ref().map_or(1, |q| q.epoch + 1);
        if epoch > u64::MAX >> 8 {
            return Err(());
        }
        *slot = Some(Arc::new(Query {
            epoch,
            target,
            depth,
            thresholds,
            planes,
            bypass_squared,
        }));
        Ok(())
    })
}
#[no_mangle]
pub unsafe extern "C" fn koe_topology_root(context: *mut Context, index: usize) -> i32 {
    protected(|| {
        let context = context.as_ref().ok_or(())?;
        let query_guard = context.query.read().map_err(|_| ())?;
        let q = query_guard.clone().ok_or(())?;
        context.geometry_version.fetch_add(1, Ordering::Relaxed);
        context
            .roots
            .get(index)
            .ok_or(())?
            .lock()
            .map_err(|_| ())?
            .evaluate(
                &q,
                &context.remaining,
                context.output_limit,
                &context.positions,
            )
    })
}
#[no_mangle]
pub unsafe extern "C" fn koe_topology_range(
    context: *mut Context,
    begin: usize,
    end: usize,
) -> i32 {
    protected(|| {
        let context = context.as_ref().ok_or(())?;
        if begin > end || end > context.roots.len() {
            return Err(());
        }
        let query_guard = context.query.read().map_err(|_| ())?;
        let query = query_guard.clone().ok_or(())?;
        context.geometry_version.fetch_add(1, Ordering::Relaxed);
        for root in &context.roots[begin..end] {
            root.lock().map_err(|_| ())?.evaluate(
                &query,
                &context.remaining,
                context.output_limit,
                &context.positions,
            )?;
        }
        Ok(())
    })
}
#[no_mangle]
pub unsafe extern "C" fn koe_topology_finish(context: *mut Context, result: *mut Output) -> i32 {
    protected(|| {
        let context = context.as_ref().ok_or(())?;
        let result = result.as_mut().ok_or(())?;
        let q = context.query.read().map_err(|_| ())?.clone().ok_or(())?;
        let mut mesh = context.mesh.lock().map_err(|_| ())?;
        mesh.direct_plan = None;
        let mut vertices = std::mem::take(&mut mesh.scratch_vertices);
        let mut indices = std::mem::take(&mut mesh.scratch_indices);
        let mut lookup = std::mem::take(&mut mesh.scratch_lookup);
        vertices.clear();
        indices.clear();
        lookup.clear();
        let mut stats = Stats::default();
        for root in &context.roots {
            let root = root.lock().map_err(|_| ())?;
            if root.epoch != q.epoch {
                return Err(());
            }
            stats.visited += root.stats.visited;
            stats.distances += root.stats.distances;
            stats.midpoints += root.stats.midpoints;
            stats.leaves += root.stats.leaves;
            stats.level_requests += root.stats.level_requests;
            if indices.len() / 3 + root.leaves.len() > context.output_limit {
                return Err(());
            }
            indices.try_reserve(root.leaves.len() * 3).map_err(|_| ())?;
            for triangle in &root.leaves {
                for id in triangle {
                    let vertex = root.nodes[*id as usize].vertex;
                    let key = position_key(vertex.position);
                    let index = if let Some(&index) = lookup.get(&key) {
                        index
                    } else {
                        vertices.try_reserve(1).map_err(|_| ())?;
                        lookup.try_reserve(1).map_err(|_| ())?;
                        let index = u32::try_from(vertices.len()).map_err(|_| ())?;
                        vertices.push(vertex);
                        lookup.insert(key, index);
                        index
                    };
                    indices.push(index);
                }
            }
        }
        let unchanged = mesh.indices == indices
            && mesh.vertices.len() == vertices.len()
            && mesh
                .vertices
                .iter()
                .zip(&vertices)
                .all(|(a, b)| a.bits() == b.bits());
        if !unchanged {
            mesh.revision += 1;
            std::mem::swap(&mut mesh.vertices, &mut vertices);
            std::mem::swap(&mut mesh.indices, &mut indices);
            mesh.short_indices.clear();
            if mesh.vertices.len() <= 65536 {
                let count = mesh.indices.len();
                mesh.short_indices.try_reserve(count).map_err(|_| ())?;
                for i in 0..count {
                    let v = mesh.indices[i] as u16;
                    mesh.short_indices.push(v);
                }
            }
        }
        mesh.scratch_vertices = vertices;
        mesh.scratch_indices = indices;
        mesh.scratch_lookup = lookup;
        mesh.stats = stats;
        let short = mesh.vertices.len() <= 65536;
        *result = Output {
            vertices: mesh.vertices.as_ptr(),
            indices: if short {
                mesh.short_indices.as_ptr().cast()
            } else {
                mesh.indices.as_ptr().cast()
            },
            vertex_count: mesh.vertices.len(),
            index_count: mesh.indices.len(),
            index_width: if short { 2 } else { 4 },
            revision: mesh.revision,
            stats,
        };
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn koe_topology_direct_abi() -> usize {
    std::mem::size_of::<Destination>()
}

#[no_mangle]
pub unsafe extern "C" fn koe_topology_plan(context: *mut Context, result: *mut Output) -> i32 {
    protected(|| {
        let context = context.as_ref().ok_or(())?;
        let result = result.as_mut().ok_or(())?;
        *result = Output::default();
        let query = context.query.write().map_err(|_| ())?;
        let query = query.as_ref().ok_or(())?;
        let mut mesh = context.mesh.lock().map_err(|_| ())?;
        mesh.direct_plan = None;
        mesh.scratch_lookup.clear();
        mesh.direct_roots.clear();
        mesh.direct_roots.try_reserve(context.roots.len()).map_err(|_| ())?;
        let mut indices = 0usize;
        let mut stats = Stats::default();
        for root in &context.roots {
            let mut root = root.lock().map_err(|_| ())?;
            if root.epoch != query.epoch { return Err(()); }
            mesh.direct_roots.push(RootView(&*root));
            indices = indices.checked_add(root.leaves.len().checked_mul(3).ok_or(())?).ok_or(())?;
            if indices / 3 > context.output_limit { return Err(()); }
            stats.visited += root.stats.visited;
            stats.distances += root.stats.distances;
            stats.midpoints += root.stats.midpoints;
            stats.leaves += root.stats.leaves;
            stats.level_requests += root.stats.level_requests;
            for leaf in 0..root.leaves.len() {
                for id in root.leaves[leaf] {
                    let key = position_key(root.nodes[id as usize].vertex.position);
                    let index = if let Some(&index) = mesh.scratch_lookup.get(&key) {
                        index
                    } else {
                        let index = u32::try_from(mesh.scratch_lookup.len()).map_err(|_| ())?;
                        mesh.scratch_lookup.try_reserve(1).map_err(|_| ())?;
                        mesh.scratch_lookup.insert(key, index);
                        index
                    };
                    root.nodes[id as usize].output_index = index;
                }
            }
        }
        let vertices = mesh.scratch_lookup.len();
        let revision = mesh.direct_revision.checked_add(1).ok_or(())?;
        mesh.direct_revision = revision;
        mesh.direct_plan = Some(DirectPlan { epoch: query.epoch, geometry_version: context.geometry_version.load(Ordering::Relaxed), revision, vertices, indices, stats, exact: true });
        *result = Output {
            vertex_count: vertices, index_count: indices,
            index_width: if vertices <= 65536 { 2 } else { 4 },
            revision, stats, ..Output::default()
        };
        Ok(())
    })
}

fn writable_range(pointer: usize, count: usize, stride: usize, alignment: usize) -> Result<(usize,usize),()> {
    if count == 0 { return Ok((0,0)); }
    let bytes = count.checked_mul(stride).ok_or(())?;
    if pointer == 0 || pointer % alignment != 0 || bytes > isize::MAX as usize { return Err(()); }
    Ok((pointer, pointer.checked_add(bytes).ok_or(())?))
}

#[no_mangle]
pub unsafe extern "C" fn koe_topology_bounds(context: *mut Context, result: *mut Output) -> i32 {
    let mut exact = false;
    let status = protected(|| {
        let context = context.as_ref().ok_or(())?;
        let result = result.as_mut().ok_or(())?;
        *result = Output::default();
        let query = context.query.write().map_err(|_| ())?;
        let query = query.as_ref().ok_or(())?;
        let mut mesh = context.mesh.lock().map_err(|_| ())?;
        mesh.direct_plan = None;
        mesh.direct_roots.clear();
        mesh.direct_roots.try_reserve(context.roots.len()).map_err(|_| ())?;
        let mut indices = 0usize;
        let mut stats = Stats::default();
        for root in &context.roots {
            let root = root.lock().map_err(|_| ())?;
            if root.epoch != query.epoch { return Err(()); }
            indices = indices.checked_add(root.leaves.len().checked_mul(3).ok_or(())?).ok_or(())?;
            if indices / 3 > context.output_limit { return Err(()); }
            mesh.direct_roots.push(RootView(&*root));
            stats.visited += root.stats.visited; stats.distances += root.stats.distances;
            stats.midpoints += root.stats.midpoints; stats.leaves += root.stats.leaves;
            stats.level_requests += root.stats.level_requests;
        }
        if indices > u16::MAX as usize + 1 {
            exact = true;
            return Ok(());
        }
        let revision = mesh.direct_revision.checked_add(1).ok_or(())?;
        mesh.direct_revision = revision;
        mesh.direct_plan = Some(DirectPlan { epoch: query.epoch, geometry_version: context.geometry_version.load(Ordering::Relaxed), revision, vertices: indices, indices, stats, exact: false });
        *result = Output { vertex_count: indices, index_count: indices, index_width: 2, revision, stats, ..Output::default() };
        Ok(())
    });
    if status == 1 && exact { koe_topology_plan(context,result) } else { status }
}

// AI-generated comment: Unity owns these buffers and keeps the job dependency alive.
// No destination pointer is retained by Rust after this call returns.
#[no_mangle]
pub unsafe extern "C" fn koe_topology_write(context: *mut Context, revision: u64, destination: *const Destination) -> i32 {
    write_output(context,revision,destination,std::ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn koe_topology_write_mesh(context: *mut Context, revision: u64, destination: *const Destination, result: *mut Output) -> i32 {
    if result.is_null() { return 0; }
    write_output(context,revision,destination,result)
}

unsafe fn write_output(context: *mut Context, revision: u64, destination: *const Destination, result: *mut Output) -> i32 {
    protected(|| {
        let context = context.as_ref().ok_or(())?;
        let destination = destination.as_ref().ok_or(())?;
        let query = context.query.write().map_err(|_| ())?;
        let query = query.as_ref().ok_or(())?;
        let mut mesh = context.mesh.lock().map_err(|_| ())?;
        let plan = mesh.direct_plan.ok_or(())?;
        if !plan.exact && result.is_null() { return Err(()); }
        if plan.epoch != query.epoch || revision != plan.revision
            || plan.geometry_version != context.geometry_version.load(Ordering::Relaxed)
            || destination.vertex_capacity < plan.vertices || destination.index_capacity < plan.indices {
            return Err(());
        }
        let short = plan.vertices <= 65536;
        let ranges = [
            writable_range(destination.positions as usize, plan.vertices, 12, 4)?,
            writable_range(destination.normals as usize, plan.vertices, 12, 4)?,
            writable_range(destination.colors as usize, plan.vertices, 16, 4)?,
            writable_range(destination.indices as usize, plan.indices, if short { 2 } else { 4 }, if short { 2 } else { 4 })?,
        ];
        for i in 0..ranges.len() {
            for j in 0..i {
                if ranges[i].0 < ranges[j].1 && ranges[j].0 < ranges[i].1 { return Err(()); }
            }
        }
        let mut written_vertices = 0usize;
        let mut written_indices = 0usize;
        let Mesh { direct_roots, scratch_lookup, .. } = &mut *mesh;
        if !plan.exact { scratch_lookup.clear(); }
        for view in direct_roots.iter() {
            let root = &*view.0;
            for triangle in &root.leaves {
                for id in triangle {
                    let node = &root.nodes[*id as usize];
                    let index = if plan.exact { node.output_index as usize } else {
                        let key = position_key(node.vertex.position);
                        if let Some(&index) = scratch_lookup.get(&key) { index as usize } else {
                            scratch_lookup.try_reserve(1).map_err(|_| ())?;
                            scratch_lookup.insert(key,written_vertices as u32);
                            written_vertices
                        }
                    };
                    if index >= plan.vertices || index > written_vertices || written_indices >= plan.indices { return Err(()); }
                    if index == written_vertices {
                        destination.positions.add(index).write(node.vertex.position);
                        destination.normals.add(index).write(node.vertex.normal);
                        destination.colors.add(index).write(node.vertex.color);
                        written_vertices += 1;
                    }
                    if short {
                        destination.indices.cast::<u16>().add(written_indices).write(index as u16);
                    } else {
                        destination.indices.cast::<u32>().add(written_indices).write(index as u32);
                    }
                    written_indices += 1;
                }
            }
        }
        if (plan.exact && written_vertices != plan.vertices) || written_indices != plan.indices { return Err(()); }
        if let Some(result) = result.as_mut() {
            *result = Output { vertex_count: written_vertices, index_count: written_indices,
                index_width: if short { 2 } else { 4 }, revision: plan.revision, stats: plan.stats, ..Output::default() };
        }
        mesh.stats = plan.stats;
        mesh.direct_plan = None;
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn koe_topology_destroy(context: *mut Context) {
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    unsafe fn prepare_direct(c: *mut Context, count: usize, planes: &[Plane;6]) -> Output {
        let target=[0.;3];
        assert_eq!(koe_topology_prepare(c,target.as_ptr(),0,std::ptr::null(),0,planes.as_ptr(),0.),1);
        assert_eq!(koe_topology_range(c,0,count),1);
        let mut plan=Output::default();
        assert_eq!(koe_topology_plan(c,&mut plan),1);
        plan
    }
    #[test]
    fn direct_output_bounds_ownership_and_stale_plans() {
        unsafe {
            let c=koe_topology_create(&triangle(),1,4,1);
            let planes=[Plane{normal:[0.;3],distance:1.};6];
            let plan=prepare_direct(c,1,&planes);
            let mut positions=vec![[42.;3];plan.vertex_count+2];
            let mut normals=positions.clone();
            let mut colors=vec![[42.;4];plan.vertex_count+2];
            let mut indices=vec![0xa5u16;plan.index_count+2];
            let mut destination=Destination{positions:positions.as_mut_ptr().add(1),normals:normals.as_mut_ptr().add(1),colors:colors.as_mut_ptr().add(1),indices:indices.as_mut_ptr().add(1).cast(),vertex_capacity:plan.vertex_count-1,index_capacity:plan.index_count};
            assert_eq!(koe_topology_write(c,plan.revision,&destination),0);
            assert!(positions.iter().all(|p|*p==[42.;3]));
            assert!(indices.iter().all(|p|*p==0xa5));
            destination.vertex_capacity=plan.vertex_count;
            let original_normals=destination.normals;
            destination.normals=destination.positions;
            assert_eq!(koe_topology_write(c,plan.revision,&destination),0);
            destination.normals=(original_normals as *mut u8).add(1).cast();
            assert_eq!(koe_topology_write(c,plan.revision,&destination),0);
            destination.normals=std::ptr::null_mut();
            assert_eq!(koe_topology_write(c,plan.revision,&destination),0);
            destination.normals=original_normals;
            assert_eq!(koe_topology_write(c,plan.revision+1,&destination),0);
            assert_eq!(koe_topology_write(c,plan.revision,&destination),1);
            assert_eq!(koe_topology_write(c,plan.revision,&destination),0);
            assert_eq!(&positions[1..4],&triangle().positions);
            assert_eq!(&normals[1..4],&triangle().normals);
            assert_eq!(&colors[1..4],&triangle().colors);
            assert_eq!(&indices[1..4],&[0,1,2]);
            assert_eq!(positions[0],[42.;3]);assert_eq!(*positions.last().unwrap(),[42.;3]);
            assert_eq!(normals[0],[42.;3]);assert_eq!(*normals.last().unwrap(),[42.;3]);
            assert_eq!(colors[0],[42.;4]);assert_eq!(*colors.last().unwrap(),[42.;4]);
            assert_eq!(indices[0],0xa5);assert_eq!(*indices.last().unwrap(),0xa5);
            let mut next=Output::default();assert_eq!(koe_topology_plan(c,&mut next),1);
            let target=[0.;3];assert_eq!(koe_topology_prepare(c,target.as_ptr(),0,std::ptr::null(),0,planes.as_ptr(),0.),1);
            assert_eq!(koe_topology_write(c,next.revision,&destination),0);
            let mesh=(*c).mesh.lock().unwrap();
            assert!(mesh.vertices.is_empty()&&mesh.indices.is_empty()&&mesh.short_indices.is_empty());
            drop(mesh);koe_topology_destroy(c);
        }
    }
    #[test]
    fn direct_fast_path_preserves_first_attributes_and_rejects_geometry_changes() {
        unsafe {
            let first=triangle();let mut second=first;second.colors=[[0.;4];3];
            let c=koe_topology_create([first,second].as_ptr(),2,8,2);
            let planes=[Plane{normal:[0.;3],distance:1.};6];
            prepare_direct(c,2,&planes);
            let mut plan=Output::default();assert_eq!(koe_topology_bounds(c,&mut plan),1);
            assert_eq!(plan.vertex_count,6);
            let mut p=vec![[42.;3];6];let mut n=p.clone();let mut colors=vec![[42.;4];6];let mut ix=vec![0u16;6];
            let destination=Destination{positions:p.as_mut_ptr(),normals:n.as_mut_ptr(),colors:colors.as_mut_ptr(),indices:ix.as_mut_ptr().cast(),vertex_capacity:6,index_capacity:6};
            let mut output=Output::default();
            assert_eq!(koe_topology_range(c,0,2),1);
            assert_eq!(koe_topology_write_mesh(c,plan.revision,&destination,&mut output),0);
            assert!(p.iter().all(|v|*v==[42.;3]));
            assert_eq!(koe_topology_bounds(c,&mut plan),1);
            assert_eq!(koe_topology_write_mesh(c,plan.revision,&destination,&mut output),1);
            assert_eq!(output.vertex_count,3);assert_eq!(output.index_count,6);
            assert_eq!(&p[..3],&first.positions);assert_eq!(&colors[..3],&first.colors);
            assert_eq!(ix,[0,1,2,0,1,2]);assert_eq!(p[3],[42.;3]);
            let mesh=(*c).mesh.lock().unwrap();assert!(mesh.vertices.is_empty()&&mesh.indices.is_empty());drop(mesh);
            koe_topology_destroy(c);
        }
    }
    #[test]
    fn direct_empty_mesh_accepts_empty_destinations() {
        unsafe {
            let c=koe_topology_create(&triangle(),1,4,1);
            let planes=[Plane{normal:[0.;3],distance:-1.};6];
            prepare_direct(c,1,&planes);
            (&(*c).roots)[0].lock().unwrap().leaves.clear();
            let mut plan=Output::default();assert_eq!(koe_topology_plan(c,&mut plan),1);
            assert_eq!(plan.vertex_count,0);assert_eq!(plan.index_count,0);
            let destination=Destination{positions:std::ptr::null_mut(),normals:std::ptr::null_mut(),colors:std::ptr::null_mut(),indices:std::ptr::null_mut(),vertex_capacity:0,index_capacity:0};
            assert_eq!(koe_topology_write(c,plan.revision,&destination),1);
            koe_topology_destroy(c);
        }
    }
    #[test]
    fn direct_index_width_boundary() {
        unsafe {
            for vertex_count in [65535usize,65536,65537] {
                let count=vertex_count.div_ceil(3);
                let source:Vec<_>=(0..count).map(|i| {
                    let mut t=triangle();
                    t.positions=std::array::from_fn(|j|[((i*3+j)%vertex_count) as f32,0.,0.]);t
                }).collect();
                let c=koe_topology_create(source.as_ptr(),count,count*4,count);
                assert!(!c.is_null());
                let plan=prepare_direct(c,count,&[Plane{normal:[0.;3],distance:1.};6]);
                assert_eq!(plan.vertex_count,vertex_count);
                assert_eq!(plan.index_width,if vertex_count<=65536{2}else{4});
                let mut p=vec![[0.;3];vertex_count];let mut n=p.clone();let mut colors=vec![[0.;4];vertex_count];
                let mut indices=vec![0u32;plan.index_count];
                let destination=Destination{positions:p.as_mut_ptr(),normals:n.as_mut_ptr(),colors:colors.as_mut_ptr(),indices:indices.as_mut_ptr().cast(),vertex_capacity:vertex_count,index_capacity:plan.index_count};
                assert_eq!(koe_topology_write(c,plan.revision,&destination),1);
                for i in 0..plan.index_count {
                    let actual=if plan.index_width==2{*indices.as_ptr().cast::<u16>().add(i) as usize}else{indices[i] as usize};
                    assert_eq!(actual,i%vertex_count);
                }
                koe_topology_destroy(c);
            }
        }
    }
    fn triangle() -> Triangle {
        Triangle {
            positions: [[0., 0., 0.], [1., 0., 0.], [0., 0., 1.]],
            normals: [[0., 1., 0.]; 3],
            colors: [[1., 0.5, 0.25, 1.]; 3],
        }
    }
    unsafe fn evaluate(c: *mut Context, target: [f32; 3], thresholds: &[f32]) -> Output {
        let planes = [Plane {
            normal: [0.; 3],
            distance: 1.,
        }; 6];
        assert_eq!(
            koe_topology_prepare(
                c,
                target.as_ptr(),
                thresholds.len() as u32,
                thresholds.as_ptr(),
                thresholds.len(),
                planes.as_ptr(),
                1e10
            ),
            1
        );
        assert_eq!(koe_topology_root(c, 0), 1);
        let mut out = Output::default();
        assert_eq!(koe_topology_finish(c, &mut out), 1);
        out
    }
    #[test]
    fn stable_query_reuses_midpoints_and_geometry() {
        unsafe {
            let c = koe_topology_create(&triangle(), 1, 10000, 10000);
            assert!(!c.is_null());
            let a = evaluate(c, [0.; 3], &[100.; 4]);
            assert_eq!(a.index_count, 256 * 3);
            assert!(a.stats.midpoints > 0);
            let b = evaluate(c, [0.1, 0., 0.], &[100.; 4]);
            assert_eq!(b.revision, a.revision);
            assert_eq!(b.stats.midpoints, 0);
            assert!(b.stats.distances < b.stats.visited * 3);
            koe_topology_destroy(c);
        }
    }
    #[test]
    fn changing_lod_changes_revision() {
        unsafe {
            let c = koe_topology_create(&triangle(), 1, 10000, 10000);
            let a = evaluate(c, [0.; 3], &[100.; 3]);
            let b = evaluate(c, [1000.; 3], &[100.; 3]);
            assert_eq!(b.index_count, 3);
            assert!(b.revision > a.revision);
            koe_topology_destroy(c);
        }
    }
    #[test]
    fn parallel_roots_preserve_first_vertex_attributes() {
        unsafe {
            let first = triangle();
            let mut second = first;
            second.colors = [[0., 0., 0., 1.]; 3];
            let source = [first, second];
            let c = koe_topology_create(source.as_ptr(), 2, 100000, 100000);
            assert!(!c.is_null());
            let target = [0.; 3];
            let levels = [100.; 5];
            let planes = [Plane {
                normal: [0.; 3],
                distance: 1.,
            }; 6];
            for _ in 0..8 {
                assert_eq!(
                    koe_topology_prepare(
                        c,
                        target.as_ptr(),
                        5,
                        levels.as_ptr(),
                        5,
                        planes.as_ptr(),
                        100.
                    ),
                    1
                );
                let address = c as usize;
                std::thread::scope(|scope| {
                    let a = scope.spawn(move || koe_topology_root(address as *mut Context, 0));
                    let b = scope.spawn(move || koe_topology_root(address as *mut Context, 1));
                    assert_eq!(a.join().unwrap(), 1);
                    assert_eq!(b.join().unwrap(), 1);
                });
                let mut output = Output::default();
                assert_eq!(koe_topology_finish(c, &mut output), 1);
                assert_eq!(output.index_count, 2 * 1024 * 3);
                for vertex in std::slice::from_raw_parts(output.vertices, output.vertex_count) {
                    assert_eq!(vertex.color, first.colors[0]);
                }
            }
            koe_topology_destroy(c);
        }
    }
    #[test]
    fn reject_invalid_and_incomplete_input() {
        unsafe {
            assert!(koe_topology_create(std::ptr::null(), 1, 100, 100).is_null());
            let mut t = triangle();
            t.colors[0][0] = f32::NAN;
            assert!(koe_topology_create(&t, 1, 100, 100).is_null());
            let c = koe_topology_create(&triangle(), 1, 4, 1);
            let p = [Plane {
                normal: [0.; 3],
                distance: 1.,
            }; 6];
            let a = [0.; 3];
            let levels = [100.];
            assert_eq!(
                koe_topology_prepare(c, a.as_ptr(), 1, levels.as_ptr(), 1, p.as_ptr(), 100.),
                1
            );
            assert_eq!(koe_topology_root(c, 0), 0);
            let mut output = Output::default();
            assert_eq!(koe_topology_finish(c, &mut output), 0);
            koe_topology_destroy(c);
        }
    }
}
