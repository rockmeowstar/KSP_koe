#include "PresentationCadence.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

static unsigned checks = 0;

static void Check(bool condition, const std::string &message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

static void CheckRefreshRates()
{
    const int sourceFrames = 10000;
    for (double baseFps : {10., 20., 30., 36., 40., 44., 48., 60., 75., 120., 144.})
    {
        for (double refreshHz : {60., 75., 120., 144., 165.})
        {
            PresentationCadence cadence;
            int generated = 0;
            for (int frame = 0; frame < sourceFrames; ++frame)
            {
                if (cadence.Midpoint(1 / baseFps, 1 / refreshHz, true))
                    ++generated;
            }

            double expected = std::fmax(0., std::fmin(1., refreshHz / baseFps - 1)) * sourceFrames;
            Check(std::abs(generated - expected) <= 1.01,
                  "At " + std::to_string(baseFps) + " base FPS on a " + std::to_string(refreshHz) +
                      " Hz display, expected about " + std::to_string(expected) + " generated frames; got " +
                      std::to_string(generated) + ".");
        }
    }
}

static void CheckHistoryAndInvalidIntervals()
{
    PresentationCadence cadence;
    Check(!cadence.Midpoint(1. / 48, 1. / 75, true), "The first frame must wait for a spare refresh slot.");
    Check(!cadence.Midpoint(1. / 48, 1. / 75, false), "A rejected source frame must not produce a midpoint.");
    Check(!cadence.Midpoint(1. / 48, 1. / 75, true), "After rejection, the refresh budget starts over.");
    Check(cadence.Midpoint(1. / 48, 1. / 75, true), "The next valid frame has enough budget for a midpoint.");
    Check(!cadence.Midpoint(0, 1. / 75, true), "A zero source interval must be rejected.");
    Check(!cadence.Midpoint(std::numeric_limits<double>::quiet_NaN(), 1. / 75, true),
          "A NaN source interval must be rejected.");
    Check(!cadence.Midpoint(1. / 48, 0, true), "A zero refresh interval must be rejected.");
    Check(cadence.Midpoint(1. / 120, 1. / 75, true, false), "Uncapped presentation can generate a midpoint.");
    Check(!cadence.Midpoint(1. / 120, 1. / 75, false, false), "Uncapped presentation still rejects invalid history.");
    Check(!cadence.Midpoint(1. / 48, 1. / 75, true), "Returning to capped presentation resets its budget.");
}

int main()
{
    try
    {
        CheckRefreshRates();
        CheckHistoryAndInvalidIntervals();
        std::cout << "Frame cadence: " << checks << " checks passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Frame cadence: " << error.what() << '\n';
        return 1;
    }
}
