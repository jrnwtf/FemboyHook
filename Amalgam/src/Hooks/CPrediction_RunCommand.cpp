#include "../SDK/SDK.h"

std::vector<Vec3> vAngles;

MAKE_HOOK(CPrediction_RunCommand, U::Memory.GetVirtual(I::Prediction, 17), void, void* rcx, CTFPlayer* pPlayer, CUserCmd* pCmd, IMoveHelper* moveHelper)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::CPrediction_RunCommand[DEFAULT_BIND])
		return CALL_ORIGINAL(rcx, pPlayer, pCmd, moveHelper);
#endif

	CALL_ORIGINAL(rcx, pPlayer, pCmd, moveHelper);

	if (pPlayer != H::Entities.GetLocal() || G::Recharge || pCmd->hasbeenpredicted)
		return;

	auto pAnimState = pPlayer->GetAnimState();
	vAngles.push_back(G::ViewAngles);

	if (!pAnimState || G::Choking)
		return;

	for (auto& vAngle : vAngles)
	{
		pAnimState->Update(vAngle.y, vAngle.x);
		pPlayer->FrameAdvance(TICK_INTERVAL);
	}

	vAngles.clear();
}