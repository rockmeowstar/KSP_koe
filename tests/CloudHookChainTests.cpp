#include "../mod/EnvironmentalVisualEnhancements/ShaderCorrectionGuards/CloudHookChain.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace CloudHookChain;
using Function = int (*)(int);
static Function original;
static std::vector<int> order;
static int Driver(int value) { order.push_back(3); return value * 2; }
static int Maps(int value) { order.push_back(2); return Driver(value); }
static int Fusion(int value) { order.push_back(1); int result = original(value); order.push_back(4); return result; }
static void Check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static std::atomic<unsigned> driverCalls{}, fusionCalls{};
static Function concurrentOriginal;
static int ConcurrentMaps(int value) { ++driverCalls; return value + 17; }
static int ConcurrentFusion(int value) { ++fusionCalls; return concurrentOriginal(value); }

int main()
{
    try
    {
        int target{}, other{};
        Registry testChain;
        Endpoint entry;
        Check(testChain.Add(entry, &target, reinterpret_cast<void *>(Maps)), "first owner registration");
        Check(!testChain.Add(entry, &target, reinterpret_cast<void *>(Maps)), "duplicate owner rejected");
        Check(Invoke(entry, Maps, 21)==42 && order==std::vector<int>({2,3}), "unchanged base dispatch");
        order.clear();
        Check(testChain.Stage(&target, reinterpret_cast<void *>(Fusion), reinterpret_cast<void **>(&original))==StageResult::Staged, "second owner stages without a second physical hook");
        Check(original==Maps, "fusion forwards into map processing");
        Check(Invoke(entry, Maps, 21)==42 && order==std::vector<int>({2,3}), "uncommitted owner stays invisible");
        testChain.Cancel(); order.clear();
        Check(Invoke(entry, Maps, 21)==42 && order==std::vector<int>({2,3}), "rollback preserves first owner");
        Check(testChain.Stage(&target, reinterpret_cast<void *>(Fusion), reinterpret_cast<void **>(&original))==StageResult::Staged, "retry after rollback");
        testChain.Commit(); order.clear();
        Check(Invoke(entry, Maps, 21)==42 && order==std::vector<int>({1,2,3,4}), "boundary then maps then exactly one original draw");
        Check(!testChain.Remove(&target), "published owner cannot be removed beneath a callback");
        void *sentinel = &other;
        Check(testChain.Stage(&target, reinterpret_cast<void *>(Fusion), &sentinel)==StageResult::Conflict && sentinel==&other, "third owner cannot overwrite chain");
        Check(testChain.Stage(&other, reinterpret_cast<void *>(Fusion), &sentinel)==StageResult::NotOwned && sentinel==&other, "unowned target remains untouched");
        Endpoint pending;
        Check(testChain.Add(pending,&other,reinterpret_cast<void *>(Maps)), "independent owner");
        Check(testChain.Remove(&other), "unpublished failed registration can be removed");

        Registry concurrent;
        Endpoint running;
        Check(concurrent.Add(running,&other,reinterpret_cast<void *>(ConcurrentMaps)), "concurrent owner");
        std::atomic<bool> stop{}, bad{};
        std::thread reader([&] { while (!stop.load()) if (Invoke(running,ConcurrentMaps,25)!=42) bad=true; });
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(driverCalls.load()<1000 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        auto staged=concurrent.Stage(&other,reinterpret_cast<void *>(ConcurrentFusion),reinterpret_cast<void **>(&concurrentOriginal));
        concurrent.Commit();
        while(fusionCalls.load()<1000 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        stop=true; reader.join();
        Check(staged==StageResult::Staged && !bad && fusionCalls>=1000 && driverCalls>=fusionCalls, "atomic publication preserves results during concurrent dispatch");
        std::puts("CLOUD_HOOK_CHAIN_PASS order, single original call, staged rollback, conflict rejection, lifetime and concurrent publication");
        return 0;
    }
    catch(const std::exception &error) { std::printf("FAIL: %s\n",error.what()); return 1; }
}
