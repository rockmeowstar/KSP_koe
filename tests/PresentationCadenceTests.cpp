#include "PresentationCadence.h"
#include "FrameBudget.h"
#include "TargetFrameRate.h"
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
        for (double base : {10.,20.,30.,44.,60.,75.,120.})
            for (double display : {50.,59.94,60.,75.,144.})
                for (int request : {30,60,75,144})
                {
                    TargetFrameRate::Planner planner;
                    double target = TargetFrameRate::Effective(request,display);
                    unsigned generated = 0;
                    for (int i=0;i<10000;++i) generated += planner.Count(1/base,target,true,100);
                    double expected = std::min(3.,std::max(0.,target/base-1))*10000;
                    Check(std::abs(generated-expected)<=1.01,"Target planner missed its requested output rate");
                    Check(target<=display,"Target exceeds the active monitor rate");
                }
        TargetFrameRate::Planner phases;
        Check(phases.Count(.05,75,true,10)==2,"20 -> 75 did not schedule multiple phases");
        Check(phases.Count(.05,75,true,10)==3,"20 -> 75 lost fractional phase credit");
        Check(phases.Count(.05,75,false,10)==0,"Inactive output generated phases");
        Check(phases.Count(.05,60,true,10)==2,"20 -> 60 did not schedule two extra frames");
        Check(phases.Count(.05,75,true,1)==1,"Phase plan exceeded its texture budget");
        Check(phases.Count(.5,75,true,100)==0,"A loading pause generated catch-up frames");
        Check(phases.Count(.05,75,true,0)==0,"A zero texture budget generated phases");
        Check(phases.Count(.2,1000,true,1000)==3,"The hard three-frame limit was bypassed");
        Check(TargetFrameRate::Effective(false,144,144)==60,"Default conservative mode did not target 60 FPS");
        Check(TargetFrameRate::Effective(false,144,50)==50,"Conservative mode exceeded a slow monitor");
        Check(TargetFrameRate::Effective(true,144,75)==75,"Full mode exceeded monitor refresh");
        Check(TargetFrameRate::Effective(true,120,144)==120,"Full mode ignored its configured target");
        TargetFrameRate::PresentationClock clock;
        Check(clock.Ready(1),"New presentation clock is not ready");
        clock.Presented(1,60);
        Check(!clock.Ready(1.01),"Presentation was allowed before the target period");
        Check(clock.Ready(1.017),"Presentation missed its next target period");
        clock.Presented(1.017,60);
        Check(!clock.Ready(1.025),"Presentation clock repeated a consumed slot");
        clock.Reset();
        Check(clock.Ready(1.025),"Hidden output retained an old pacing deadline");
        CheckHistoryAndInvalidIntervals();
        Check(std::abs(FrameBudget::Input(1, .04, 1.01) - .03) < 1e-12, "GPU input budget was charged twice");
        Check(FrameBudget::Input(1, .04, 1.05) == 0, "GPU input wait exceeded one source interval");
        Check(std::abs(FrameBudget::Decision(1, .04, 1.03, 1.035) - .015) < 1e-12, "GPU-ready input lost its processing budget");
        Check(std::abs(FrameBudget::Decision(1, .04, 1.05, 1.055) - .005) < 1e-12, "Late input escaped the total age limit");
        Check(FrameBudget::Decision(1, .04, 1.07, 1.07) == 0, "Stale input received a new deadline");
        for (double invalid : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
            Check(FrameBudget::Decision(1, invalid, 1.01, 1.01) == 0, "Invalid interval received a budget");
        std::cout << "Frame cadence: " << checks << " checks passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Frame cadence: " << error.what() << '\n';
        return 1;
    }
}
