#pragma once
#include "../../../SDK/SDK.h"
#include "../../../SDK/Events/Events.h"

enum EVaccinatorResist
{
	VACC_BULLET = 0,
	VACC_BLAST,
	VACC_FIRE,
	VACC_NONE
};

//#define DEBUG_VACCINATOR

class CAutoHeal
{
	bool IsValidHealTarget(CTFPlayer* pLocal, CBaseEntity* pEntity);
	bool ActivateOnVoice(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd);
	void AutoVaccinator(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd);
	void GetDangers(CTFPlayer* pTarget, bool bVaccinator, float& flBulletDanger, float& flBlastDanger, float& flFireDanger);
	void SwapResistType(CUserCmd* pCmd, int iType);
	void ActivateResistType(CUserCmd* pCmd, int iType);
	bool RunAutoUber(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd);
	bool ShouldPop(CTFPlayer* pLocal, CWeaponMedigun* pWeapon);
	bool ShouldPopOnHealth(CTFPlayer* pLocal, CWeaponMedigun* pWeapon);
	bool ShouldPopOnEnemyCount(CTFPlayer* pLocal, CWeaponMedigun* pWeapon);
	bool ShouldPopOnDangerProjectiles(CTFPlayer* pLocal, CWeaponMedigun* pWeapon);
	bool RunVaccinator(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd);
	void SwitchResistance(CWeaponMedigun* pWeapon, CUserCmd* pCmd, EVaccinatorResist eResist);
	EVaccinatorResist GetCurrentResistType(CWeaponMedigun* pWeapon);
	EVaccinatorResist GetBestResistType(CTFPlayer* pLocal);
	EVaccinatorResist GetResistTypeForClass(int nClass);
	std::vector<EVaccinatorResist> GetResistTypesFromNearbyEnemies(CTFPlayer* pLocal, float flRange);
	void CycleToNextResistance(CWeaponMedigun* pWeapon, CUserCmd* pCmd);
	std::vector<EVaccinatorResist> m_vActiveResistances;

	struct DamageRecord_t
	{
		EVaccinatorResist Type;
		float Amount;
		float Time;
	};

	std::deque<DamageRecord_t> m_DamageRecords;
	float m_flLastSwitchTime = 0.0f;
	EVaccinatorResist m_eCurrentAutoResist = VACC_NONE;
	int m_iCurrentResistIndex = 0;
	float m_flLastCycleTime = 0.0f;

	int m_iResistType = -1;
	float m_flChargeLevel = 0.f;
	float m_flSwapTime = 0.f;
	bool m_bPreventResistSwap = false;
	bool m_bPreventResistCharge = false;

	int m_iDamagedType = -1;
	float m_flDamagedDPS = -1;
	float m_flDamagedTime = 0.f;

#ifdef DEBUG_VACCINATOR
	std::vector<std::pair<float, int>> vResistDangers = {};
#endif

public:
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
	void Event(IGameEvent* pEvent, uint32_t uHash, CTFPlayer* pLocal);

	std::unordered_map<int, bool> m_mMedicCallers = {};

#ifdef DEBUG_VACCINATOR
	void Draw(CTFPlayer* pLocal);
#endif
};

ADD_FEATURE(CAutoHeal, AutoHeal);