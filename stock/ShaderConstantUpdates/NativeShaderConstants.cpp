#include <Windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <fstream>
#include <filesystem>
#include "MinHook.h"
#include "UniformIdentity.h"
#ifdef KOE_RUST_KERNEL
#include "RustKernel.h"
static_assert(RustKernelUnwindSize == 0, "Rust kernel must remain a position-independent leaf");
static constexpr unsigned CodePages = 2;
#else
static constexpr unsigned CodePages = 1;
#endif
struct Buffer { unsigned char dirty, padding[7]; unsigned char *bytes; };
using Update = void(*)(Buffer *, int, const void *, uint64_t, bool);
extern "C" void UniformUpdateKernel(Buffer *, int, const void *, uint64_t, bool);
extern "C" unsigned char UniformUpdateKernelEnd;
extern "C" MH_STATUS WINAPI MH_CreateCallHook(void *, void *);
extern "C" MH_STATUS WINAPI MH_SetCallHookDestination(void *, void *);
static Update uniformOriginal = nullptr;
static void *target = nullptr;
static unsigned char *nearCode = nullptr;
static std::atomic<Update> selectedKernel{nullptr};
#ifdef KOE_UNIFORM_AUDIT
static void *testCall = nullptr;
static void *validationRelay = nullptr;
static RUNTIME_FUNCTION testUnwind{};
#endif
static std::atomic<int> mode{0};
#ifdef KOE_UNIFORM_AUDIT
static std::atomic<uint64_t> stats[10]{};
struct Case { uint32_t count, force, dirty, reserved; unsigned char source[256], before[256]; };
static_assert(sizeof(Case) == 528);
static std::mutex caseMutex;
static std::vector<Case> cases;
extern "C" void UniformUpdateValidation(Buffer *buffer, int offset, const void *source, uint64_t count, bool force)
{
    stats[0].fetch_add(1, std::memory_order_relaxed);
    unsigned category = count > 16 ? 6 : count == 0 ? 1 : count == 1 ? 2 : count == 2 ? 3 : count <= 4 ? 4 : 5;
    stats[category].fetch_add(1, std::memory_order_relaxed);
    if (force) stats[7].fetch_add(1, std::memory_order_relaxed);
    static thread_local uint32_t serial = 0;
    serial = serial * 1664525u + 1013904223u;
    if (count > 16 || (serial >> 24) != 0)
    {
        uniformOriginal(buffer, offset, source, count, force); return;
    }
    size_t size = static_cast<size_t>(count) * 16;
    auto *destination = buffer->bytes + offset;
    auto begin = reinterpret_cast<uintptr_t>(destination), src = reinterpret_cast<uintptr_t>(source);
    if (size && begin < src + size && src < begin + size)
    {
        uniformOriginal(buffer, offset, source, count, force); return;
    }
    Case record{}; record.count = static_cast<uint32_t>(count); record.force = force; record.dirty = buffer->dirty;
    if (size) { std::memcpy(record.before, destination, size); std::memcpy(record.source, source, size); }
    unsigned char copy[256]; if (size) std::memcpy(copy, record.before, size);
    Buffer shadow{}; shadow.dirty = buffer->dirty; shadow.bytes = copy;
    selectedKernel.load(std::memory_order_relaxed)(&shadow, 0, record.source, count, force);
    uniformOriginal(buffer, offset, source, count, force);
    stats[8].fetch_add(1, std::memory_order_relaxed);
    if (shadow.dirty != buffer->dirty || (size && std::memcmp(copy, destination, size)))
        stats[9].fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(caseMutex);
    if (cases.size() < 20000) cases.push_back(record);
}
#endif
static unsigned char *AllocateNear(void *origin)
{
    SYSTEM_INFO info; GetSystemInfo(&info);
    auto center = reinterpret_cast<uintptr_t>(origin) & ~(uintptr_t(info.dwAllocationGranularity) - 1);
    for (uintptr_t distance = info.dwAllocationGranularity; distance < 0x40000000; distance += info.dwAllocationGranularity)
    for (uintptr_t address : {center + distance, center - distance})
    {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) == sizeof(region) && region.State == MEM_FREE)
            if (auto memory = VirtualAlloc(reinterpret_cast<void *>(address), info.dwPageSize * CodePages, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<unsigned char *>(memory);
    }
    return nullptr;
}
static void AbsoluteJump(unsigned char *code, const void *destination)
{
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    std::memcpy(code, jump, sizeof(jump)); std::memcpy(code + 6, &destination, sizeof(destination));
}
extern "C" __declspec(dllexport) int InstallUniformKernel()
{
    if (target) return -1;
    SYSTEM_INFO cpu; GetNativeSystemInfo(&cpu);
    if (cpu.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) return -12;
    auto image = reinterpret_cast<unsigned char *>(GetModuleHandleW(L"UnityPlayer.dll"));
    if (!image) return -2;
    auto original = image + 0x446980;
    BCRYPT_ALG_HANDLE algorithm{}; unsigned char hash[32]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return -3;
    auto result = BCryptHash(algorithm, nullptr, 0, original, 156, hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0 || std::memcmp(hash, ExpectedUniformHash, sizeof(hash))) return -4;
    target = image + 0x4466b2;
    static const unsigned char call[] = {0xe8,0xc9,0x02,0,0};
    if (std::memcmp(target, call, sizeof(call))) return -7;
    uniformOriginal = reinterpret_cast<Update>(original);
    nearCode = AllocateNear(target); if (!nearCode) return -8;
    size_t kernelSize = reinterpret_cast<uintptr_t>(&UniformUpdateKernelEnd) - reinterpret_cast<uintptr_t>(UniformUpdateKernel);
    if (!kernelSize || kernelSize > 512) return -9;
    static const unsigned char guard[] = {0x49,0x83,0xf9,0x10,0x0f,0x87};
    std::memcpy(nearCode, guard, sizeof(guard));
    int32_t skip = static_cast<int32_t>(kernelSize); std::memcpy(nearCode + 6, &skip, 4);
    std::memcpy(nearCode + 10, reinterpret_cast<void *>(UniformUpdateKernel), kernelSize);
    AbsoluteJump(nearCode + 10 + kernelSize, original);
    selectedKernel = reinterpret_cast<Update>(nearCode);
#ifdef KOE_RUST_KERNEL
    static_assert(sizeof(RustKernelBytes) < 2048, "Rust kernel exceeds its code-page budget");
    auto rust = nearCode + 4096; std::memcpy(rust, guard, sizeof(guard));
    int32_t rustSkip = sizeof(RustKernelBytes); std::memcpy(rust + 6, &rustSkip, 4);
    std::memcpy(rust + 10, RustKernelBytes, sizeof(RustKernelBytes));
    AbsoluteJump(rust + 10 + sizeof(RustKernelBytes), original);
    selectedKernel = reinterpret_cast<Update>(rust);
#endif
#ifdef KOE_UNIFORM_AUDIT
    validationRelay = nearCode + 0x400; AbsoluteJump(static_cast<unsigned char *>(validationRelay), reinterpret_cast<void *>(UniformUpdateValidation));
    static const unsigned char wrapper[] = {0x48,0x8b,0x44,0x24,0x28,0x48,0x83,0xec,0x28,0x48,0x89,0x44,0x24,0x20,0xe8,0,0,0,0,0x48,0x83,0xc4,0x28,0xc3};
    std::memcpy(nearCode + 0x800, wrapper, sizeof(wrapper)); testCall = nearCode + 0x800 + 14;
    int32_t displacement = static_cast<int32_t>(original - (static_cast<unsigned char *>(testCall) + 5));
    std::memcpy(static_cast<unsigned char *>(testCall) + 1, &displacement, 4);
    static const unsigned char unwind[] = {1,9,1,0,9,0x42,0,0}; std::memcpy(nearCode + 0x900, unwind, sizeof(unwind));
    testUnwind.BeginAddress = 0x800; testUnwind.EndAddress = 0x800 + sizeof(wrapper); testUnwind.UnwindData = 0x900;
    if (!RtlAddFunctionTable(&testUnwind, 1, reinterpret_cast<DWORD64>(nearCode))) return -10;
#endif
    DWORD protection; if (!VirtualProtect(nearCode, 4096 * CodePages, PAGE_EXECUTE_READ, &protection)) return -11;
    FlushInstructionCache(GetCurrentProcess(), nearCode, 4096 * CodePages);
#ifdef KOE_UNIFORM_AUDIT
    cases.reserve(20000);
#endif
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return -5;
    if (MH_CreateCallHook(target, nearCode) != MH_OK) return -6;
#ifdef KOE_UNIFORM_AUDIT
    if (MH_CreateCallHook(testCall, nearCode) != MH_OK) return -6;
#endif
    return 0;
}
extern "C" __declspec(dllexport) int SetUniformKernelMode(int value)
{
#ifdef KOE_UNIFORM_AUDIT
    constexpr int maxMode = 2;
    if (value && stats[9].load()) return -1;
#else
    constexpr int maxMode = 1;
#endif
    if (!target || value < 0 || value > maxMode) return -1;
    MH_QueueDisableHook(target);
#ifdef KOE_UNIFORM_AUDIT
    MH_QueueDisableHook(testCall);
#endif
    if (MH_ApplyQueued() != MH_OK) return -2;
    if (value)
    {
        void *destination = reinterpret_cast<void *>(selectedKernel.load());
#ifdef KOE_UNIFORM_AUDIT
        if (value == 2) destination = validationRelay;
#endif
        if (MH_SetCallHookDestination(target, destination) != MH_OK) return -3;
        MH_QueueEnableHook(target);
#ifdef KOE_UNIFORM_AUDIT
        if (MH_SetCallHookDestination(testCall, destination) != MH_OK) return -3;
        MH_QueueEnableHook(testCall);
#endif
        if (MH_ApplyQueued() != MH_OK) return -4;
    }
    mode.store(value); return 0;
}
#ifdef KOE_UNIFORM_AUDIT
#ifdef KOE_RUST_KERNEL
extern "C" __declspec(dllexport) int SetUniformKernelBackend(int backend)
{
    if (mode.load() != 0 || backend < 0 || backend > 1) return -1;
    selectedKernel.store(reinterpret_cast<Update>(nearCode + (backend ? 4096 : 0)));
    return 0;
}
#endif
extern "C" __declspec(dllexport) void *GetUniformKernelTestEntry() { return nearCode ? nearCode + 0x800 : nullptr; }
extern "C" __declspec(dllexport) int ReadUniformKernelStats(uint64_t *output, int count)
{
    if (!output || count != 10) return -1;
    for (unsigned i = 0; i < 10; ++i) output[i] = stats[i].load(std::memory_order_relaxed);
    return mode.load();
}
extern "C" __declspec(dllexport) int SaveUniformKernelCases(const wchar_t *path)
{
    std::lock_guard<std::mutex> lock(caseMutex);
    std::ofstream stream(std::filesystem::path(path), std::ios::binary);
    uint32_t count = static_cast<uint32_t>(cases.size());
    stream.write("KOEUCB1\0", 8); stream.write(reinterpret_cast<const char *>(&count), sizeof(count));
    stream.write(reinterpret_cast<const char *>(cases.data()), cases.size() * sizeof(Case));
    return stream ? static_cast<int>(count) : -1;
}
#endif
