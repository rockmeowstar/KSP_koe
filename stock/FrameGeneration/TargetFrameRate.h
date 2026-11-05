#pragma once
#include <algorithm>
#include <cmath>
namespace TargetFrameRate
{
constexpr int Default = 60;
constexpr int Minimum = 60;
constexpr unsigned MaxAdditionalFrames = 3;
inline int Clamp(int requested, int refresh)
{
    return std::min(std::max(Minimum, requested), refresh > 0 ? refresh : Default);
}
inline double Effective(int requested, double refresh)
{
    return std::min(double(std::max(Minimum, requested)),
        std::isfinite(refresh) && refresh > 0 ? refresh : double(Default));
}
class Planner
{
    double credit = 0;
    double previousTarget = 0;
  public:
    unsigned Count(double interval, double target, bool eligible, unsigned capacity)
    {
        capacity = std::min(capacity, MaxAdditionalFrames);
        if (!eligible || !std::isfinite(interval) || !std::isfinite(target) || interval <= 0
            || interval > .25 || target <= 0 || !capacity)
        {
            credit = 0;
            previousTarget = 0;
            return 0;
        }
        if (target != previousTarget) credit = 0;
        previousTarget = target;
        double additional = std::max(0., target*interval-1);
        if (!additional) { credit = 0; return 0; }
        credit += additional;
        auto count = static_cast<unsigned>(std::min(std::floor(credit+1e-9), double(capacity)));
        credit = std::min(credit-count, 1.0);
        return count;
    }
};
inline double Effective(bool fullGeneration, double requested, double refresh)
{
    double display = std::isfinite(refresh) && refresh > 0 ? refresh : double(Default);
    double target = fullGeneration && std::isfinite(requested) && requested > 0 ? requested : double(Default);
    return std::min(target, display);
}
class PresentationClock
{
    double next = 0;
  public:
    bool Ready(double now) const { return now >= next; }
    void Presented(double now, double target)
    {
        double period = 1/target;
        if (!next || now-next>period) next=now;
        next+=period;
    }
    void Reset() { next = 0; }
};
}
