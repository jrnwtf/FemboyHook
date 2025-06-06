#include "../SDK/SDK.h"

MAKE_SIGNATURE(CBasePlayer_CalcFreezeCamView, "client.dll", "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC ? ? ? ? 56 57 8B F9 8B 07", 0x0);

MAKE_HOOK(CBasePlayer_CalcFreezeCamView, S::CBasePlayer_CalcFreezeCamView(), void, void* rcx, Vector& eyeOrigin, QAngle& eyeAngles, float& fov)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::CBasePlayer_CalcFreezeCamView[DEFAULT_BIND])
		return CALL_ORIGINAL(rcx, eyeOrigin, eyeAngles, fov);
#endif

	CALL_ORIGINAL(rcx, eyeOrigin, eyeAngles, fov);
}