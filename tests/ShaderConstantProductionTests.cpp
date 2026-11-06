#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
struct Buffer { unsigned char dirty, padding[7]; unsigned char *bytes; };
using Update = void(*)(Buffer *, int, const void *, uint64_t, bool);
static unsigned checks = 0;
static void Check(bool ok, const char *name) { ++checks; if (!ok) { std::printf("FAIL %s\n", name); std::exit(1); } }
template<class T> T Export(HMODULE module, const char *name)
{ auto address = GetProcAddress(module, name); Check(address != nullptr, name); return reinterpret_cast<T>(address); }
int wmain(int argc, wchar_t **argv)
{
    Check(argc == 3, "Unity and production runtime paths");
    auto image = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto module = LoadLibraryW(argv[2]); Check(image && module, "load images");
    auto base = reinterpret_cast<unsigned char *>(image), call = base + 0x4466b2;
    auto original = reinterpret_cast<Update>(base + 0x446980);
    unsigned char backup[5]; std::memcpy(backup, call, 5);
    auto init = Export<int(*)()>(module, "InstallUniformKernel");
    auto select = Export<int(*)(int)>(module, "SetUniformKernelMode");
    Check(GetProcAddress(module, "ReadUniformKernelStats") == nullptr && GetProcAddress(module, "SaveUniformKernelCases") == nullptr && GetProcAddress(module, "GetUniformKernelTestEntry") == nullptr, "diagnostics absent");
    Check(init() == 0 && select(1) == 0, "production activation");
    int32_t relative; std::memcpy(&relative, call + 1, 4);
    auto optimized = reinterpret_cast<Update>(call + 5 + relative);
    for (unsigned count : {0u,1u,2u,4u,8u,16u,17u,32u,4096u})
    for (unsigned variant = 0; variant < 8; ++variant)
    {
        std::vector<unsigned char> source(count * 16, 73), a(count * 16 + 32, 73), b = a;
        if (count && variant % 3 == 1) a[0] = b[0] = 17;
        if (count && variant % 3 == 2) a[count * 16 - 1] = b[count * 16 - 1] = 17;
        Buffer first{}, second{}; first.bytes = a.data(); second.bytes = b.data(); first.dirty = second.dirty = static_cast<unsigned char>(variant);
        original(&first, 0, source.data(), count, variant >= 4);
        optimized(&second, 0, source.data(), count, variant >= 4);
        Check(a == b && first.dirty == second.dirty, "production byte parity");
    }
    Check(select(2) != 0, "validation mode excluded from production");
    Check(select(0) == 0 && std::memcmp(backup, call, 5) == 0, "exact original call restoration");
    std::printf("UNIFORM_PRODUCTION_BYTE_PARITY_AND_RESTORE_PASS checks=%u\n", checks);
}
