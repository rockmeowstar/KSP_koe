// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <limits>
#include "TargetFrameRate.h"

class FrameTimeline
{
    double visibleTime{};
    bool active{};
public:
    static double Deadline(double captured,double interval)
    {
        return std::isfinite(captured) && std::isfinite(interval) && captured>0 && interval>0
            ?captured+interval:std::numeric_limits<double>::quiet_NaN();
    }
    static bool HasTime(double now,double deadline,double refresh)
    {
        return std::isfinite(now) && std::isfinite(deadline) && std::isfinite(refresh)
            && now>0 && refresh>0 && now+refresh<deadline;
    }
    static bool CanAnchor(double imageTime,double latestCaptured)
    {
        return std::isfinite(imageTime) && std::isfinite(latestCaptured) && imageTime>0 && imageTime>=latestCaptured;
    }
    static double SampleTime(double before,double after,unsigned phase,unsigned count)
    {
        if(!std::isfinite(before) || !std::isfinite(after) || before<=0 || after<=before || !phase || phase>count || count>TargetFrameRate::MaxAdditionalFrames)
            return std::numeric_limits<double>::quiet_NaN();
        return before+(after-before)*double(phase)/double(count+1);
    }
    bool Newer(double imageTime)const
    {return active && std::isfinite(imageTime) && imageTime>visibleTime;}
    void Shown(double imageTime)
    {
        if(std::isfinite(imageTime) && imageTime>0 && (!active || imageTime>visibleTime))
        {visibleTime=imageTime;active=true;}
    }
    void Reset(){active=false;visibleTime=0;}
};
