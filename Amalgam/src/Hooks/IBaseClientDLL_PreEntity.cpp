#include "../SDK/SDK.h"

MAKE_HOOK(BaseClientDLL_PreEntity, U::Memory.GetVirtual(I::BaseClientDLL, 5), void, void* rcx, const char* szMapName)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::BaseClientDLL_PreEntity[DEFAULT_BIND])
		return CALL_ORIGINAL(rcx, szMapName);
#endif

	CALL_ORIGINAL(rcx, szMapName);
}