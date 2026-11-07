use std::collections::VecDeque;
use std::ptr;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::Instant;

#[derive(Clone, Copy)]
pub struct CopyRange {
    pub source: usize,
    pub destination: usize,
    pub bytes: usize,
}

struct CopyBatch {
    ranges: usize,
    count: usize,
    output: usize,
    next: AtomicUsize,
    chunk: usize,
    remaining: AtomicUsize,
    done: Mutex<()>,
    wake: Condvar,
}

impl CopyBatch {
    unsafe fn work(&self) {
        loop {
            let index = self.next.fetch_add(self.chunk, Ordering::Relaxed);
            if index >= self.count {
                break;
            }
            for index in index..(index + self.chunk).min(self.count) {
                let range = &*((self.ranges as *const CopyRange).add(index));
                ptr::copy_nonoverlapping(
                    range.source as *const u8,
                    (self.output + range.destination) as *mut u8,
                    range.bytes,
                );
            }
        }
    }
    fn finish(&self) {
        let guard = self.done.lock().unwrap();
        if self.remaining.fetch_sub(1, Ordering::AcqRel) == 1 {
            self.wake.notify_one();
        }
        drop(guard);
    }
}

enum Task {
    Build(Box<dyn FnOnce() + Send>),
    Copy(Arc<CopyBatch>),
}

struct Shared {
    tasks: Mutex<VecDeque<Task>>,
    wake: Condvar,
    completed: Vec<AtomicU64>,
}

pub struct Pool {
    shared: Arc<Shared>,
    dispatch: Mutex<()>,
    workers: usize,
    logical: usize,
    threshold: AtomicUsize,
    cache_budget: AtomicUsize,
    builds: AtomicU64,
    batches: AtomicU64,
    bytes: AtomicU64,
}

static POOL: OnceLock<Pool> = OnceLock::new();

pub fn pool() -> &'static Pool {
    POOL.get_or_init(|| {
        let logical = std::thread::available_parallelism().map_or(1, usize::from);
        let shared = Arc::new(Shared {
            tasks: Mutex::new(VecDeque::new()),
            wake: Condvar::new(),
            completed: (0..logical.saturating_sub(1))
                .map(|_| AtomicU64::new(0))
                .collect(),
        });
        let mut workers = 0;
        for index in 0..logical.saturating_sub(1) {
            let shared = shared.clone();
            if std::thread::Builder::new()
                .name(format!("KOE render {index}"))
                .spawn(move || loop {
                    let task = {
                        let mut queue = shared.tasks.lock().unwrap();
                        while queue.is_empty() {
                            queue = shared.wake.wait(queue).unwrap();
                        }
                        queue.pop_front().unwrap()
                    };
                    match task {
                        Task::Build(build) => {
                            build();
                            shared.completed[index].fetch_add(1, Ordering::Relaxed);
                        }
                        Task::Copy(batch) => {
                            unsafe {
                                batch.work();
                            }
                            shared.completed[index].fetch_add(1, Ordering::Relaxed);
                            batch.finish();
                        }
                    }
                })
                .is_err()
            {
                break;
            }
            workers += 1;
        }
        Pool {
            shared,
            dispatch: Mutex::new(()),
            workers,
            logical,
            threshold: AtomicUsize::new(usize::MAX),
            cache_budget: AtomicUsize::new(0),
            builds: AtomicU64::new(0),
            batches: AtomicU64::new(0),
            bytes: AtomicU64::new(0),
        }
    })
}

impl Pool {
    pub fn threshold(&self) -> usize {
        self.threshold.load(Ordering::Relaxed)
    }
    pub fn cache_budget(&self) -> usize {
        self.cache_budget.load(Ordering::Relaxed)
    }
    pub fn schedule(&self, build: impl FnOnce() + Send + 'static) -> bool {
        if self.workers == 0 {
            return false;
        }
        let mut queue = self.shared.tasks.lock().unwrap();
        if queue.len() >= self.workers * 4 {
            return false;
        }
        queue.push_back(Task::Build(Box::new(build)));
        self.builds.fetch_add(1, Ordering::Relaxed);
        drop(queue);
        self.shared.wake.notify_one();
        true
    }

    unsafe fn dispatch(&self, ranges: &[CopyRange], output: *mut u8) -> bool {
        if self.workers == 0 || ranges.len() < 2 {
            return false;
        }
        let Ok(_exclusive) = self.dispatch.try_lock() else {
            return false;
        };
        let workers = self.workers.min(ranges.len() - 1);
        let batch = Arc::new(CopyBatch {
            ranges: ranges.as_ptr() as usize,
            count: ranges.len(),
            output: output as usize,
            next: AtomicUsize::new(0),
            chunk: ranges.len().div_ceil(self.logical),
            remaining: AtomicUsize::new(workers),
            done: Mutex::new(()),
            wake: Condvar::new(),
        });
        {
            let mut queue = self.shared.tasks.lock().unwrap();
            for _ in 0..workers {
                queue.push_back(Task::Copy(batch.clone()));
            }
        }
        if workers == self.workers {
            self.shared.wake.notify_all();
        } else {
            for _ in 0..workers {
                self.shared.wake.notify_one();
            }
        }
        batch.work();
        let mut guard = batch.done.lock().unwrap();
        while batch.remaining.load(Ordering::Acquire) != 0 {
            guard = batch.wake.wait(guard).unwrap();
        }
        true
    }

    // AI-generated comment: The caller owns disjoint destinations and keeps both buffers alive until this returns.
    pub unsafe fn copy(&self, ranges: &[CopyRange], output: *mut u8, bytes: usize) -> bool {
        if bytes < self.threshold.load(Ordering::Relaxed) {
            return false;
        }
        if !self.dispatch(ranges, output) {
            return false;
        }
        self.batches.fetch_add(1, Ordering::Relaxed);
        self.bytes.fetch_add(bytes as u64, Ordering::Relaxed);
        true
    }

    pub fn calibrate(&self, cache_bytes: usize, page_bytes: usize) {
        self.cache_budget.store(cache_bytes, Ordering::Relaxed);
        if self.workers == 0 {
            return;
        }
        let maximum = cache_bytes.max(page_bytes).max(1);
        let mut source = Vec::new();
        let mut output = Vec::new();
        if source.try_reserve_exact(maximum).is_err() || output.try_reserve_exact(maximum).is_err()
        {
            return;
        }
        source.resize(maximum, 0x5au8);
        output.resize(maximum, 0u8);
        let mut threshold = usize::MAX;
        let mut bytes = page_bytes.max(1).min(maximum);
        loop {
            let chunk = bytes
                .div_ceil(self.logical)
                .next_multiple_of(page_bytes.max(1));
            let ranges: Vec<_> = (0..bytes)
                .step_by(chunk)
                .map(|offset| CopyRange {
                    source: source.as_ptr() as usize + offset,
                    destination: offset,
                    bytes: (bytes - offset).min(chunk),
                })
                .collect();
            let mut serial = u128::MAX;
            let mut parallel = 0;
            let mut dispatched = false;
            for _ in 0..3 {
                let start = Instant::now();
                unsafe {
                    ptr::copy_nonoverlapping(source.as_ptr(), output.as_mut_ptr(), bytes);
                }
                std::hint::black_box(&output);
                serial = serial.min(start.elapsed().as_nanos());
                let start = Instant::now();
                let ran = unsafe { self.dispatch(&ranges, output.as_mut_ptr()) };
                std::hint::black_box(&output);
                if ran {
                    parallel = parallel.max(start.elapsed().as_nanos());
                    dispatched = true;
                }
            }
            if dispatched && parallel < serial {
                threshold = bytes;
                break;
            }
            if bytes == maximum {
                break;
            }
            bytes = bytes.saturating_mul(2).min(maximum);
        }
        self.threshold.store(threshold, Ordering::Relaxed);
    }

    pub fn info(&self) -> [u64; 7] {
        [
            self.logical as u64,
            self.workers as u64,
            self.threshold.load(Ordering::Relaxed) as u64,
            self.builds.load(Ordering::Relaxed),
            self.batches.load(Ordering::Relaxed),
            self.bytes.load(Ordering::Relaxed),
            self.shared
                .completed
                .iter()
                .filter(|count| count.load(Ordering::Relaxed) != 0)
                .count() as u64,
        ]
    }
}

#[no_mangle]
pub extern "C" fn KoeRustInitializeParallel(cache_bytes: usize, page_bytes: usize) {
    pool().calibrate(cache_bytes, page_bytes);
    for counter in &pool().shared.completed {
        counter.store(0, Ordering::Relaxed);
    }
}

#[no_mangle]
pub unsafe extern "C" fn KoeRustParallelInfo(output: *mut u64, count: usize) {
    if output.is_null() {
        return;
    }
    let info = pool().info();
    ptr::copy_nonoverlapping(info.as_ptr(), output, count.min(info.len()));
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn calibration_never_times_a_skipped_dispatch_as_success() {
        pool().calibrate(4096, 4096);
        assert_eq!(pool().threshold(), usize::MAX);
    }
    #[test]
    fn workers_copy_disjoint_ranges_and_preserve_guards() {
        let source: Vec<u8> = (0..8 * 1024 * 1024).map(|i| (i ^ (i >> 8)) as u8).collect();
        let mut destination = vec![0xa5u8; source.len() + 128];
        let ranges: Vec<_> = (0..source.len())
            .step_by(7919)
            .map(|offset| CopyRange {
                source: source.as_ptr() as usize + offset,
                destination: offset,
                bytes: (source.len() - offset).min(7919),
            })
            .collect();
        if pool().workers == 0 {
            return;
        }
        unsafe {
            assert!(pool().dispatch(&ranges, destination.as_mut_ptr().add(64)));
        }
        assert_eq!(&destination[64..64 + source.len()], source.as_slice());
        assert!(destination[..64]
            .iter()
            .chain(destination[64 + source.len()..].iter())
            .all(|&x| x == 0xa5));
    }
    #[test]
    fn owned_jobs_complete_before_their_data_is_released() {
        if pool().workers == 0 {
            return;
        }
        let result = Arc::new(AtomicUsize::new(0));
        let jobs = pool().logical;
        for _ in 0..jobs {
            let result = result.clone();
            let values = vec![7usize; 1001];
            let task = move || {
                result.fetch_add(values.iter().sum::<usize>(), Ordering::Release);
            };
            assert!(pool().schedule(task));
        }
        let deadline = Instant::now() + std::time::Duration::from_secs(5);
        while result.load(Ordering::Acquire) != jobs * 7007 && Instant::now() < deadline {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert_eq!(result.load(Ordering::Acquire), jobs * 7007);
    }
    #[test]
    fn concurrent_callers_keep_their_destinations_separate() {
        let callers: Vec<_> = (0..4)
            .map(|seed| {
                std::thread::spawn(move || {
                    let source = vec![seed as u8; 1024 * 1024];
                    let mut destination = vec![255u8; source.len()];
                    let ranges: Vec<_> = (0..source.len())
                        .step_by(4096)
                        .map(|offset| CopyRange {
                            source: source.as_ptr() as usize + offset,
                            destination: offset,
                            bytes: 4096,
                        })
                        .collect();
                    unsafe {
                        if !pool().dispatch(&ranges, destination.as_mut_ptr()) {
                            ptr::copy_nonoverlapping(
                                source.as_ptr(),
                                destination.as_mut_ptr(),
                                source.len(),
                            );
                        }
                    }
                    assert_eq!(source, destination);
                })
            })
            .collect();
        for caller in callers {
            caller.join().unwrap();
        }
    }
}
