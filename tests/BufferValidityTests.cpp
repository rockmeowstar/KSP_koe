#include "../stock/InstancedPointLights/BufferValidity.h"
#include <cstdio>
#include <cstdlib>
#include <random>

static unsigned checks = 0;
static void Check(bool value) { ++checks; if (!value) { std::puts("BUFFER_VALIDITY_FAIL"); std::exit(1); } }
int main()
{
    std::mt19937 random(17039);
    const size_t sizes[] = {0, 1, 15, 16, 17, 48, 96, 352, 1120, 4096, 65536};
    for (size_t size : sizes)
    {
        BufferValidity baseline, candidate; baseline.Resize(size); candidate.Resize(size);
        std::vector<unsigned char> oracle(size);
        for (unsigned step = 0; step < 5000; ++step)
        {
            size_t begin = random() % (size + 1), end = random() % (size + 1);
            if (begin > end) std::swap(begin, end);
            switch (random() % 4)
            {
            case 0: baseline.Clear(false); candidate.Clear(true); std::fill(oracle.begin(), oracle.end(), 0); break;
            case 1: baseline.SetAll(false); candidate.SetAll(true); std::fill(oracle.begin(), oracle.end(), 1); break;
            default: baseline.SetRange(begin, end, false); candidate.SetRange(begin, end, true); std::fill(oracle.begin() + begin, oracle.begin() + end, 1); break;
            }
            for (unsigned query = 0; query < 8; ++query)
            {
                begin = random() % (size + 1); end = random() % (size + 1); if (begin > end) std::swap(begin, end);
                bool expected = std::find(oracle.begin() + begin, oracle.begin() + end, 0) == oracle.begin() + end;
                Check(baseline.Contains(begin, end, false) == expected);
                Check(candidate.Contains(begin, end, true) == expected);
            }
            Check(!candidate.Contains(size + 1, size + 1, true));
        }
    }
    std::printf("BUFFER_VALIDITY_DIFFERENTIAL_PASS checks=%u\n", checks);
}
