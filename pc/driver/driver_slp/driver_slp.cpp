// Steam Link Pyrowave - SteamVR driver shim (driver_slp.dll).
// SPDX-License-Identifier: MIT
//
// Registered as a SteamVR driver so vrserver.exe loads it at startup (no injector needed).
// Its only job is to load SteamFramePyrowaveBackend.dll from the same bin\win64 folder; the
// backend's DllMain starts the scan that patches driver_vrlink's NVENC / D3D11 imports when
// vrlink is loaded. The driver adds no tracked devices.
//
// Off switch without uninstalling: %TEMP%\slp_driver_disabled (checked at SteamVR start).
// Register / unregister: Install-SlpDriver.ps1 / Uninstall-SlpDriver.ps1 (vrpathreg, no admin).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "openvr_driver.h"

static HMODULE g_self;

static void dbg(const char *msg)
{
	OutputDebugStringA(msg);
	char path[MAX_PATH + 32];
	DWORD n = GetTempPathA(MAX_PATH, path);
	if (!n || n >= MAX_PATH)
		return;
	strcat(path, "slp_driver.log");
	if (FILE *f = fopen(path, "a"))
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
		fclose(f);
	}
}

static bool temp_flag(const char *name)
{
	char path[MAX_PATH + 64];
	DWORD n = GetTempPathA(MAX_PATH, path);
	if (!n || n >= MAX_PATH)
		return false;
	strcat(path, name);
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

class SlpDriverProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init(vr::IVRDriverContext *ctx) override
	{
		VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
		if (temp_flag("slp_driver_disabled"))
		{
			dbg("[slp-driver] %TEMP%\\slp_driver_disabled present: backend not loaded");
			return vr::VRInitError_None;
		}
		if (GetModuleHandleW(L"SteamFramePyrowaveBackend.dll"))
		{
			dbg("[slp-driver] backend already loaded (injector?), not loading a second copy");
			return vr::VRInitError_None;
		}
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
		if (!n || n >= MAX_PATH)
			return vr::VRInitError_None;
		wchar_t *slash = wcsrchr(path, L'\\');
		if (!slash)
			return vr::VRInitError_None;
		slash[1] = 0;
		if (wcslen(path) + 40 >= MAX_PATH)
			return vr::VRInitError_None;
		wcscat(path, L"SteamFramePyrowaveBackend.dll");
		// ALTERED_SEARCH_PATH: libpyrowave-shared-1.dll is found next to the backend.
		m_backend = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		char msg[512];
		if (m_backend)
			snprintf(msg, sizeof(msg), "[slp-driver] loaded %ls", path);
		else
			snprintf(msg, sizeof(msg), "[slp-driver] could not load %ls (error %lu); SteamVR continues without Pyrowave", path,
			         GetLastError());
		dbg(msg);
		// v30: hand the backend SteamVR's driver-input interface (the one vrlink reports the Frame's gaze to), so it
		// can read the gaze instead of guessing it from the picture.
		if (m_backend)
		{
			using set_fn = void (*)(void *);
			auto set = reinterpret_cast<set_fn>(reinterpret_cast<void *>(GetProcAddress(m_backend, "slp_set_driver_input")));
			vr::EVRInitError e = vr::VRInitError_None;
			void *input = vr::VRDriverContext()->GetGenericInterface("IVRDriverInput_005", &e);
			snprintf(msg, sizeof(msg), "[slp-driver] IVRDriverInput_005 %p (error %d), backend hook %s", input, (int)e,
			         set ? "found" : "missing");
			dbg(msg);
			if (set && input && e == vr::VRInitError_None)
				set(input);
		}
		// Never fail SteamVR start because of us.
		return vr::VRInitError_None;
	}

	void Cleanup() override
	{
		// The backend's hooks stay installed for the life of vrserver (unpatching while vrlink may
		// still call through them is unsafe), so the backend is deliberately not unloaded.
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}

	const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
	void RunFrame() override {}
	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}

private:
	HMODULE m_backend = nullptr;
};

static SlpDriverProvider g_provider;

extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *interface_name, int *return_code)
{
	if (interface_name && strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0)
	{
		if (return_code)
			*return_code = vr::VRInitError_None;
		return &g_provider;
	}
	if (return_code)
		*return_code = vr::VRInitError_Init_InterfaceNotFound;
	return nullptr;
}

BOOL WINAPI DllMain(HINSTANCE hmod, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hmod);
		g_self = hmod;
	}
	return TRUE;
}
