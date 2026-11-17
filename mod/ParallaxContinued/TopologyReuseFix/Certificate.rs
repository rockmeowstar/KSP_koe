use super::{distance, protected, Context, Query};
use std::sync::{atomic::Ordering, Arc};

#[derive(Clone, Copy)]
struct Interval { lo: f32, hi: f32 }
impl Interval {
    fn point(value: f32) -> Self { Self { lo: value, hi: value } }
    fn around(value: f32, radius: f64) -> Self {
        if radius == 0. { return Self::point(value); }
        if !radius.is_finite() { return Self { lo: f32::MIN, hi: f32::MAX }; }
        Self { lo: ((value as f64-radius) as f32).next_down(), hi: ((value as f64+radius) as f32).next_up() }
    }
    fn fixed(self) -> bool { self.lo.to_bits() == self.hi.to_bits() }
    fn contains(self, value: f32) -> bool { value >= self.lo && value <= self.hi }
    fn add(self, other: Self) -> Self {
        if self.fixed() && other.fixed() { return Self::point(self.lo+other.lo); }
        Self { lo: (self.lo+other.lo).next_down(), hi: (self.hi+other.hi).next_up() }
    }
    fn sub(self, other: Self) -> Self {
        if self.fixed() && other.fixed() { return Self::point(self.lo-other.lo); }
        Self { lo: (self.lo-other.hi).next_down(), hi: (self.hi-other.lo).next_up() }
    }
    fn mul(self, other: Self) -> Self {
        if self.fixed() && other.fixed() { return Self::point(self.lo*other.lo); }
        let values=[self.lo*other.lo,self.lo*other.hi,self.hi*other.lo,self.hi*other.hi];
        if values.iter().any(|v| v.is_nan()) { return Self { lo: f32::NAN, hi: f32::NAN }; }
        Self { lo: values.into_iter().fold(f32::INFINITY,f32::min).next_down(), hi: values.into_iter().fold(f32::NEG_INFINITY,f32::max).next_up() }
    }
    fn square(self) -> Self {
        if self.fixed() { return Self::point(self.lo*self.lo); }
        let lo=if self.lo<=0. && self.hi>=0. { 0. } else { (self.lo*self.lo).min(self.hi*self.hi).next_down().max(0.) };
        Self { lo, hi: (self.lo*self.lo).max(self.hi*self.hi).next_up() }
    }
}
fn distance_range(position: [f32;3], target: &[Interval;3]) -> Interval {
    let d: [_;3]=std::array::from_fn(|i| Interval::point(position[i]).sub(target[i]).square());
    d[0].add(d[1]).add(d[2])
}
fn plane_value(position: [f32;3], plane: &super::Plane) -> f32 {
    ((plane.normal[0]*position[0]+plane.normal[1]*position[1])+plane.normal[2]*position[2])+plane.distance
}
fn plane_range(position: [f32;3], plane: &[Interval;4]) -> Interval {
    plane[0].mul(Interval::point(position[0])).add(plane[1].mul(Interval::point(position[1])))
        .add(plane[2].mul(Interval::point(position[2]))).add(plane[3])
}
struct LodWitness { position: [f32;3], level: usize }
struct BypassWitness { centre: [f32;3], below: bool }
struct PlaneWitness { position: [f32;3], plane: usize, positive: bool }

pub(super) struct Certificate {
    anchor: Arc<Query>,
    geometry_version: u64,
    target: [Interval;3],
    planes: [[Interval;4];6],
    lod: Vec<LodWitness>,
    bypass: Vec<BypassWitness>,
    visibility: Vec<PlaneWitness>,
    point_proof: bool,
}
impl Certificate {
    fn exact(anchor: Arc<Query>, geometry_version: u64) -> Self {
        Self { target: anchor.target.map(Interval::point),
            planes: std::array::from_fn(|i| std::array::from_fn(|j| Interval::point(if j<3 { anchor.planes[i].normal[j] } else { anchor.planes[i].distance }))),
            anchor, geometry_version, lod:Vec::new(),bypass:Vec::new(),visibility:Vec::new(),point_proof:false }
    }
}
fn radius_for_distance(squared: f32, threshold: f32) -> Option<f64> {
    if !squared.is_finite() { return None; }
    if threshold < 0. { return Some(f64::INFINITY); }
    Some(((squared as f64).sqrt()-(threshold as f64).sqrt()).abs()/8.)
}

// AI-generated comment: Radii only propose a region. Outward-rounded float
// intervals must prove every relevant branch before that region is accepted.
#[no_mangle]
pub unsafe extern "C" fn koe_topology_seal(context: *mut Context) -> i32 {
    seal(context,true)
}
#[no_mangle]
pub unsafe extern "C" fn koe_topology_seal_fast(context: *mut Context) -> i32 {
    seal(context,false)
}
unsafe fn seal(context: *mut Context, build_region: bool) -> i32 {
    protected(|| {
        let context=context.as_ref().ok_or(())?;
        let query_guard=context.query.write().map_err(|_| ())?;
        let query=query_guard.as_ref().ok_or(())?.clone();
        let version=context.geometry_version.load(Ordering::Relaxed);
        let mut destination=context.certificate.lock().map_err(|_| ())?;
        *destination=None;
        let mut certificate=Certificate::exact(query.clone(),version);
        let mut target_radius=f64::INFINITY;
        let mut normal_radius=f64::INFINITY;
        let mut offset_radius=f64::INFINITY;
        let mut lod=Vec::new();let mut bypass=Vec::new();let mut planes=Vec::new();
        let mut possible=true;let mut logical_valid=true;
        for root in &context.roots {
            let root=root.lock().map_err(|_| ())?;
            if root.epoch!=query.epoch { return Err(()); }
            if query.depth==0 { continue; }
            let vertices: [_;3]=std::array::from_fn(|i| root.nodes[i].vertex.position);
            if vertices.iter().all(|&p| distance(p,query.target)>query.thresholds[0]) {
                for node in root.nodes.iter().take(3) { node.distance.state.store(query.epoch<<8,Ordering::Relaxed); }
                continue;
            }
            let centre=std::array::from_fn(|i| ((vertices[0][i]+vertices[1][i])+vertices[2][i])*0.333);
            let squared=distance(query.target,centre);
            let below=squared<query.bypass_squared;
            let values: [[f32;3];6]=std::array::from_fn(|i| vertices.map(|v| plane_value(v,&query.planes[i])));
            if values.iter().flatten().any(|v| !v.is_finite()) || !squared.is_finite() { possible=false; }
            if !below&&values.iter().flatten().any(|v| !v.is_finite()) { logical_valid=false; }
            if below {
                bypass.push(BypassWitness { centre,below:true });
            } else if values.iter().all(|v| v.iter().any(|&x| x>0.)) {
                for i in 0..6 {
                    let best=(0..3).max_by(|&a,&b| values[i][a].total_cmp(&values[i][b])).unwrap();
                    planes.push(PlaneWitness { position:vertices[best],plane:i,positive:true });
                }
            } else {
                bypass.push(BypassWitness { centre,below:false });
                let best=(0..6).filter(|&i| values[i].iter().all(|&v| v<=0.))
                    .min_by(|&a,&b| values[a].into_iter().fold(f32::NEG_INFINITY,f32::max).total_cmp(&values[b].into_iter().fold(f32::NEG_INFINITY,f32::max)));
                if let Some(i)=best { for position in vertices { planes.push(PlaneWitness { position,plane:i,positive:false }); } }
                else { possible=false;logical_valid=false; }
            }
        }
        let positions=context.positions.lock().map_err(|_| ())?;
        for slot in positions.values() {
            let state=slot.state.load(Ordering::Relaxed);
            if state>>8!=query.epoch { continue; }
            let level=(state&255) as usize;
            lod.push(LodWitness { position:slot.position,level });
        }
        drop(positions);
        certificate.point_proof=logical_valid;
        if !build_region {
            certificate.lod=lod;certificate.bypass=bypass;certificate.visibility=planes;
            *destination=Some(certificate);
            return Ok(());
        }
        for witness in &lod {
            let squared=distance(witness.position,query.target);
            for i in 0..(witness.level+1).min(query.depth as usize) {
                if let Some(radius)=radius_for_distance(squared,query.thresholds[i]) { target_radius=target_radius.min(radius); }
                else { possible=false; }
            }
        }
        for witness in &bypass {
            if let Some(radius)=radius_for_distance(distance(witness.centre,query.target),query.bypass_squared) { target_radius=target_radius.min(radius); }
            else { possible=false; }
        }
        for witness in &planes {
            let margin=plane_value(witness.position,&query.planes[witness.plane]).abs() as f64;
            let magnitude=witness.position.iter().map(|v| (*v as f64).abs()).sum::<f64>();
            normal_radius=normal_radius.min(margin/(8.*(magnitude+1.)));
            offset_radius=offset_radius.min(margin/8.);
        }
        if possible {
            let target=query.target.map(|v| Interval::around(v,target_radius));
            let coefficients=std::array::from_fn::<_,6,_>(|i| std::array::from_fn(|j| Interval::around(if j<3 {query.planes[i].normal[j]} else {query.planes[i].distance},if j<3 {normal_radius} else {offset_radius})));
            let lod_valid=lod.iter().all(|w| {
                let range=distance_range(w.position,&target);
                (0..w.level).all(|i| range.hi<=query.thresholds[i])
                    && (w.level==query.depth as usize || range.lo>query.thresholds[w.level])
            });
            let bypass_valid=bypass.iter().all(|w| { let range=distance_range(w.centre,&target);if w.below {range.hi<query.bypass_squared} else {range.lo>=query.bypass_squared} });
            let planes_valid=planes.iter().all(|w| { let range=plane_range(w.position,&coefficients[w.plane]);if w.positive {range.lo>0.} else {range.hi<=0.} });
            if lod_valid&&bypass_valid&&planes_valid { certificate.target=target;certificate.planes=coefficients; }
        }
        certificate.lod=lod;certificate.bypass=bypass;certificate.visibility=planes;
        *destination=Some(certificate);
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn koe_topology_can_reuse(context: *mut Context,target: *const f32,depth: u32,thresholds: *const f32,count: usize,planes: *const super::Plane,bypass: f32) -> i32 {
    protected(|| {
        let context=context.as_ref().ok_or(())?;
        if target.is_null()||planes.is_null()||depth>31||count<depth as usize||(depth!=0&&thresholds.is_null()) { return Err(()); }
        let _query=context.query.read().map_err(|_| ())?;
        let slot=context.certificate.lock().map_err(|_| ())?;
        let c=slot.as_ref().ok_or(())?;
        if c.geometry_version!=context.geometry_version.load(Ordering::Relaxed)||depth!=c.anchor.depth||bypass.to_bits()!=c.anchor.bypass_squared.to_bits() { return Err(()); }
        for i in 0..depth as usize { if (*thresholds.add(i)).to_bits()!=c.anchor.thresholds[i].to_bits() { return Err(()); } }
        let mut inside=true;
        let position=std::array::from_fn(|i|*target.add(i));
        for i in 0..3 { if !position[i].is_finite() { return Err(()); }inside&=c.target[i].contains(position[i]); }
        for i in 0..6 {
            let p=&*planes.add(i);
            for j in 0..4 { let v=if j<3 {p.normal[j]} else {p.distance};if !v.is_finite(){return Err(());}inside&=c.planes[i][j].contains(v); }
        }
        if inside { return Ok(()); }
        if !c.point_proof { return Err(()); }
        for w in &c.lod {
            let squared=distance(w.position,position);
            if !(0..w.level).all(|i|squared<=c.anchor.thresholds[i])
                || (w.level<c.anchor.depth as usize && !(squared>c.anchor.thresholds[w.level])) { return Err(()); }
        }
        for w in &c.bypass {
            let below=distance(position,w.centre)<bypass;
            if below!=w.below { return Err(()); }
        }
        for w in &c.visibility {
            let value=plane_value(w.position,&*planes.add(w.plane));
            if !value.is_finite()||(value>0.)!=w.positive { return Err(()); }
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::*;
    fn triangle() -> Triangle {
        Triangle { positions:[[0.,0.,0.],[1.,0.,0.],[0.,0.,1.]],normals:[[0.,1.,0.];3],colors:[[1.,0.5,0.25,1.];3] }
    }
    unsafe fn evaluate(c:*mut Context,target:[f32;3],levels:&[f32],planes:&[Plane;6],bypass:f32) -> (Vec<[u32;10]>,Vec<u8>) {
        assert_eq!(koe_topology_prepare(c,target.as_ptr(),levels.len() as u32,levels.as_ptr(),levels.len(),planes.as_ptr(),bypass),1);
        assert_eq!(koe_topology_root(c,0),1);
        let mut output=Output::default();assert_eq!(koe_topology_finish(c,&mut output),1);
        (std::slice::from_raw_parts(output.vertices,output.vertex_count).iter().map(Vertex::bits).collect(),
            std::slice::from_raw_parts(output.indices,output.index_count*output.index_width as usize).to_vec())
    }
    #[test]
    fn certified_motion_and_plane_changes_preserve_every_output_bit() {
        unsafe {
            let anchor=koe_topology_create(&triangle(),1,10000,10000);
            let reference=koe_topology_create(&triangle(),1,10000,10000);
            let target=[5.,0.,5.];let levels=[100.,25.,6.25];
            let planes=[Plane{normal:[0.;3],distance:1.};6];
            assert_eq!(koe_topology_can_reuse(anchor,target.as_ptr(),3,levels.as_ptr(),3,planes.as_ptr(),0.),0);
            let expected=evaluate(anchor,target,&levels,&planes,0.);
            assert_eq!(koe_topology_seal(anchor),1);
            let mut hits=0;
            for i in 0..512 {
                let delta=((i*37%257) as f32-128.)*0.00001;
                let moved=[target[0]+delta,delta*0.5,target[2]-delta];
                let mut rotated=planes;rotated[0].normal=[delta,0.,-delta];rotated[0].distance+=delta;
                if koe_topology_can_reuse(anchor,moved.as_ptr(),3,levels.as_ptr(),3,rotated.as_ptr(),0.)==1 {
                    hits+=1;assert_eq!(evaluate(reference,moved,&levels,&rotated,0.),expected);
                }
            }
            assert!(hits>400);
            let outside={let certificate=(*anchor).certificate.lock().unwrap();let c=certificate.as_ref().unwrap();[c.target[0].hi.next_up(),target[1],target[2]]};
            if koe_topology_can_reuse(anchor,outside.as_ptr(),3,levels.as_ptr(),3,planes.as_ptr(),0.)==1 {
                assert_eq!(evaluate(reference,outside,&levels,&planes,0.),expected);
            }
            let moved=[0.;3];assert_eq!(koe_topology_can_reuse(anchor,moved.as_ptr(),3,levels.as_ptr(),3,planes.as_ptr(),0.),0);
            let mut hidden=planes;hidden[0].distance=-10.;
            assert_eq!(koe_topology_can_reuse(anchor,target.as_ptr(),3,levels.as_ptr(),3,hidden.as_ptr(),0.),0);
            let mut changed=levels;changed[0]=101.;
            assert_eq!(koe_topology_can_reuse(anchor,target.as_ptr(),3,changed.as_ptr(),3,planes.as_ptr(),0.),0);
            assert_eq!(koe_topology_root(anchor,0),1);
            assert_eq!(koe_topology_can_reuse(anchor,target.as_ptr(),3,levels.as_ptr(),3,planes.as_ptr(),0.),0);
            koe_topology_destroy(anchor);koe_topology_destroy(reference);
        }
    }
    #[test]
    fn nonfinite_intermediate_culling_does_not_grant_reuse() {
        unsafe {
            let t=Triangle{positions:[[1e20,1e20,0.],[1e20+1e14,1e20,0.],[1e20,1e20+1e14,0.]],normals:[[0.,0.,1.];3],colors:[[1.;4];3]};
            let c=koe_topology_create(&t,1,100,100);
            let target=[1e20,1e20,0.];let levels=[1e30];
            let overflow=[Plane{normal:[f32::MAX,-f32::MAX,0.],distance:0.};6];
            evaluate(c,target,&levels,&overflow,0.);assert_eq!(koe_topology_seal_fast(c),1);
            let visible=[Plane{normal:[0.;3],distance:1.};6];
            assert_eq!(koe_topology_can_reuse(c,target.as_ptr(),1,levels.as_ptr(),1,visible.as_ptr(),0.),0);
            koe_topology_destroy(c);
        }
    }
    #[test]
    fn coarse_roots_do_not_depend_on_frustum_visibility() {
        unsafe {
            let anchor=koe_topology_create(&triangle(),1,1000,1000);
            let reference=koe_topology_create(&triangle(),1,1000,1000);
            let levels=[1.,4.,16.];let target=[30.,0.,0.];
            let hidden=[Plane{normal:[0.;3],distance:-1.};6];let visible=[Plane{normal:[0.;3],distance:1.};6];
            let expected=evaluate(anchor,target,&levels,&hidden,0.);
            assert_eq!(koe_topology_seal_fast(anchor),1);
            let moved=[31.,0.,0.];assert_eq!(koe_topology_can_reuse(anchor,moved.as_ptr(),3,levels.as_ptr(),3,visible.as_ptr(),0.),1);
            assert_eq!(evaluate(reference,moved,&levels,&visible,0.),expected);
            let close=[0.;3];assert_eq!(koe_topology_can_reuse(anchor,close.as_ptr(),3,levels.as_ptr(),3,visible.as_ptr(),0.),0);
            koe_topology_destroy(anchor);koe_topology_destroy(reference);
        }
    }
    #[test]
    fn float_boundaries_fail_closed() {
        unsafe {
            let planes=[Plane{normal:[0.;3],distance:1.};6];
            for value in [0.,f32::from_bits(1),1e-20,1.,1e10,1e20] {
                let c=koe_topology_create(&triangle(),1,100,100);
                let target=[value,0.,0.];let squared=distance(target,[0.;3]);
                let levels=[if squared.is_finite(){squared}else{f32::MAX}];
                let expected=evaluate(c,target,&levels,&planes,0.);assert_eq!(koe_topology_seal(c),1);
                assert_eq!(koe_topology_can_reuse(c,target.as_ptr(),1,levels.as_ptr(),1,planes.as_ptr(),0.),1);
                for next in [value.next_down(),value.next_up()] {
                    if !next.is_finite(){continue;}
                    let moved=[next,0.,0.];
                    if koe_topology_can_reuse(c,moved.as_ptr(),1,levels.as_ptr(),1,planes.as_ptr(),0.)==1 {
                        let reference=koe_topology_create(&triangle(),1,100,100);
                        assert_eq!(evaluate(reference,moved,&levels,&planes,0.),expected);koe_topology_destroy(reference);
                    }
                }
                assert_eq!(koe_topology_prepare(c,target.as_ptr(),1,levels.as_ptr(),1,planes.as_ptr(),0.),1);
                assert_eq!(koe_topology_can_reuse(c,target.as_ptr(),1,levels.as_ptr(),1,planes.as_ptr(),0.),0);
                koe_topology_destroy(c);
            }
        }
    }
    #[test]
    fn outward_distance_intervals_enclose_float_evaluation() {
        let mut seed=1317u32;
        for _ in 0..10000 {
            let mut next=||{seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;(seed as i32 as f32)*0.0001};
            let position=[next(),next(),next()];let centre=[next(),next(),next()];
            let radius=next().abs() as f64*0.001;
            let target=centre.map(|v|Interval::around(v,radius));let range=distance_range(position,&target);
            for corner in 0..8 {
                let value=std::array::from_fn(|i|if corner&(1<<i)==0{target[i].lo}else{target[i].hi});
                let computed=distance(position,value);assert!(computed>=range.lo&&computed<=range.hi);
            }
        }
    }
}
