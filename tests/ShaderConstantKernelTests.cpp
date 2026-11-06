#include <Windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>
#include <filesystem>
#include <fstream>
struct Buffer { unsigned char dirty, padding[7]; unsigned char *bytes; };
static_assert(offsetof(Buffer, bytes) == 8);
using Update = void(*)(Buffer *, int, const void *, uint64_t, bool);
extern "C" void UniformUpdateKernel(Buffer *, int, const void *, uint64_t, bool);
static Update candidate = UniformUpdateKernel;
extern "C" void UniformUpdateDetour(Buffer *, int, const void *, uint64_t, bool);
extern "C" { volatile long uniformValidationEnabled = 0; Update uniformOriginal = nullptr; }
extern "C" void UniformUpdateValidation(Buffer *b, int offset, const void *source, uint64_t count, bool force)
{ uniformOriginal(b, offset, source, count, force); }
static unsigned checks = 0;
static void Check(bool ok, const char *name)
{ ++checks; if (!ok) { std::printf("FAIL %s\n", name); std::exit(1); } }
static std::vector<unsigned char> Random(std::mt19937 &random, size_t size)
{ std::vector<unsigned char> bytes(size); for (auto &b : bytes) b = static_cast<unsigned char>(random()); return bytes; }
static void Compare(Update original, const std::vector<unsigned char> &initial, int offset,
                    const std::vector<unsigned char> &source, unsigned count, bool force, unsigned char dirty)
{
    auto a = initial, b = initial; Buffer before{}, after{}; before.dirty = after.dirty = dirty;
    before.bytes = a.data(); after.bytes = b.data();
    original(&before, offset, source.data(), count, force); candidate(&after, offset, source.data(), count, force);
    Check(a == b && before.dirty == after.dirty, "actual Unity output and dirty-byte parity");
}
static void Benchmark(Update original)
{
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    volatile unsigned checksum = 0;
    for (unsigned count : {1u, 2u, 4u, 8u, 16u, 32u})
    for (unsigned pattern : {0u, 1u, 2u, 3u})
    {
        std::vector<unsigned char> source(count * 16, 71), initial(count * 16, 71), data(count * 16);
        if (pattern == 1) initial[0] ^= 1;
        if (pattern == 2) initial.back() ^= 1;
        bool force = pattern == 3;
        auto run = [&](Update function)
        {
            Buffer buffer{}; buffer.bytes = data.data(); double best = 1e30;
            for (unsigned repeat = 0; repeat < 5; ++repeat)
            {
                data = initial; LARGE_INTEGER start, end; QueryPerformanceCounter(&start);
                for (unsigned n = 0; n < 300000; ++n)
                {
                    if (pattern == 1) data[0] ^= 1;
                    if (pattern == 2) data.back() ^= 1;
                    function(&buffer, 0, source.data(), count, force);
                }
                QueryPerformanceCounter(&end); checksum = checksum + buffer.dirty + data[0];
                best = (std::min)(best, (end.QuadPart - start.QuadPart) * 1e9 / frequency.QuadPart / 300000);
            }
            return best;
        };
        double a = run(original), b = run(candidate), c = run(candidate), d = run(original);
        double base = (a + d) / 2, candidate = (b + c) / 2;
        std::printf("KERNEL_BENCH blocks=%u pattern=%u original_ns=%.3f simd_ns=%.3f speed_change_pct=%.2f\n",
                    count, pattern, base, candidate, 100 * (base / candidate - 1));
    }
    std::printf("checksum=%u\n", checksum);
}
int wmain(int argc, wchar_t **argv)
{
    Check(argc == 2 || argc == 3, "UnityPlayer path and optional kernel blob");
    if (argc == 3)
    {
        std::ifstream input(std::filesystem::path(argv[2]), std::ios::binary);
        std::vector<char> code{std::istreambuf_iterator<char>(input), {}};
        Check(!code.empty() && code.size() < 4096, "Rust leaf kernel blob");
        auto memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Check(memory != nullptr, "kernel code allocation"); std::memcpy(memory, code.data(), code.size());
        DWORD old; Check(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old) != 0, "kernel executable protection");
        FlushInstructionCache(GetCurrentProcess(), memory, code.size()); candidate = reinterpret_cast<Update>(memory);
    }
    auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(module != nullptr, "load original Unity image");
    auto address = reinterpret_cast<unsigned char *>(module) + 0x446980;
    static const unsigned char prefix[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x4c,0x63,0xd2};
    Check(std::memcmp(address, prefix, sizeof(prefix)) == 0, "supported original prologue");
    auto original = reinterpret_cast<Update>(address); uniformOriginal = original;
    std::mt19937 random(42017);
    for (unsigned count : {0u,1u,2u,3u,4u,8u,16u,17u,32u,64u,256u,4096u})
    for (unsigned alignment = 0; alignment < 16; ++alignment)
    for (unsigned trial = 0; trial < 50; ++trial)
    {
        auto initial = Random(random, count * 16 + 48), source = Random(random, count * 16);
        if (trial % 3 == 0) std::copy(initial.begin() + alignment, initial.begin() + alignment + source.size(), source.begin());
        if (trial % 3 == 1 && !source.empty()) { std::copy(initial.begin() + alignment, initial.begin() + alignment + source.size(), source.begin()); source.back() ^= 1; }
        Compare(original, initial, static_cast<int>(alignment), source, count, (trial & 1) != 0, static_cast<unsigned char>(trial));
    }
    for (int distance : {-31, -16, -8, -1, 0, 1, 8, 16, 31})
    for (unsigned count : {1u,2u,4u,16u,32u})
    for (bool force : {false, true})
    {
        auto a = Random(random, count * 16 + 256), b = a; Buffer before{}, after{};
        before.bytes = a.data(); after.bytes = b.data();
        original(&before, 96, a.data() + 96 + distance, count, force);
        candidate(&after, 96, b.data() + 96 + distance, count, force);
        Check(a == b && before.dirty == after.dirty, "overlapping source/destination parity");
    }
    SYSTEM_INFO system; GetSystemInfo(&system); size_t page = system.dwPageSize;
    for (unsigned count : {1u,2u,4u,8u,16u})
    {
        auto memory = static_cast<unsigned char *>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(memory != nullptr, "guard allocation"); DWORD old;
        Check(VirtualProtect(memory + page, page, PAGE_NOACCESS, &old) != 0, "source guard");
        Check(VirtualProtect(memory + page * 3, page, PAGE_NOACCESS, &old) != 0, "destination guard");
        auto source = memory + page - count * 16, destination = memory + page * 3 - count * 16;
        std::memset(source, 7, count * 16); std::memset(destination, 7, count * 16);
        Buffer buffer{}; buffer.bytes = destination;
        candidate(&buffer, 0, source, count, false); Check(buffer.dirty == 0, "equal guard boundary");
        source[count * 16 - 1] ^= 1; candidate(&buffer, 0, source, count, false);
        Check(buffer.dirty == 1 && std::memcmp(source, destination, count * 16) == 0, "changed guard boundary");
        VirtualFree(memory, 0, MEM_RELEASE);
    }
    std::printf("ACTUAL_UNITY_UNIFORM_KERNEL_PARITY_PASS checks=%u\n", checks);
    Benchmark(original);
}
