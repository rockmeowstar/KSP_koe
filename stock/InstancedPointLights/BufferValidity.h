#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

class BufferValidity
{
    static constexpr unsigned char Invalid = 0, Valid = 1;
    std::vector<unsigned char> bytes;
    bool full = false, empty = true;
public:
    void Resize(size_t size) { bytes.assign(size, 0); full = false; empty = true; }
    void Clear(bool fast)
    {
        full = false; empty = true;
        if (!fast) std::fill(bytes.begin(), bytes.end(), Invalid);
    }
    void SetAll(bool fast)
    {
        full = true; empty = false;
        if (!fast) std::fill(bytes.begin(), bytes.end(), Valid);
    }
    void SetRange(size_t begin, size_t end, bool fast)
    {
        if (begin == end) return;
        if (begin == 0 && end == bytes.size()) { SetAll(fast); return; }
        if (fast && full) return;
        if (fast && empty) std::fill(bytes.begin(), bytes.end(), Invalid);
        std::fill(bytes.begin() + begin, bytes.begin() + end, Valid);
        empty = false;
    }
    bool Contains(size_t begin, size_t end, bool fast) const
    {
        if (begin > end || end > bytes.size()) return false;
        if (begin == end) return true;
        if (fast && full) return true;
        if (fast && empty) return false;
        return std::all_of(bytes.begin() + begin, bytes.begin() + end,
                           [](unsigned char value) { return value != 0; });
    }
    bool Full() const { return full; }
};
