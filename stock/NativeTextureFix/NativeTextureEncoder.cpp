// AI-generated comment: Unity's original block encoder processes disjoint row ranges. Modes 1/2 are diagnostics; production uses 3.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstring>
#include "MinHook.h"
#include "EncoderLocator.h"

using Encoder = void (*)(int, int, const unsigned char *, unsigned char *, bool, bool);
static Encoder original;
static PTP_POOL pool;
static TP_CALLBACK_ENVIRON environment;
static unsigned processors;
static int mode;
static std::atomic_flag busy = ATOMIC_FLAG_INIT;
static std::atomic<bool> rejected(false);
static std::atomic<long long> calls(0), parallelCalls(0), checks(0), mismatches(0), bytes(0), serialTicks(0),
    parallelTicks(0);
static void *FindEncoder()
{
    auto module = GetModuleHandleW(L"UnityPlayer.dll");
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
        return nullptr;
    auto *image = static_cast<unsigned char *>(info.lpBaseOfDll);
    const auto size = info.SizeOfImage;
    if (size < sizeof(IMAGE_DOS_HEADER))
        return nullptr;
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<unsigned long long>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size)
        return nullptr;
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    auto *sections = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<const unsigned char *>(sections) - image +
            static_cast<unsigned long long>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) >
        size)
        return nullptr;
    void *found = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto &section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !(section.Characteristics & IMAGE_SCN_MEM_READ))
            continue;
        const auto offset = section.VirtualAddress, length = section.Misc.VirtualSize;
        if (offset >= size || length > size - offset)
            return nullptr;
        auto match = EncoderLocator::Find(image + offset, length);
        if (match.count > 1 || (match.count && found))
            return nullptr;
        if (match.count)
            found = image + offset + match.offset;
    }
    return found;
}
extern "C" __declspec(dllexport) int ProbeEncoder()
{
    return original || FindEncoder() ? 1 : 0;
}
struct Job
{
    int width, height;
    const unsigned char *input;
    unsigned char *output;
    bool alpha, quality;
};
static void Encode(const Job &j)
{
    original(j.width, j.height, j.input, j.output, j.alpha, j.quality);
}
static void CALLBACK Worker(PTP_CALLBACK_INSTANCE, void *context, PTP_WORK)
{
    Encode(*static_cast<Job *>(context));
}
static long long Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static void Parallel(int w, int h, const unsigned char *input, unsigned char *output, bool alpha, bool quality)
{
    const int rows = (h + 3) / 4, blockBytes = alpha ? 16 : 8;
    const unsigned count = std::min(processors, static_cast<unsigned>(rows / 8));
    if (count < 2)
    {
        original(w, h, input, output, alpha, quality);
        return;
    }
    std::vector<Job> jobs(count);
    std::vector<PTP_WORK> work(count, nullptr);
    for (unsigned i = 0; i < count; i++)
    {
        int first = static_cast<int>((static_cast<long long>(rows) * i) / count);
        int last = static_cast<int>((static_cast<long long>(rows) * (i + 1)) / count);
        jobs[i] = {w,
                   std::min(h, last * 4) - first * 4,
                   input + static_cast<size_t>(first) * 4 * w * 4,
                   output + static_cast<size_t>(first) * ((w + 3) / 4) * blockBytes,
                   alpha,
                   quality};
        if (i)
        {
            work[i] = CreateThreadpoolWork(Worker, &jobs[i], &environment);
            if (work[i])
                SubmitThreadpoolWork(work[i]);
            else
                Encode(jobs[i]);
        }
    }
    Encode(jobs[0]);
    for (unsigned i = 1; i < count; i++)
        if (work[i])
        {
            WaitForThreadpoolWorkCallbacks(work[i], FALSE);
            CloseThreadpoolWork(work[i]);
        }
}
static void Hook(int w, int h, const unsigned char *input, unsigned char *output, bool alpha, bool quality)
{
    calls++;
    if (mode == 1)
    {
        auto started = Now();
        original(w, h, input, output, alpha, quality);
        serialTicks += Now() - started;
        return;
    }
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || static_cast<long long>(w) * h < 262144 || rejected.load() ||
        busy.test_and_set())
    {
        original(w, h, input, output, alpha, quality);
        return;
    }
    struct Release
    {
        ~Release()
        {
            busy.clear();
        }
    } release;
    const size_t size = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16 : 8);
    try
    {
        if (mode == 2)
        {
            std::vector<unsigned char> reference(size);
            auto started = Now();
            original(w, h, input, reference.data(), alpha, quality);
            serialTicks += Now() - started;
            started = Now();
            Parallel(w, h, input, output, alpha, quality);
            parallelTicks += Now() - started;
            checks++;
            bytes += static_cast<long long>(size);
            if (std::memcmp(reference.data(), output, size))
            {
                mismatches++;
                rejected = true;
                std::memcpy(output, reference.data(), size);
            }
        }
        else
        {
            auto started = Now();
            Parallel(w, h, input, output, alpha, quality);
            parallelTicks += Now() - started;
        }
        parallelCalls++;
    }
    catch (...)
    {
        rejected = true;
        original(w, h, input, output, alpha, quality);
    }
}
static bool SelfTest()
{
    const int widths[] = {1, 3, 4, 17, 257, 1024}, heights[] = {65, 67, 513, 1024};
    unsigned random = 0x31415926;
    for (int w : widths)
        for (int h : heights)
        {
            std::vector<unsigned char> input(static_cast<size_t>(w) * h * 4);
            for (auto &value : input)
            {
                random = random * 1664525u + 1013904223u;
                value = static_cast<unsigned char>(random >> 24);
            }
            for (bool alpha : {false, true})
                for (bool quality : {false, true})
                {
                    size_t size = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16 : 8);
                    std::vector<unsigned char> reference(size + 64, 0xa5), actual(size + 64, 0xa5);
                    original(w, h, input.data(), reference.data(), alpha, quality);
                    Parallel(w, h, input.data(), actual.data(), alpha, quality);
                    if (reference != actual)
                        return false;
                }
        }
    return true;
}
extern "C" __declspec(dllexport) int Initialize(int requestedMode)
{
    if (original)
        return -1;
    if (requestedMode < 1 || requestedMode > 3)
        return -2;
    // AI-generated comment: Resolve from the loaded image base; never assume a fixed function RVA.
    // The managed caller also verifies the supported Unity image SHA256.
    void *target = FindEncoder();
    if (!target)
        return -3;
    processors = std::max(1u, static_cast<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    pool = CreateThreadpool(nullptr);
    if (!pool)
        return -5;
    SetThreadpoolThreadMaximum(pool, std::max(1u, processors - 1));
    InitializeThreadpoolEnvironment(&environment);
    SetThreadpoolCallbackPool(&environment, pool);
    if (MH_Initialize() != MH_OK)
        return -6;
    if (MH_CreateHook(target, reinterpret_cast<void *>(Hook), reinterpret_cast<void **>(&original)) != MH_OK)
        return -7;
    // AI-generated comment: Force STB's lazy immutable tables to initialize before worker calls.
    unsigned char input[64] = {}, output[16] = {};
    original(4, 4, input, output, true, false);
    if (requestedMode == 2 && !SelfTest())
        return -9;
    mode = requestedMode;
    if (MH_EnableHook(target) != MH_OK)
        return -8;
    return static_cast<int>(processors);
}
extern "C" __declspec(dllexport) void Counters(long long *result)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    long long values[] = {calls.load(),         parallelCalls.load(), checks.load(),
                          mismatches.load(),    bytes.load(),         serialTicks.load(),
                          parallelTicks.load(), frequency.QuadPart,   rejected.load() ? 1 : 0};
    std::memcpy(result, values, sizeof(values));
}
