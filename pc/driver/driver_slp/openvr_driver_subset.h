// Minimal OpenVR driver-side API subset for the SteamVR driver shim.
// SPDX-License-Identifier: MIT
//
// Contains only the symbols needed to implement a bare IServerTrackedDeviceProvider.
// If the full openvr_driver.h from the OpenVR SDK is on the build machine, add its
// path via -DOPENVR_SDK_INCLUDE_DIR and that header will be used instead.
//
// Interface version must match the SteamVR runtime the target machine runs.
// SteamVR 2.x (beta) uses IServerTrackedDeviceProvider_004.

#pragma once

#include <cstdint>

namespace vr {

static const char * const IServerTrackedDeviceProvider_Version =
    "IServerTrackedDeviceProvider_004";

// Minimal EVRInitError values we need.
enum EVRInitError {
    VRInitError_None = 0,
    VRInitError_Init_HmdNotFound = 108,
};

// Opaque context pointer; we don't need to call into it.
struct IVRDriverContext {};

// The interface our driver implements.
class IServerTrackedDeviceProvider {
public:
    virtual EVRInitError Init(IVRDriverContext *ctx) = 0;
    virtual void         Cleanup()                   = 0;
    virtual const char * const * GetInterfaceVersions() = 0;
    virtual void         RunFrame()                  = 0;
    virtual bool         ShouldBlockStandbyMode()    = 0;
    virtual void         EnterStandby()              = 0;
    virtual void         LeaveStandby()              = 0;
    virtual              ~IServerTrackedDeviceProvider() {}
};

// Interface version table sentinel (last entry must be nullptr).
static const char * const k_InterfaceVersions[] = {
    IServerTrackedDeviceProvider_Version,
    nullptr,
};

} // namespace vr
