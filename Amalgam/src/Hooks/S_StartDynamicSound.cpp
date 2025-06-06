#include "../SDK/SDK.h"
#include "../Features/Visuals/Visuals.h"

MAKE_SIGNATURE(S_StartDynamicSound, "engine.dll", "4C 8B DC 57 48 81 EC", 0x0);

MAKE_HOOK(S_StartDynamicSound, S::S_StartDynamicSound(), int, StartSoundParams_t& params)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::S_StartDynamicSound[DEFAULT_BIND])
		return CALL_ORIGINAL(params);
#endif

	F::Visuals.ManualNetwork(params);
	return CALL_ORIGINAL(params);
}