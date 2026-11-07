use crate::abi::PropertySheet;
use crate::parallel::pool;
use std::collections::VecDeque;
use std::sync::{Arc, OnceLock};

struct Entry {
    key: u64,
    index: usize,
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn duplicate_names_keep_the_first_index_within_each_type() {
        let layout = Layout {
            starts: [0, 3, 5, 6, 7, 8, 9],
            names: vec![42, 9, 42, 42, 42, 42, 42, 42, 42].into_boxed_slice(),
            entries: OnceLock::new(),
        };
        layout.build();
        for (kind, index) in [0, 3, 5, 6, 7, 8].iter().enumerate() {
            assert_eq!(layout.find(kind, 42), Some(Some(*index)));
        }
        assert_eq!(layout.find(0, 99), Some(None));
    }
    #[test]
    fn cache_detects_changed_names_in_a_reused_buffer() {
        let mut names: Vec<u32> = (0..25).collect();
        let mut sheet: PropertySheet = unsafe { std::mem::zeroed() };
        sheet.starts = [0, 25, 25, 25, 25, 25, 25];
        sheet.names.data = names.as_mut_ptr();
        sheet.names.len = names.len();
        sheet.names.capacity_tag = names.len() * 2;
        let mut cache = Cache::new();
        unsafe {
            cache.prepare(&sheet);
        }
        cache.current.as_ref().unwrap().build();
        assert_eq!(cache.find(0, 3), Some(Some(3)));
        names[3] = 987;
        unsafe {
            cache.prepare(&sheet);
        }
        cache.current.as_ref().unwrap().build();
        assert_eq!(cache.find(0, 3), Some(None));
        assert_eq!(cache.find(0, 987), Some(Some(3)));
    }
}
pub struct Layout {
    starts: [i32; 7],
    names: Box<[u32]>,
    entries: OnceLock<Box<[Entry]>>,
}

impl Layout {
    fn slot(key: u64, mask: usize) -> usize {
        let mixed = key.wrapping_mul(0x9e3779b97f4a7c15);
        ((mixed ^ (mixed >> 32)) as usize) & mask
    }
    fn build(&self) {
        let Some(capacity) = self
            .names
            .len()
            .checked_mul(2)
            .and_then(usize::checked_next_power_of_two)
        else {
            return;
        };
        let mut entries = Vec::new();
        if entries.try_reserve_exact(capacity).is_err() {
            return;
        }
        entries.resize_with(capacity, || Entry {
            key: 0,
            index: usize::MAX,
        });
        for kind in 0..6 {
            for index in self.starts[kind] as usize..self.starts[kind + 1] as usize {
                let key = (kind as u64) << 32 | self.names[index] as u64;
                let mut slot = Self::slot(key, capacity - 1);
                loop {
                    if entries[slot].index == usize::MAX {
                        entries[slot] = Entry { key, index };
                        break;
                    }
                    if entries[slot].key == key {
                        break;
                    }
                    slot = (slot + 1) & (capacity - 1);
                }
            }
        }
        let _ = self.entries.set(entries.into_boxed_slice());
    }
    fn matches(&self, starts: &[i32; 7], names: &[u32]) -> bool {
        self.starts == *starts && self.names.as_ref() == names
    }
    pub fn find(&self, kind: usize, name: u32) -> Option<Option<usize>> {
        let entries = self.entries.get()?;
        let key = (kind as u64) << 32 | name as u64;
        let mut slot = Self::slot(key, entries.len() - 1);
        loop {
            let entry = &entries[slot];
            if entry.index == usize::MAX {
                return Some(None);
            }
            if entry.key == key {
                return Some(Some(entry.index));
            }
            slot = (slot + 1) & (entries.len() - 1);
        }
    }
    fn bytes(&self) -> usize {
        self.names.len() * 4
            + self.entries.get().map_or(self.names.len() * 64, |x| {
                x.len() * std::mem::size_of::<Entry>()
            })
    }
}

pub struct Cache {
    current: Option<Arc<Layout>>,
    previous: VecDeque<Arc<Layout>>,
    bytes: usize,
}
impl Cache {
    pub const fn new() -> Self {
        Self {
            current: None,
            previous: VecDeque::new(),
            bytes: 0,
        }
    }
    pub unsafe fn prepare(&mut self, sheet: &PropertySheet) {
        let names = std::slice::from_raw_parts(sheet.names.data, sheet.names.len);
        if self
            .current
            .as_ref()
            .is_some_and(|x| x.matches(&sheet.starts, names))
        {
            return;
        }
        let layout = if let Some(index) = self
            .previous
            .iter()
            .position(|x| x.matches(&sheet.starts, names))
        {
            self.previous.remove(index).unwrap()
        } else {
            let mut copy = Vec::new();
            if copy.try_reserve_exact(names.len()).is_err() {
                self.current = None;
                return;
            }
            copy.extend_from_slice(names);
            let layout = Arc::new(Layout {
                starts: sheet.starts,
                names: copy.into_boxed_slice(),
                entries: OnceLock::new(),
            });
            let build = layout.clone();
            if !pool().schedule(move || build.build()) {
                layout.build();
            }
            layout
        };
        if let Some(old) = self.current.replace(layout) {
            self.previous.push_front(old);
        }
        self.bytes = self.previous.iter().map(|x| x.bytes()).sum();
        let budget = pool().cache_budget();
        while self.bytes > budget {
            if let Some(old) = self.previous.pop_back() {
                self.bytes = self.bytes.saturating_sub(old.bytes());
            } else {
                break;
            }
        }
    }
    pub fn find(&self, kind: usize, name: u32) -> Option<Option<usize>> {
        self.current.as_ref()?.find(kind, name)
    }
}
