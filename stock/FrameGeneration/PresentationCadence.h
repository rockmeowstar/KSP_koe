#pragma once
#include <cmath>

// AI-generated comment: Keep every possible real presentation. Interpolate only into spare refresh
// slots; always presenting two images per input caps real output at refresh/2.
class PresentationCadence
{
    double credit = 0;

  public:
    bool Midpoint(double interval, double refresh, bool eligible, bool adaptive = true)
    {
        if (!adaptive)
        {
            credit = 0;
            return eligible;
        }
        if (!eligible || !std::isfinite(interval) || !std::isfinite(refresh) || interval <= 0 || refresh <= 0)
        {
            credit = 0;
            return false;
        }
        double slots = interval / refresh;
        if (slots >= 2)
        {
            credit = 0;
            return true;
        }
        if (slots <= 1)
        {
            credit = 0;
            return false;
        }
        credit += slots - 1;
        if (credit >= 1)
        {
            credit -= 1;
            return true;
        }
        return false;
    }
};
