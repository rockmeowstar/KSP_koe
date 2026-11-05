#pragma once
#include <windows.h>
#include <vector>
#include <cwchar>

inline double ReadDisplayRefresh(HWND window)
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    double refresh = EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)
        && mode.dmDisplayFrequency > 1 ? double(mode.dmDisplayFrequency) : 0;
    UINT32 pathCount{}, modeCount{};
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return refresh;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)
        != ERROR_SUCCESS) return refresh;
    for (UINT32 i=0;i<pathCount;++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
            || std::wcscmp(source.viewGdiDeviceName, monitor.szDevice) != 0) continue;
        const auto &rate = paths[i].targetInfo.refreshRate;
        if (rate.Numerator && rate.Denominator) return double(rate.Numerator)/rate.Denominator;
    }
    return refresh;
}
