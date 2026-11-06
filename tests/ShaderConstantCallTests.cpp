#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
struct Buffer { unsigned char dirty, padding[7]; unsigned char *bytes; };
using Update = void(*)(Buffer *, int, const void *, uint64_t, bool);
using Select = int(*)(int);
static unsigned checks = 0;
static void Check(bool ok, const char *name) { ++checks; if (!ok) { std::printf("FAIL %s\n", name); std::exit(1); } }
template<class T> T Export(HMODULE module, const char *name)
{ auto address = GetProcAddress(module, name); Check(address != nullptr, name); return reinterpret_cast<T>(address); }
int wmain(int argc, wchar_t **argv)
{
    Check(argc == 3, "Unity and runtime paths");
    auto image = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto native = LoadLibraryW(argv[2]); Check(image && native, "load images");
    auto function = reinterpret_cast<Update>(reinterpret_cast<unsigned char *>(image) + 0x446980);
    unsigned char original[156]; std::memcpy(original, reinterpret_cast<void *>(function), sizeof(original));
    auto init = Export<int(*)()>(native, "InstallUniformKernel");
    auto select = Export<Select>(native, "SetUniformKernelMode");
    auto stats = Export<int(*)(uint64_t *, int)>(native, "ReadUniformKernelStats");
    Check(init() == 0, "install supported hook");
    auto helper = function;
    function = reinterpret_cast<Update>(Export<void *(*)()>(native, "GetUniformKernelTestEntry")());
    unsigned char testOriginal[24]; std::memcpy(testOriginal, reinterpret_cast<void *>(function), sizeof(testOriginal));
    for (unsigned count : {0u,1u,2u,3u,4u,8u,16u,17u,32u,4096u})
    for (unsigned pattern = 0; pattern < 8; ++pattern)
    {
        std::vector<unsigned char> source(count * 16, 73), a(count * 16 + 32, 73), b = a;
        if (count && pattern % 3 == 1) a[0] = b[0] = 17;
        if (count && pattern % 3 == 2) a[count * 16 - 1] = b[count * 16 - 1] = 17;
        Buffer first{}, second{}; first.bytes = a.data(); second.bytes = b.data();
        first.dirty = second.dirty = static_cast<unsigned char>(pattern);
        Check(select(0) == 0, "disable hook"); function(&first, 0, source.data(), count, pattern >= 4);
        Check(select(1) == 0, "enable optimized hook"); function(&second, 0, source.data(), count, pattern >= 4);
        Check(a == b && first.dirty == second.dirty, "actual hooked function byte parity");
    }
    Check(select(2) == 0, "enable validation");
    unsigned char source[64]{}, data[64]{}; Buffer buffer{}; buffer.bytes = data;
    for (unsigned n = 0; n < 100000; ++n) { source[n % 64] ^= 1; function(&buffer, 0, source, 4, false); }
    uint64_t counters[10]{}; Check(stats(counters, 10) == 2 && counters[8] > 0 && counters[9] == 0, "shadow validation against original function");
    Check(select(0) == 0, "restore original");
    Check(std::memcmp(original, reinterpret_cast<void *>(helper), sizeof(original)) == 0, "original helper remains untouched");
    Check(std::memcmp(testOriginal, reinterpret_cast<void *>(function), sizeof(testOriginal)) == 0, "exact call-site code restoration");
    std::printf("ACTUAL_UNITY_HOOK_PARITY_PASS checks=%u samples=%llu mismatches=%llu\n", checks, counters[8], counters[9]);
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency); volatile unsigned checksum = 0;
    for (unsigned count : {1u,2u,4u,8u,16u,32u})
    for (unsigned pattern : {0u,1u,2u,3u})
    {
        std::vector<unsigned char> src(count * 16, 71), dst(count * 16, 71);
        Buffer state{}; state.bytes = dst.data();
        auto run = [&](int mode)
        {
            Check(select(mode) == 0, "benchmark mode"); double best = 1e30;
            for (unsigned trial = 0; trial < 5; ++trial)
            {
                std::fill(dst.begin(), dst.end(), static_cast<unsigned char>(71));
                LARGE_INTEGER begin, end; QueryPerformanceCounter(&begin);
                for (unsigned n = 0; n < 300000; ++n)
                {
                    if (pattern == 1) dst[0] ^= 1;
                    if (pattern == 2) dst.back() ^= 1;
                    function(&state, 0, src.data(), count, pattern == 3);
                }
                QueryPerformanceCounter(&end); checksum = checksum + state.dirty + dst[0];
                best = (std::min)(best, (end.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart / 300000);
            }
            return best;
        };
        double a0 = run(0), b0 = run(1), b1 = run(1), a1 = run(0);
        double base = (a0 + a1) / 2, fast = (b0 + b1) / 2;
        std::printf("HOOK_BENCH blocks=%u pattern=%u original_ns=%.3f patched_ns=%.3f speed_change_pct=%.2f\n",count,pattern,base,fast,100*(base/fast-1));
    }
    Check(select(0) == 0, "final restoration");
    std::printf("checksum=%u\n", checksum);
}
