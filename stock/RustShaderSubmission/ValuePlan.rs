use crate::abi::{Array, PropertySheet, ValueParameter};
use crate::{parallel, property_data, Lookup, Writer};
use std::collections::VecDeque;
use std::{mem, ptr, slice};

#[derive(Clone, Copy)]
struct Parameter {
    name: u32,
    array_size: i32,
}

#[repr(C, align(16))]
struct Copy {
    source: u32,
    destination: u32,
    bytes: u32,
    header: u32,
}

const _: () = assert!(mem::size_of::<Copy>() == 16 && mem::align_of::<Copy>() == 16);

#[inline(always)]
unsafe fn emit(copy: &Copy, source: *const u8, output: *mut u8) {
    let destination = output.add(copy.destination as usize);
    ptr::write_unaligned(destination.cast::<u32>(), copy.header);
    match copy.bytes {
        0 => (),
        16 => ptr::copy_nonoverlapping(source.add(copy.source as usize), destination.add(4), 16),
        64 => ptr::copy_nonoverlapping(source.add(copy.source as usize), destination.add(4), 64),
        bytes => ptr::copy_nonoverlapping(
            source.add(copy.source as usize),
            destination.add(4),
            bytes as usize,
        ),
    }
}

struct Plan {
    parameters_address: usize,
    names_address: usize,
    counts: [u32; 3],
    starts: [i32; 7],
    parameters: Vec<Parameter>,
    names: Vec<u32>,
    descriptions: Vec<u32>,
    copies: Vec<Copy>,
    output_bytes: usize,
    source_end: usize,
}

impl Plan {
    unsafe fn matches(
        &self,
        parameters: &Array<ValueParameter>,
        counts: [u32; 3],
        properties: &PropertySheet,
    ) -> bool {
        self.parameters_address == parameters.data as usize
            && self.names_address == properties.names.data as usize
            && self.counts == counts
            && self.starts == properties.starts
            && self.names.as_slice()
                == slice::from_raw_parts(properties.names.data, properties.names.len)
            && self.descriptions.as_slice()
                == slice::from_raw_parts(properties.descriptions.data, properties.descriptions.len)
            && self.parameters.iter().enumerate().all(|(i, saved)| {
                let current = &*parameters.data.add(i);
                current.name == saved.name && current.array_size == saved.array_size
            })
    }

    unsafe fn compile(
        parameters: &Array<ValueParameter>,
        counts: [u32; 3],
        properties: &PropertySheet,
        lookup: &Lookup,
    ) -> Option<Self> {
        let count: usize = counts.iter().map(|&n| n as usize).sum();
        let mut plan = Self {
            parameters_address: parameters.data as usize,
            names_address: properties.names.data as usize,
            counts,
            starts: properties.starts,
            parameters: Vec::new(),
            names: Vec::new(),
            descriptions: Vec::new(),
            copies: Vec::new(),
            output_bytes: 4,
            source_end: 0,
        };
        plan.parameters.try_reserve_exact(count).ok()?;
        plan.copies.try_reserve_exact(count).ok()?;
        plan.names.try_reserve_exact(properties.names.len).ok()?;
        plan.descriptions
            .try_reserve_exact(properties.descriptions.len)
            .ok()?;
        plan.names.extend_from_slice(slice::from_raw_parts(
            properties.names.data,
            properties.names.len,
        ));
        plan.descriptions.extend_from_slice(slice::from_raw_parts(
            properties.descriptions.data,
            properties.descriptions.len,
        ));
        let mut index = 0;
        let mut destination = 0usize;
        for (kind, &amount) in counts.iter().enumerate() {
            let width = [4usize, 16, 64][kind];
            for _ in 0..amount {
                let parameter = &*parameters.data.add(index);
                plan.parameters.push(Parameter {
                    name: parameter.name,
                    array_size: parameter.array_size,
                });
                if let Some(property) = lookup.find(properties, kind, parameter.name) {
                    let (_, description) = property_data(properties, property, 0)?;
                    let declared = if parameter.array_size == 0 {
                        1
                    } else {
                        parameter.array_size as u16
                    };
                    let elements = declared.min(((description >> 20) & 1023) as u16);
                    let bytes = elements as usize * width;
                    property_data(properties, property, bytes)?;
                    let source = (description & 0xfffff) as usize;
                    plan.source_end = plan.source_end.max(source.checked_add(bytes)?);
                    plan.copies.push(Copy {
                        source: source.try_into().ok()?,
                        destination: destination.try_into().ok()?,
                        bytes: bytes.try_into().ok()?,
                        header: index as u32 | (elements as u32) << 16,
                    });
                    destination = destination.checked_add(4 + bytes)?;
                }
                index += 1;
            }
        }
        plan.output_bytes = destination.checked_add(4)?;
        Some(plan)
    }

    fn bytes(&self) -> usize {
        mem::size_of::<Self>()
            + self.parameters.capacity() * mem::size_of::<Parameter>()
            + self.copies.capacity() * mem::size_of::<Copy>()
            + (self.names.capacity() + self.descriptions.capacity()) * 4
    }

    unsafe fn write(&self, properties: &PropertySheet, writer: &mut Writer) -> bool {
        if self.source_end > properties.values.len {
            return false;
        }
        let Some(output) = writer.reserve(self.output_bytes) else {
            return false;
        };
        let mut pairs = self.copies.chunks_exact(2);
        for pair in &mut pairs {
            emit(&pair[0], properties.values.data, output);
            emit(&pair[1], properties.values.data, output);
        }
        for copy in pairs.remainder() {
            emit(copy, properties.values.data, output);
        }
        ptr::write_unaligned(output.add(self.output_bytes - 4).cast::<u32>(), 0xffff);
        true
    }
}

pub(super) struct Cache {
    plans: VecDeque<Plan>,
    bytes: usize,
}

impl Cache {
    pub(super) const fn new() -> Self {
        Self {
            plans: VecDeque::new(),
            bytes: 0,
        }
    }

    pub(super) unsafe fn write(
        &mut self,
        parameters: &Array<ValueParameter>,
        counts: [u32; 3],
        properties: &PropertySheet,
        lookup: &Lookup,
        writer: &mut Writer,
    ) -> Option<bool> {
        let budget = parallel::pool().cache_budget();
        if budget == 0 {
            return None;
        }
        if let Some(index) = self
            .plans
            .iter()
            .position(|plan| plan.matches(parameters, counts, properties))
        {
            let plan = self.plans.remove(index)?;
            let result = plan.write(properties, writer);
            self.plans.push_front(plan);
            return Some(result);
        }
        let plan = Plan::compile(parameters, counts, properties, lookup)?;
        let bytes = plan.bytes();
        if bytes > budget || self.plans.try_reserve(1).is_err() {
            return None;
        }
        while self.bytes > budget - bytes {
            let old = self.plans.pop_back()?;
            self.bytes -= old.bytes();
        }
        let result = plan.write(properties, writer);
        self.bytes += bytes;
        self.plans.push_front(plan);
        Some(result)
    }
}
