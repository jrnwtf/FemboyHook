#include "../SDK/SDK.h"

MAKE_HOOK(UniformRandomStream_RandInt, U::Memory.GetVirtual(I::UniformRandomStream, 2), int, void* rcx, int iMinVal, int iMaxVal)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::UniformRandomStream_RandInt[DEFAULT_BIND])
		return CALL_ORIGINAL(rcx, iMinVal, iMaxVal);
#endif

	if (Vars::Misc::MedalFlip.Value && I::EngineVGui->IsGameUIVisible())
	{
		if (iMinVal == 0 && iMaxVal == 9)
			return 0;
	}

	return CALL_ORIGINAL(rcx, iMinVal, iMaxVal);
}