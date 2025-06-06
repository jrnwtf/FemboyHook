#include "../SDK/SDK.h"
#include "../Features/Killstreak/Killstreak.h"

#ifndef TEXTMODE
MAKE_HOOK(CGameEventManager_FireEventClientSide, U::Memory.GetVirtual(I::GameEventManager, 8), bool, IGameEventManager2* rcx, IGameEvent* pEvent)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::CGameEventManager_FireEventClientSide[DEFAULT_BIND])
		return CALL_ORIGINAL(rcx, pEvent);
#endif

	auto uHash = FNV1A::Hash32(pEvent->GetName());

	if (uHash == FNV1A::Hash32Const("player_death"))
	{
		if (Vars::Visuals::Other::KillstreakWeapons.Value)
		{
			F::Killstreak.PlayerDeath(pEvent);
		}
	}

	return CALL_ORIGINAL(rcx, pEvent);
}
#endif