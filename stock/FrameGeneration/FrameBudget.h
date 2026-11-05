#pragma once
#include <algorithm>
#include <cmath>

namespace FrameBudget
{
inline double Input(double captured, double interval, double now)
{
    if (!std::isfinite(captured) || !std::isfinite(interval) || !std::isfinite(now) || interval <= 0 || now < captured)
        return 0;
    return std::max(0.0, captured + interval - now);
}

inline double Decision(double captured, double interval, double ready, double now)
{
    if (!std::isfinite(ready) || ready < captured || now < ready)
        return 0;
    if (!std::isfinite(captured) || !std::isfinite(interval) || !std::isfinite(now) || interval <= 0)
        return 0;
    // AI-generated comment: Reserve one source interval for GPU input readiness and half for processing.
    double deadline = std::min(ready + interval * .5, captured + interval * 1.5);
    return std::max(0.0, deadline - now);
}
}
