#include "../SDK/SDK.h"

std::hash<std::string_view> mHash;

class IInterpolatedVar
{
public:
	virtual ~IInterpolatedVar() = 0;
	virtual void Setup(void* pValue, int type) = 0;
	virtual void SetInterpolationAmount(float seconds) = 0;
	virtual void NoteLastNetworkedValue() = 0;
	virtual bool NoteChanged(float changetime, bool bUpdateLastNetworkedValue) = 0;
	virtual void Reset() = 0;
	virtual int Interpolate(float currentTime) = 0;
	virtual int  GetType() const = 0;
	virtual void RestoreToLastNetworked() = 0;
	virtual void Copy(IInterpolatedVar* pSrc) = 0;
	virtual const char* GetDebugName() = 0;
	virtual void SetDebugName(const char* pName) = 0;
	virtual void SetDebug(bool bDebug) = 0;
};

MAKE_SIGNATURE(CBaseEntity_AddVar, "engine.dll", "55 8B EC 83 EC 10 8B 45 08 53 8B D9 C7 45 ? ? ? ? ? 83 78 20 00 0F 8E ? ? ? ? 33 D2 56 89 55 F8 57", 0x0);

MAKE_HOOK(CBaseEntity_AddVar, S::CBaseEntity_AddVar(), void, void* rcx, void* data, IInterpolatedVar* watcher, int type, bool bSetup)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::CBaseEntity_AddVar[DEFAULT_BIND])
		return CALL_ORIGINAL(rcx, data, watcher, type, bSetup);
#endif

	if (watcher && Vars::Misc::DisableInterpolation.Value)
	{
		const size_t hash = mHash(watcher->GetDebugName());

		static const size_t m_iv_vecVelocity = mHash("CBaseEntity::m_iv_vecVelocity");
		static const size_t m_iv_angEyeAngles = mHash("CTFPlayer::m_iv_angEyeAngles");
		static const size_t m_iv_flPoseParameter = mHash("CBaseAnimating::m_iv_flPoseParameter");
		static const size_t m_iv_flCycle = mHash("CBaseAnimating::m_iv_flCycle");
		static const size_t m_iv_flMaxGroundSpeed = mHash("CMultiPlayerAnimState::m_iv_flMaxGroundSpeed");

		if (hash == m_iv_vecVelocity || hash == m_iv_flPoseParameter || hash == m_iv_flCycle || hash == m_iv_flMaxGroundSpeed || (hash == m_iv_angEyeAngles && ecx == g_EntityCache.GetLocal())) 
		{
			return;
		}
	}

	return CALL_ORIGINAL(rcx, data, watcher, type, bSetup);
}