#include "AutoHeal.h"

#include "../../Players/PlayerUtils.h"
#include "../../Backtrack/Backtrack.h"
#include "../../CritHack/CritHack.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"
#include "../AimbotProjectile/AimbotProjectile.h"
#include "../../NavBot/FollowBot.h"

bool CAutoHeal::IsValidHealTarget(CTFPlayer* pLocal, CBaseEntity* pEntity)
{
	if (!pEntity)
		return false;
	
	if (!pEntity->IsPlayer())
		return false;
	
	CTFPlayer* pTarget = pEntity->As<CTFPlayer>();
	if (!pTarget || !pTarget->IsAlive())
		return false;
	
	if (Vars::Aimbot::Healing::FriendsOnly.Value && 
	    !H::Entities.IsFriend(pTarget->entindex()) && 
	    !H::Entities.InParty(pTarget->entindex()))
		return false;
	
	if (Vars::Aimbot::Healing::FBotTargetOnly.Value)
	{
		if (F::FollowBot.m_iTargetIndex == -1)
			return false;
		
		return (pTarget->entindex() == F::FollowBot.m_iTargetIndex);
	}
	
	return true;
}

void CAutoHeal::ActivateOnVoice(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd)
{
	if (!Vars::Aimbot::Healing::ActivateOnVoice.Value)
		return;

	auto pTarget = pWeapon->m_hHealingTarget().Get();
	if (!IsValidHealTarget(pLocal, pTarget))
		return false;

	if (m_mMedicCallers.contains(pTarget->entindex()))
		pCmd->buttons |= IN_ATTACK2;
}

static inline Vec3 PredictOrigin(Vec3& vOrigin, Vec3 vVelocity, float flLatency, bool bTrace = true, Vec3 vMins = {}, Vec3 vMaxs = {}, unsigned int nMask = MASK_SOLID, float flNormal = 0.f)
{
	if (vVelocity.IsZero() || !flLatency)
		return vOrigin;

	Vec3 vTo = vOrigin + vVelocity * flLatency;
	if (!bTrace)
		return vTo;

	CGameTrace trace = {};
	CTraceFilterWorldAndPropsOnly filter = {};

	SDK::TraceHull(vOrigin, vTo, vMins, vMaxs, nMask, &filter, &trace);
#ifdef DEBUG_VACCINATOR
	G::SweptStorage.emplace_back(std::pair<Vec3, Vec3>(trace.startpos, trace.endpos), vMins, vMaxs, Vec3(), I::GlobalVars->curtime + 60.f, Color_t(255, 255, 255, 10), true);
#endif
	Vec3 vOrigin2 = vOrigin + (vTo - vOrigin) * trace.fraction;
	if (trace.DidHit() && flNormal)
		vOrigin2 += trace.plane.normal * flNormal;
	return vOrigin2;
}

static inline bool TraceToEntity(CTFPlayer* pPlayer, CBaseEntity* pEntity, Vec3& vPlayerOrigin, Vec3& vEntityOrigin, unsigned int nMask = MASK_SHOT | CONTENTS_GRATE)
{
	CGameTrace trace = {};
	CTraceFilterWorldAndPropsOnly filter = {};

	if (pEntity->IsPlayer())
	{
		Vec3 vMins = pPlayer->m_vecMins() + 1;
		Vec3 vMaxs = pPlayer->m_vecMaxs() - 1;
		Vec3 vMinsS = (vMins - vMaxs) / 2;
		Vec3 vMaxsS = (vMaxs - vMins) / 2;

		std::vector<Vec3> vPoints = {
			Vec3(),
			Vec3(vMinsS.x, vMinsS.y, vMaxsS.z),
			Vec3(vMaxsS.x, vMinsS.y, vMaxsS.z),
			Vec3(vMinsS.x, vMaxsS.y, vMaxsS.z),
			Vec3(vMaxsS.x, vMaxsS.y, vMaxsS.z),
			Vec3(vMinsS.x, vMinsS.y, vMinsS.z),
			Vec3(vMaxsS.x, vMinsS.y, vMinsS.z),
			Vec3(vMinsS.x, vMaxsS.y, vMinsS.z),
			Vec3(vMaxsS.x, vMaxsS.y, vMinsS.z)
		};
		for (auto& vPoint : vPoints)
		{
			SDK::Trace(vPlayerOrigin + vPoint, vEntityOrigin, nMask, &filter, &trace);
#ifdef DEBUG_VACCINATOR
			if (trace.fraction == 1.f)
				G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(trace.startpos, trace.endpos), I::GlobalVars->curtime + 60.f, Color_t(0, 255, 0, 25), true);
#endif
			if (trace.fraction == 1.f)
				return true;
		}
	}
	else
	{
		SDK::Trace(vPlayerOrigin, vEntityOrigin, nMask, &filter, &trace);
#ifdef DEBUG_VACCINATOR
		if (trace.fraction == 1.f)
			G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(trace.startpos, trace.endpos), I::GlobalVars->curtime + 60.f, Color_t(0, 255, 0, 25), true);
#endif
		return trace.fraction == 1.f;
	}

	return false;
}

static inline int GetShotsWithinTime(int iWeaponID, float flFireRate, float flTime)
{
	int iTicks = TIME_TO_TICKS(flTime);

	int iDelay = 1;
	switch (iWeaponID)
	{
	case TF_WEAPON_MINIGUN:
	case TF_WEAPON_PIPEBOMBLAUNCHER:
	case TF_WEAPON_CANNON:
		iDelay = 2;
	}

	return 1 + (iTicks - iDelay) / std::ceilf(flFireRate / TICK_INTERVAL);
}

void CAutoHeal::GetDangers(CTFPlayer* pTarget, bool bVaccinator, float& flBulletOut, float& flBlastOut, float& flFireOut)
{
	if (pTarget->IsInvulnerable())
		return;

	float flBulletDanger = 0.f, flBlastDanger = 0.f, flFireDanger = 0.f;
	float flBulletResist = pTarget->InCond(TF_COND_BULLET_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) ? 0.75f : pTarget->InCond(TF_COND_MEDIGUN_SMALL_BULLET_RESIST) ? -0.1f : 0.f;
	float flBlastResist = pTarget->InCond(TF_COND_BLAST_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST) ? 0.75f : pTarget->InCond(TF_COND_MEDIGUN_SMALL_BLAST_RESIST) ? -0.1f : 0.f;
	float flFireResist = pTarget->InCond(TF_COND_FIRE_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST) ? 0.75f : pTarget->InCond(TF_COND_MEDIGUN_SMALL_FIRE_RESIST) ? -0.1f : 0.f;

	float flLatency = F::Backtrack.GetReal();
	int iPlayerHealth = pTarget->m_iHealth();
	Vec3 vTargetOrigin = PredictOrigin(pTarget->m_vecOrigin(), pTarget->m_vecVelocity(), pTarget->entindex() != I::EngineClient->GetLocalPlayer() ? flLatency : 0.f, true, pTarget->m_vecMins(), pTarget->m_vecMaxs(), pTarget->SolidMask());
	Vec3 vTargetCenter = vTargetOrigin + pTarget->GetOffset() / 2;
	Vec3 vTargetEye = vTargetOrigin + pTarget->GetViewOffset();

	for (auto pEntity : H::Entities.GetGroup(EGroupType::PLAYERS_ENEMIES))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		int iIndex = pPlayer->entindex();
		if (pPlayer->IsDormant() || !pPlayer->CanAttack(true, false))
			continue;

		auto pWeapon = pPlayer->m_hActiveWeapon().Get()->As<CTFWeaponBase>();
		int nWeaponID = pWeapon ? pWeapon->GetWeaponID() : 0;
		auto eWeaponType = SDK::GetWeaponType(pWeapon);
		if (eWeaponType == EWeaponType::UNKNOWN || eWeaponType == EWeaponType::MELEE // if we ever want to port this over to auto uber or something, handle melee
			|| nWeaponID == TF_WEAPON_DRG_POMSON || nWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER
			|| nWeaponID == TF_WEAPON_MINIGUN && !pPlayer->InCond(TF_COND_AIMING))
			continue;

		Vec3 vPlayerOrigin = PredictOrigin(pPlayer->m_vecOrigin(), pPlayer->m_vecVelocity(), flLatency, true, pPlayer->m_vecMins(), pPlayer->m_vecMaxs(), pPlayer->SolidMask());
		Vec3 vPlayerEye = vPlayerOrigin + pPlayer->GetViewOffset();

		bool bCheater = F::PlayerUtils.HasTag(iIndex, F::PlayerUtils.TagToIndex(CHEATER_TAG));
		bool bZoom = pPlayer->InCond(TF_COND_ZOOMED);
		float flFOV = Math::CalcFov(H::Entities.GetEyeAngles(iIndex) + H::Entities.GetPingAngles(iIndex), Math::CalcAngle(vPlayerEye, bZoom ? vTargetEye : vTargetCenter));
		bool bFOV = bCheater || flFOV < (bZoom ? 10 : eWeaponType == EWeaponType::HITSCAN ? 30 : 90);
		if (!bFOV || !TraceToEntity(pTarget, pPlayer, vTargetCenter, vPlayerEye))
			continue;

		bool bCrits = pPlayer->IsCritBoosted() || F::CritHack.WeaponCanCrit(pWeapon);
		bool bMinicrits = pPlayer->IsMiniCritBoosted() || pTarget->IsMarked();
		float flDistance = vPlayerEye.DistTo(vTargetCenter);
		float flDamage, flMult = bCrits ? 3.f : bMinicrits ? 1.36f : 1.f;
		float flFireRate = std::min(pWeapon->GetFireRate(), 1.f);
		int iBulletCount = pWeapon->GetBulletsPerShot();
		int iShotsWithinTime = GetShotsWithinTime(nWeaponID, flFireRate, flLatency + TICKS_TO_TIME(bCheater ? 22 : 0));
		switch (nWeaponID)
		{
		case TF_WEAPON_SNIPERRIFLE:
		case TF_WEAPON_SNIPERRIFLE_DECAP:
		case TF_WEAPON_SNIPERRIFLE_CLASSIC:
		{
			bool bClassic = nWeaponID == TF_WEAPON_SNIPERRIFLE_CLASSIC;
			bool bHeadshot = pWeapon->As<CTFSniperRifle>()->GetRifleType() != RIFLE_JARATE;
			bool bPiss = SDK::AttribHookValue(0, "jarate_duration", pWeapon) > 0;
			auto GetSniperDot = [](CBaseEntity* pEntity) -> CSniperDot*
				{
					for (auto pDot : H::Entities.GetGroup(EGroupType::MISC_DOTS))
					{
						if (pDot->m_hOwnerEntity().Get() == pEntity)
							return pDot->As<CSniperDot>();
					}
					return nullptr;
				};
			if (CSniperDot* pPlayerDot = GetSniperDot(pEntity))
			{
				float flChargeTime = std::max(SDK::AttribHookValue(3.f, "mult_sniper_charge_per_sec", pWeapon), 1.5f);
				flDamage = Math::RemapVal(TICKS_TO_TIME(I::ClientState->m_ClockDriftMgr.m_nServerTick) - pPlayerDot->m_flChargeStartTime() - 0.3f, 0.f, flChargeTime, 50.f, 150.f);
				if (bClassic && flDamage != 150.f)
					flDamage = SDK::AttribHookValue(flDamage, "bodyshot_damage_modify", pWeapon);
				else
					flMult = std::max(bHeadshot || bClassic ? 3.f : bPiss ? 1.36f : 1.f, flMult);
				break;
			}
			if (SDK::AttribHookValue(0, "sniper_only_fire_zoomed", pWeapon) && !bZoom)
				flDamage = 0.f;
			else if (bClassic || !bZoom)
				flDamage = SDK::AttribHookValue(50.f, "bodyshot_damage_modify", pWeapon);
			else
				flDamage = 150.f, flMult = flMult = std::max(bHeadshot || bClassic ? 3.f : bPiss ? 1.36f : 1.f, flMult);
			break;
		}
		case TF_WEAPON_COMPOUND_BOW:
			flDamage = 120.f, flMult = 3.f;
			break;
		case TF_WEAPON_FLAMETHROWER:
			flDamage = 80.f * flFireRate;
			break;
		default:
			flDamage = pWeapon->GetDamage(false);
		}

		switch (eWeaponType)
		{
		case EWeaponType::HITSCAN:
		{
			flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist;
			flDamage *= flMult * (pWeapon ? SDK::AttribHookValue(1.f, "mult_dmg", pWeapon) : 1);

			float flSpread = std::clamp(pWeapon->GetWeaponSpread(), 0.001f, 1.f);
			float flMappedCount = Math::RemapVal(flDistance, 20 / flSpread, 100 / flSpread, iBulletCount, 1);
			float flDamageInLatency = flDamage * flMappedCount * iShotsWithinTime;
			float flDamageDanger = flDamageInLatency / iPlayerHealth;
			float flDistanceDanger = Math::RemapVal(flDistance, 100, 100 / flSpread, 1.f, 0.001f);
			if (bCheater) // may use seed pred +/ crits
				flDistanceDanger = std::max(flDistanceDanger, bCrits ? 1.f : 0.34f);

#ifdef DEBUG_VACCINATOR
			//SDK::Output("Hitscan", std::format("{}, {}, {}, {}, {}", flDamage, flSpread, flDamageInLatency, flDamageDanger, flDistanceDanger).c_str(), { 255, 200, 200 }, Vars::Debug::Logging.Value);
#endif
			flBulletDanger += flDamageDanger * flDistanceDanger;
			break;
		}
		case EWeaponType::PROJECTILE:
		{
			ProjectileInfo tProjInfo = {};
			if (!F::ProjSim.GetInfo(pPlayer, pWeapon, {}, tProjInfo, ProjSimEnum::NoRandomAngles | ProjSimEnum::MaxSpeed))
				continue;

			int iType = MEDIGUN_BLAST_RESIST;
			switch (nWeaponID)
			{
			case TF_WEAPON_COMPOUND_BOW:
			case TF_WEAPON_CROSSBOW:
			case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
			case TF_WEAPON_SYRINGEGUN_MEDIC:
			case TF_WEAPON_RAYGUN: iType = MEDIGUN_BULLET_RESIST; break;
			case TF_WEAPON_FLAMETHROWER: // maybe just test full range?
			case TF_WEAPON_FLAME_BALL: iType = MEDIGUN_FIRE_RESIST; break;
			}

			switch (iType)
			{
			case MEDIGUN_BULLET_RESIST: flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist; break;
			case MEDIGUN_BLAST_RESIST: flMult = flBlastResist > 0.f ? 1.f - flBlastResist : flMult + flBlastResist; break;
			case MEDIGUN_FIRE_RESIST: flMult = flFireResist > 0.f ? 1.f - flFireResist : flMult + flFireResist; break;
			}
			flDamage *= flMult * (pWeapon ? SDK::AttribHookValue(1.f, "mult_dmg", pWeapon) : 1);

			float flRadius = tProjInfo.m_flVelocity * flLatency + F::AimbotProjectile.GetSplashRadius(pWeapon, pPlayer) + pTarget->GetSize().Length() / 2;
			float flDamageInLatency = flDamage * iBulletCount * iShotsWithinTime;
			float flDamageDanger = flDamageInLatency / iPlayerHealth;
			float flDistanceDanger = Math::RemapVal(flDistance, flRadius, flRadius, 1.f, 0.001f);

#ifdef DEBUG_VACCINATOR
			SDK::Output("Projectile", std::format("{}, {}, {}, {}", flDamage, flDamageInLatency, flDamageDanger, flDistanceDanger).c_str(), { 255, 200, 200 }, Vars::Debug::Logging.Value);
#endif
			switch (iType)
			{
			case MEDIGUN_BULLET_RESIST: flBulletDanger += flDamageDanger * flDistanceDanger; break;
			case MEDIGUN_BLAST_RESIST: flBlastDanger += flDamageDanger * flDistanceDanger; break;
			case MEDIGUN_FIRE_RESIST: flFireDanger += flDamageDanger * flDistanceDanger; break;
			}
		}
		}
	}

	for (auto pEntity : H::Entities.GetGroup(EGroupType::BUILDINGS_ENEMIES))
	{
		auto pSentry = pEntity->As<CObjectSentrygun>();
		if (pSentry->GetClassID() != ETFClassID::CObjectSentrygun)
			continue;

		if (pSentry->m_hEnemy().Get() != pTarget && pSentry->m_hAutoAimTarget().Get() != pTarget || !pSentry->m_iAmmoShells())
			continue;

		float flDistance = pSentry->GetCenter().DistTo(vTargetCenter);
		float flDamage = 19.f, flMult = pTarget->IsMarked() ? 1.36f : 1.f;
		float flFireRate;
		switch (pSentry->m_bMiniBuilding() ? 0 : pSentry->m_iUpgradeLevel())
		{
		case 0:
			flDamage /= 2;
			flFireRate = pSentry->m_bPlayerControlled() ? 0.09f : 0.18f;
			break;
		case 1:
			flFireRate = pSentry->m_bPlayerControlled() ? 0.135f : 0.225f;
			break;
		default:
			flFireRate = pSentry->m_bPlayerControlled() ? 0.09f : 0.135f;
		}
		int iShotsWithinTime = GetShotsWithinTime(0, flFireRate, 1.f /*flLatency*/);
		flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist;
		flDamage *= flMult;

		float flDamageInLatency = flDamage * iShotsWithinTime;
		float flDamageDanger = flDamageInLatency / iPlayerHealth;
		float flDistanceDanger = Math::RemapVal(flDistance, 1100.f, 3200.f, 1.f, 0.001f);

#ifdef DEBUG_VACCINATOR
		SDK::Output("Sentry", std::format("{}, {}, {}, {}", flDamage, flDamageInLatency, flDamageDanger, flDistanceDanger).c_str(), { 255, 200, 200 }, Vars::Debug::Logging.Value);
#endif
		flBulletDanger += flDamageDanger * flDistanceDanger;
	}

	for (auto pEntity : H::Entities.GetGroup(EGroupType::WORLD_PROJECTILES))
	{
		CTFPlayer* pOwner = nullptr;
		CTFWeaponBase* pWeapon = nullptr;

		switch (pEntity->GetClassID())
		{
		case ETFClassID::CTFGrenadePipebombProjectile:
		case ETFClassID::CTFWeaponBaseMerasmusGrenade:
		case ETFClassID::CTFProjectile_SpellMeteorShower:
			pWeapon = pEntity->As<CTFGrenadePipebombProjectile>()->m_hOriginalLauncher().Get()->As<CTFWeaponBase>();
			pOwner = pEntity->As<CTFWeaponBaseGrenadeProj>()->m_hThrower().Get()->As<CTFPlayer>();
			break;
		case ETFClassID::CTFProjectile_Arrow:
		case ETFClassID::CTFProjectile_HealingBolt:
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_SentryRocket:
		case ETFClassID::CTFProjectile_BallOfFire:
		case ETFClassID::CTFProjectile_SpellFireball:
		case ETFClassID::CTFProjectile_SpellLightningOrb:
		case ETFClassID::CTFProjectile_EnergyBall:
		case ETFClassID::CTFProjectile_Flare:
			pWeapon = pEntity->As<CTFBaseRocket>()->m_hLauncher().Get()->As<CTFWeaponBase>();
			pOwner = pWeapon ? pWeapon->m_hOwner().Get()->As<CTFPlayer>() : nullptr;
			break;
		case ETFClassID::CTFProjectile_EnergyRing:
			pWeapon = pEntity->As<CTFBaseProjectile>()->m_hLauncher().Get()->As<CTFWeaponBase>();
			pOwner = pWeapon ? pWeapon->m_hOwner().Get()->As<CTFPlayer>() : nullptr;
		}
		if (!pOwner || pOwner->m_iTeamNum() == pTarget->m_iTeamNum() || pWeapon && !pWeapon->GetDamage())
			continue;

		Vec3 vVelocity = pEntity->GetAbsVelocity();
		float flRadius = 0.f;
		switch (pEntity->GetClassID())
		{
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_SentryRocket:
		case ETFClassID::CTFProjectile_EnergyBall:
			vVelocity = pEntity->As<CTFProjectile_Rocket>()->m_vInitialVelocity();
			[[fallthrough]];
		case ETFClassID::CTFGrenadePipebombProjectile:
		case ETFClassID::CTFWeaponBaseMerasmusGrenade:
			flRadius = 146.f;
			break;
		case ETFClassID::CTFProjectile_Flare:
			flRadius = 110.f;
		}
		if (pOwner && pWeapon && flRadius)
		{
			flRadius = SDK::AttribHookValue(flRadius, "mult_explosion_radius", pWeapon);
			switch (pWeapon->GetWeaponID())
			{
			case TF_WEAPON_ROCKETLAUNCHER:
			case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
			case TF_WEAPON_PARTICLE_CANNON:
				if (pOwner->InCond(TF_COND_BLASTJUMPING) && SDK::AttribHookValue(1.f, "rocketjump_attackrate_bonus", pWeapon) != 1.f)
					flRadius *= 0.8f;
			}
		}

		Vec3 vProjectileOrigin = PredictOrigin(pEntity->m_vecOrigin(), vVelocity, flLatency, true, pEntity->m_vecMins(), pEntity->m_vecMaxs());
		if (!TraceToEntity(pTarget, pEntity, vTargetEye, vProjectileOrigin, MASK_SHOT))
			continue;

		int iType = MEDIGUN_BLAST_RESIST;
		float flDamage = 100.f, flMult = pTarget->IsMarked() ? 1.36f : 1.f;
		switch (pEntity->GetClassID())
		{
		case ETFClassID::CTFGrenadePipebombProjectile:
		case ETFClassID::CTFWeaponBaseMerasmusGrenade:
		case ETFClassID::CTFProjectile_SpellMeteorShower:
			if (pEntity->As<CTFWeaponBaseGrenadeProj>()->m_bCritical())
				flMult = 3.f;
			else if (pEntity->As<CTFWeaponBaseGrenadeProj>()->m_iDeflected() && (pEntity->GetClassID() != ETFClassID::CTFGrenadePipebombProjectile || !pEntity->GetAbsVelocity().IsZero()))
				flMult = 1.36f;
			break;
		case ETFClassID::CTFProjectile_Arrow:
			if (pEntity->As<CTFProjectile_Arrow>()->m_iProjectileType() == TF_PROJECTILE_ARROW)
			{
				flMult = 3.f;
				break;
			}
			[[fallthrough]];
		case ETFClassID::CTFProjectile_HealingBolt:
			if (pEntity->As<CTFProjectile_Arrow>()->m_bCritical())
				flMult = 3.f;
			else if (pEntity->As<CTFBaseRocket>()->m_iDeflected())
				flMult = 1.36f;
			break;
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_BallOfFire:
		case ETFClassID::CTFProjectile_SentryRocket:
		case ETFClassID::CTFProjectile_SpellFireball:
		case ETFClassID::CTFProjectile_SpellLightningOrb:
			if (pEntity->As<CTFProjectile_Rocket>()->m_bCritical())
				flMult = 3.f;
			else if (pEntity->As<CTFBaseRocket>()->m_iDeflected())
				flMult = 1.36f;
			break;
		case ETFClassID::CTFProjectile_EnergyBall:
			if (pEntity->As<CTFProjectile_EnergyBall>()->m_bChargedShot())
				flMult = 2.f; // whatever
			else if (pEntity->As<CTFBaseRocket>()->m_iDeflected())
				flMult = 1.36f;
			break;
		case ETFClassID::CTFProjectile_Flare:
			if (pEntity->As<CTFProjectile_Flare>()->m_bCritical() || pTarget->InCond(TF_COND_BURNING) || pTarget->InCond(TF_COND_BURNING_PYRO))
				flMult = 3.f;
			else if (pEntity->As<CTFBaseRocket>()->m_iDeflected())
				flMult = 1.36f;
		}
		switch (pEntity->GetClassID())
		{
		case ETFClassID::CTFGrenadePipebombProjectile:
			flDamage = 100.f;
			break;
		case ETFClassID::CTFWeaponBaseMerasmusGrenade:
			flDamage = 50.f;
			break;
		case ETFClassID::CTFProjectile_SpellMeteorShower:
			flDamage = 100.f;
			iType = MEDIGUN_FIRE_RESIST;
			break;
		case ETFClassID::CTFProjectile_Arrow:
			flDamage = pEntity->As<CTFProjectile_Arrow>()->m_iProjectileType() == TF_PROJECTILE_ARROW
				? Math::RemapVal(pEntity->As<CTFProjectile_Arrow>()->m_vInitialVelocity().Length(), 1800.f, 2600.f, 50.f, 120.f)
				: 40.f;
			iType = MEDIGUN_BULLET_RESIST;
			break;
		case ETFClassID::CTFProjectile_HealingBolt:
			flDamage = Math::RemapVal(pOwner->m_vecOrigin().DistTo(vProjectileOrigin), 200.f, 1600.f, 38.f, 75.f);
			iType = MEDIGUN_BULLET_RESIST;
			break;
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_EnergyBall:
			flDamage = flMult ? 90.f : Math::RemapVal(pOwner->m_vecOrigin().DistTo(vProjectileOrigin), 500.f, 920.f, 90.f, 48.f);
			break;
		case ETFClassID::CTFProjectile_SentryRocket:
			flDamage = flMult ? 100.f : Math::RemapVal(pOwner->m_vecOrigin().DistTo(vProjectileOrigin), 100.f, 920.f, 100.f, 50.f);
			break;
		case ETFClassID::CTFProjectile_BallOfFire:
			flDamage = pTarget->InCond(TF_COND_BURNING) || pTarget->InCond(TF_COND_BURNING_PYRO) ? 90.f : 30.f;
			iType = MEDIGUN_FIRE_RESIST;
			break;
		case ETFClassID::CTFProjectile_SpellFireball:
			flDamage = 100.f;
			iType = MEDIGUN_FIRE_RESIST;
			break;
		case ETFClassID::CTFProjectile_SpellLightningOrb:
			flDamage = 100.f;
			iType = MEDIGUN_FIRE_RESIST;
			break;
		case ETFClassID::CTFProjectile_Flare:
			flDamage = 30.f;
			iType = MEDIGUN_FIRE_RESIST;
			break;
		case ETFClassID::CTFProjectile_EnergyRing: 
			flDamage = 60.f;
			iType = MEDIGUN_BULLET_RESIST;
		}
		switch (iType)
		{
		case MEDIGUN_BULLET_RESIST: flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist; break;
		case MEDIGUN_BLAST_RESIST: flMult = flBlastResist > 0.f ? 1.f - flBlastResist : flMult + flBlastResist; break;
		case MEDIGUN_FIRE_RESIST: flMult = flFireResist > 0.f ? 1.f - flFireResist : flMult + flFireResist; break;
		}
		flDamage *= flMult * (pWeapon ? SDK::AttribHookValue(1.f, "mult_dmg", pWeapon) : 1);

		flRadius += vVelocity.Length() * flLatency + pTarget->GetSize().Length() / 2;
		float flDamageDanger = flDamage / iPlayerHealth;
		float flDistanceDanger = Math::RemapVal(pTarget->m_vecOrigin().DistTo(vProjectileOrigin), flRadius, flRadius, 1.f, 0.001f);

#ifdef DEBUG_VACCINATOR
		G::SphereStorage.emplace_back(vProjectileOrigin, flRadius, 36, 36, I::GlobalVars->curtime + 60.f, Color_t(255, 255, 255, 1), Color_t(0, 0, 0, 0), true);
		SDK::Output(std::format("Projectile {}", iType).c_str(), std::format("{}, {}, {}", flDamage, flDamageDanger, flDistanceDanger).c_str(), { 255, 200, 200 }, Vars::Debug::Logging.Value);
#endif
		switch (iType)
		{
		case MEDIGUN_BULLET_RESIST:
			flBulletDanger += flDamageDanger * flDistanceDanger;
			break;
		case MEDIGUN_BLAST_RESIST:
			flBlastDanger += flDamageDanger * flDistanceDanger;
			break;
		case MEDIGUN_FIRE_RESIST:
			flFireDanger += flDamageDanger * flDistanceDanger;
		}
	}

	if (m_flDamagedTime <= TICK_INTERVAL)
	{
		if (pTarget->InCond(TF_COND_BURNING) || pTarget->InCond(TF_COND_BURNING_PYRO))
		{
			m_flDamagedTime = TICK_INTERVAL * 2;
			m_iDamagedType = MEDIGUN_FIRE_RESIST;
			m_flDamagedDPS = 8.f;
		}
	}
	if (m_flDamagedTime = std::max(m_flDamagedTime - TICK_INTERVAL, 0.f))
	{
		switch (m_iDamagedType)
		{
		case MEDIGUN_BULLET_RESIST:
			flBulletDanger += m_flDamagedDPS / iPlayerHealth;
			break;
		case MEDIGUN_BLAST_RESIST:
			flBlastDanger += m_flDamagedDPS / iPlayerHealth;
			break;
		case MEDIGUN_FIRE_RESIST:
			flFireDanger += m_flDamagedDPS / iPlayerHealth;
		}
	}

	// ignore resistances we are already charged with, else scale
	if (flBulletResist >= (bVaccinator ? 0.75f : 1.f))
		flBulletDanger = 0.f;
	else
		flBulletDanger *= Vars::Aimbot::Healing::AutoVaccinatorBulletScale.Value / 100.f;
	if (flBlastResist >= (bVaccinator ? 0.75f : 1.f))
		flBlastDanger = 0.f;
	else
		flBlastDanger *= Vars::Aimbot::Healing::AutoVaccinatorBlastScale.Value / 100.f;
	if (flFireResist >= (bVaccinator ? 0.75f : 1.f))
		flFireDanger = 0.f;
	else
		flFireDanger *= Vars::Aimbot::Healing::AutoVaccinatorFireScale.Value / 100.f;

	flBulletOut = std::max(flBulletOut, flBulletDanger);
	flBlastOut = std::max(flBlastOut, flBlastDanger);
	flFireOut = std::max(flFireOut, flFireDanger);
}

void CAutoHeal::SwapResistType(CUserCmd* pCmd, int iType)
{
	if (m_iResistType != iType && !(G::LastUserCmd->buttons & IN_RELOAD))
		pCmd->buttons |= IN_RELOAD;
}

void CAutoHeal::ActivateResistType(CUserCmd* pCmd, int iType)
{
	if (m_flChargeLevel >= 0.25f && m_iResistType == iType)
		pCmd->buttons |= IN_ATTACK2;
}

void CAutoHeal::AutoVaccinator(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd)
{
	if (!Vars::Aimbot::Healing::AutoVaccinator.Value || pWeapon->GetMedigunType() != MEDIGUN_RESIST)
		return;

#ifdef DEBUG_VACCINATOR
	G::LineStorage.clear();
	G::SweptStorage.clear();
	G::SphereStorage.clear();
#endif
	if (m_iResistType == -1)
		m_iResistType = pWeapon->GetResistType();
#ifdef DEBUG_VACCINATOR
	vResistDangers = {
#else
	std::vector<std::pair<float, int>> vResistDangers = {
#endif
		{ 0.f, MEDIGUN_BULLET_RESIST },
		{ 0.f, MEDIGUN_BLAST_RESIST },
		{ 0.f, MEDIGUN_FIRE_RESIST }
	};

	GetDangers(pLocal, true, vResistDangers[MEDIGUN_BULLET_RESIST].first, vResistDangers[MEDIGUN_BLAST_RESIST].first, vResistDangers[MEDIGUN_FIRE_RESIST].first);
	auto pTarget = pWeapon->m_hHealingTarget().Get()->As<CTFPlayer>();
	if (pTarget && (!Vars::Aimbot::Healing::FriendsOnly.Value || H::Entities.IsFriend(pTarget->entindex()) || H::Entities.InParty(pTarget->entindex())))
		GetDangers(pTarget, true, vResistDangers[MEDIGUN_BULLET_RESIST].first, vResistDangers[MEDIGUN_BLAST_RESIST].first, vResistDangers[MEDIGUN_FIRE_RESIST].first);
	std::sort(vResistDangers.begin(), vResistDangers.end(), [&](const std::pair<float, int>& a, const std::pair<float, int>& b) -> bool
		{
			return a.first > b.first;
		});

	int iTargetResist = vResistDangers.front().second;
	float flTargetDanger = vResistDangers.front().first;
	if (flTargetDanger)
		SwapResistType(pCmd, iTargetResist);
	if (flTargetDanger >= 1.f)
		ActivateResistType(pCmd, iTargetResist);

	if (m_bPreventResistSwap)
		pCmd->buttons &= ~IN_RELOAD;
	if (m_bPreventResistCharge)
		pCmd->buttons &= ~IN_ATTACK2;
	if (pCmd->buttons & IN_RELOAD && !(G::LastUserCmd->buttons & IN_RELOAD))
	{
		m_iResistType = (m_iResistType + 1) % 3;
		m_flSwapTime = I::GlobalVars->curtime + F::Backtrack.GetReal(MAX_FLOWS, false) * 1.5f + 0.1f;
	}
	if (m_flSwapTime < I::GlobalVars->curtime)
		m_iResistType = pWeapon->GetResistType();
	m_flChargeLevel = pWeapon->m_flChargeLevel();
}

EVaccinatorResist CAutoHeal::GetCurrentResistType(CWeaponMedigun* pWeapon)
{
	int nResistType = pWeapon->m_nChargeResistType();
	
	switch (nResistType)
	{
	case 0: return VACC_BULLET;
	case 1: return VACC_BLAST;
	case 2: return VACC_FIRE;
	default: return VACC_BULLET; // Default to bullet
	}
}

void CAutoHeal::SwitchResistance(CWeaponMedigun* pWeapon, CUserCmd* pCmd, EVaccinatorResist eResist)
{
	EVaccinatorResist eCurrentResist = GetCurrentResistType(pWeapon);
	
	if (eCurrentResist == eResist)
		return;
	
	int nClicks = 0;
	switch (eCurrentResist)
	{
	case VACC_BULLET:
		nClicks = (eResist == VACC_BLAST) ? 1 : 2; // BULLET -> BLAST = 1, BULLET -> FIRE = 2
		break;
	case VACC_BLAST:
		nClicks = (eResist == VACC_FIRE) ? 1 : 2; // BLAST -> FIRE = 1, BLAST -> BULLET = 2
		break;
	case VACC_FIRE:
		nClicks = (eResist == VACC_BULLET) ? 1 : 2; // FIRE -> BULLET = 1, FIRE -> BLAST = 2
		break;
	}
	
	pCmd->buttons |= IN_RELOAD;
	
	m_flLastSwitchTime = I::GlobalVars->curtime;
	m_eCurrentAutoResist = eResist;
}

void CAutoHeal::CycleToNextResistance(CWeaponMedigun* pWeapon, CUserCmd* pCmd)
{
	if (m_vActiveResistances.empty())
	{
		m_vActiveResistances.push_back(VACC_BULLET);
		m_iCurrentResistIndex = 0;
		return;
	}
	
	m_iCurrentResistIndex = (m_iCurrentResistIndex + 1) % m_vActiveResistances.size();
	EVaccinatorResist eNextResist = m_vActiveResistances[m_iCurrentResistIndex];
	SwitchResistance(pWeapon, pCmd, eNextResist);
	m_flLastCycleTime = I::GlobalVars->curtime;
}

EVaccinatorResist CAutoHeal::GetResistTypeForClass(int nClass)
{
	switch (nClass)
	{
	case TF_CLASS_SCOUT:
	case TF_CLASS_HEAVY:
	case TF_CLASS_SNIPER:
		return VACC_BULLET;
	
	case TF_CLASS_SOLDIER:
	case TF_CLASS_DEMOMAN:
		return VACC_BLAST;
	
	case TF_CLASS_PYRO:
		return VACC_FIRE;
	
	case TF_CLASS_ENGINEER: // Engineer has sentry (bullet) but also shotgun
	case TF_CLASS_MEDIC:    // Medic's syringe gun is projectile but acts like hitscan
	case TF_CLASS_SPY:      // Spy's revolver is hitscan
	default:
		return VACC_BULLET;
	}
}

std::vector<EVaccinatorResist> CAutoHeal::GetResistTypesFromNearbyEnemies(CTFPlayer* pLocal, float flRange)
{
	std::vector<EVaccinatorResist> resistTypes;
	std::unordered_map<EVaccinatorResist, int> resistCounts;
	for (int i = 1; i <= I::GlobalVars->maxClients; i++)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pEntity || !pEntity->IsPlayer())
			continue;
		
		CTFPlayer* pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
			continue;
		
		Vector vLocalPos = pLocal->GetAbsOrigin();
		Vector vEnemyPos = pPlayer->GetAbsOrigin();
		float flDistance = vLocalPos.DistTo(vEnemyPos);
		
		if (flDistance <= flRange)
		{
			EVaccinatorResist resistType = GetResistTypeForClass(pPlayer->m_iClass());
			resistCounts[resistType]++;
		}
	}
	
	std::vector<std::pair<EVaccinatorResist, int>> sortedResists;
	for (const auto& pair : resistCounts)
	{
		sortedResists.push_back(pair);
	}
	
	std::sort(sortedResists.begin(), sortedResists.end(), 
		[](const std::pair<EVaccinatorResist, int>& a, const std::pair<EVaccinatorResist, int>& b) {
			return a.second > b.second;
		});
	
	for (const auto& pair : sortedResists)
	{
		if (pair.second > 0)
		{
			resistTypes.push_back(pair.first);
		}
	}
	
	if (resistTypes.empty())
	{
		resistTypes.push_back(VACC_BULLET);
	}
	
	return resistTypes;
}

EVaccinatorResist CAutoHeal::GetBestResistType(CTFPlayer* pLocal)
{
	if (Vars::Aimbot::Healing::VaccinatorClassBased.Value)
	{
		auto resistTypes = GetResistTypesFromNearbyEnemies(pLocal, Vars::Aimbot::Healing::VaccinatorRange.Value);
		m_vActiveResistances = resistTypes;
		if (!resistTypes.empty())
		{
			return resistTypes[0];
		}
	}

	// Fall back to damage-based detection if class-based detection is disabled or no enemies found
	// Remove old damage records (older than 5 seconds)
	const float MAX_RECORD_AGE = 5.0f;
	while (!m_DamageRecords.empty() && (I::GlobalVars->curtime - m_DamageRecords.front().Time) > MAX_RECORD_AGE)
	{
		m_DamageRecords.pop_front();
	}
	
	// Count damage by type
	float bulletDamage = 0.0f;
	float blastDamage = 0.0f;
	float fireDamage = 0.0f;
	
	for (const auto& record : m_DamageRecords)
	{
		switch (record.Type)
		{
		case VACC_BULLET: bulletDamage += record.Amount; break;
		case VACC_BLAST: blastDamage += record.Amount; break;
		case VACC_FIRE: fireDamage += record.Amount; break;
		}
	}
	m_vActiveResistances.clear();
	
	// Sort damage types by amount
	std::vector<std::pair<EVaccinatorResist, float>> sortedDamage = {
		{VACC_BULLET, bulletDamage},
		{VACC_BLAST, blastDamage},
		{VACC_FIRE, fireDamage}
	};
	
	std::sort(sortedDamage.begin(), sortedDamage.end(),
		[](const std::pair<EVaccinatorResist, float>& a, const std::pair<EVaccinatorResist, float>& b) {
			return a.second > b.second;
		});
	
	for (const auto& pair : sortedDamage)
	{
		if (pair.second > 0.0f)
		{
			m_vActiveResistances.push_back(pair.first);
		}
	}
	
	// If no damage records, default to bullet
	if (m_vActiveResistances.empty())
	{
		m_vActiveResistances.push_back(VACC_BULLET);
	}
	
	return m_vActiveResistances[0];
}

bool CAutoHeal::RunVaccinator(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd)
{
	if (!Vars::Aimbot::Healing::AutoVaccinator.Value)
		return false;
	if (pWeapon->m_iItemDefinitionIndex() != 998) // Vaccinator item def index
		return false;
	auto pTarget = pWeapon->m_hHealingTarget().Get();
	if (!IsValidHealTarget(pLocal, pTarget))
		return false;
	
	int nMode = Vars::Aimbot::Healing::VaccinatorMode.Value;
	if (nMode == Vars::Aimbot::Healing::VaccinatorModeEnum::Manual)
	{
		EVaccinatorResist eResist;
		switch (Vars::Aimbot::Healing::VaccinatorResist.Value)
		{
		case Vars::Aimbot::Healing::VaccinatorResistEnum::Bullet: eResist = VACC_BULLET; break;
		case Vars::Aimbot::Healing::VaccinatorResistEnum::Blast: eResist = VACC_BLAST; break;
		case Vars::Aimbot::Healing::VaccinatorResistEnum::Fire: eResist = VACC_FIRE; break;
		default: eResist = VACC_BULLET; break;
		}
		
		SwitchResistance(pWeapon, pCmd, eResist);
		return true;
	}
	if (nMode == Vars::Aimbot::Healing::VaccinatorModeEnum::Auto)
	{
		GetBestResistType(pLocal);
		if (Vars::Aimbot::Healing::VaccinatorMultiResist.Value && m_vActiveResistances.size() > 1)
		{
			if ((I::GlobalVars->curtime - m_flLastCycleTime) >= Vars::Aimbot::Healing::VaccinatorDelay.Value)
			{
				CycleToNextResistance(pWeapon, pCmd);
			}
			return true;
		}
		
		else
		{
			if ((I::GlobalVars->curtime - m_flLastSwitchTime) < Vars::Aimbot::Healing::VaccinatorDelay.Value)
				return true;
			EVaccinatorResist eBestResist = !m_vActiveResistances.empty() ? m_vActiveResistances[0] : VACC_BULLET;
			if (eBestResist != GetCurrentResistType(pWeapon))
			{
				SwitchResistance(pWeapon, pCmd, eBestResist);
			}
		}
		
		return true;
	}
	
	return false;
}
void CAutoHeal::Event(IGameEvent* pEvent, uint32_t uHash, CTFPlayer* pLocal)
{
    if (!pEvent || !pLocal || !I::EngineClient || !I::GlobalVars)
        return;

    if (!pLocal->IsAlive() || pLocal->m_iClass() != TF_CLASS_MEDIC)
        return;

    if (strcmp(pEvent->GetName(), "player_hurt") != 0)
        return;

    if (!Vars::Aimbot::Healing::AutoVaccinator.Value || !Vars::Aimbot::Healing::VaccinatorSmart.Value)
        return;

    int userid_val = pEvent->GetInt("userid", 0);
    if (userid_val <= 0)
        return;

    int iVictim = I::EngineClient->GetPlayerForUserID(userid_val);
    if (iVictim <= 0 || iVictim != pLocal->entindex())
        return;

    int nDamage = pEvent->GetInt("damageamount", 0);
    if (nDamage <= 0)
        return;

    EVaccinatorResist eResistType = VACC_BULLET;
    int iDamageType = pEvent->GetInt("damagetype", 0);
    
    if (iDamageType & DMG_BURN || iDamageType & DMG_IGNITE || iDamageType & DMG_PLASMA)
        eResistType = VACC_FIRE;
    else if (iDamageType & DMG_BLAST || iDamageType & DMG_BLAST_SURFACE || iDamageType & DMG_SONIC)
        eResistType = VACC_BLAST;

    int nWeaponID = pEvent->GetInt("weaponid", 0);
    
    if (nWeaponID == TF_WEAPON_ROCKETLAUNCHER || 
        nWeaponID == TF_WEAPON_GRENADELAUNCHER || 
        nWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER)
        eResistType = VACC_BLAST;
    else if (nWeaponID == TF_WEAPON_FLAMETHROWER || 
             nWeaponID == TF_WEAPON_FLAREGUN)
        eResistType = VACC_FIRE;

    if (I::GlobalVars->curtime > 0.0f)
    {
        DamageRecord_t record;
        record.Type = eResistType;
        record.Amount = static_cast<float>(nDamage);
        record.Time = I::GlobalVars->curtime;
        m_DamageRecords.push_back(record);
    }
}

bool CAutoHeal::ShouldPopOnHealth(CTFPlayer* pLocal, CWeaponMedigun* pWeapon)
{
	if (!pWeapon)
		return false;
	
	// Get health threshold percentage
	float flThreshold = Vars::Aimbot::Healing::HealthThreshold.Value / 100.0f;
	
	// Check medic's own health if enabled
	if (Vars::Aimbot::Healing::SelfLowHealth.Value)
	{
		int iMaxHealth = pLocal->GetMaxHealth();
		int iCurrentHealth = pLocal->m_iHealth();
		
		if (iCurrentHealth <= iMaxHealth * flThreshold)
			return true;
	}
	
	// Check patient's health if enabled
	if (Vars::Aimbot::Healing::PatientLowHealth.Value)
	{
		auto pTarget = pWeapon->m_hHealingTarget().Get();
		if (pTarget && pTarget->IsPlayer())
		{
			CTFPlayer* pPlayerTarget = pTarget->As<CTFPlayer>();
			if (pPlayerTarget && pPlayerTarget->IsAlive())
			{
				int iMaxHealth = pPlayerTarget->GetMaxHealth();
				int iCurrentHealth = pPlayerTarget->m_iHealth();
				
				if (iCurrentHealth <= iMaxHealth * flThreshold)
					return true;
			}
		}
	}
	
	return false;
}

bool CAutoHeal::ShouldPopOnEnemyCount(CTFPlayer* pLocal, CWeaponMedigun* pWeapon)
{
	if (!Vars::Aimbot::Healing::PopOnMultipleEnemies.Value)
		return false;
	
	int iEnemyThreshold = Vars::Aimbot::Healing::EnemyCountThreshold.Value;
	
	// Get our position (use patient position if we're healing someone)
	Vector vPosition = pLocal->GetAbsOrigin();
	auto pTarget = pWeapon->m_hHealingTarget().Get();
	if (pTarget && pTarget->IsPlayer())
	{
		CTFPlayer* pPlayerTarget = pTarget->As<CTFPlayer>();
		if (pPlayerTarget && pPlayerTarget->IsAlive())
		{
			vPosition = pPlayerTarget->GetAbsOrigin();
		}
	}
	
	// Count nearby enemies
	int iEnemyCount = 0;
	float flMaxDistance = 800.0f; // Consider enemies within this range
	
	for (int i = 1; i <= I::GlobalVars->maxClients; i++)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pEntity || !pEntity->IsPlayer())
			continue;
		
		CTFPlayer* pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
			continue;
		
		// Check distance
		Vector vEnemyPos = pPlayer->GetAbsOrigin();
		float flDistance = vPosition.DistTo(vEnemyPos);
		
		if (flDistance <= flMaxDistance)
			iEnemyCount++;
	}
	
	return (iEnemyCount >= iEnemyThreshold);
}

bool CAutoHeal::ShouldPopOnDangerProjectiles(CTFPlayer* pLocal, CWeaponMedigun* pWeapon)
{
	if (!Vars::Aimbot::Healing::PopOnDangerProjectiles.Value)
		return false;
	
	Vector vPosition = pLocal->GetAbsOrigin();
	auto pTarget = pWeapon->m_hHealingTarget().Get();
	if (pTarget && pTarget->IsPlayer())
	{
		CTFPlayer* pPlayerTarget = pTarget->As<CTFPlayer>();
		if (pPlayerTarget && pPlayerTarget->IsAlive())
		{
			vPosition = pPlayerTarget->GetAbsOrigin();
		}
	}
	
	// Loop through all entities to find dangerous projectiles
	for (int i = I::GlobalVars->maxClients + 1; i <= I::ClientEntityList->GetHighestEntityIndex(); i++)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pEntity)
			continue;
		
		const char* pszClassname = pEntity->GetClientClass()->m_pNetworkName;
		
		// Skip if not a projectile (using string comparison instead of FStrEq)
		if (strcmp(pszClassname, "CTFProjectile_Rocket") != 0 && 
		    strcmp(pszClassname, "CTFProjectile_SentryRocket") != 0 && 
		    strcmp(pszClassname, "CTFGrenadePipebombProjectile") != 0 && 
		    strcmp(pszClassname, "CTFProjectile_Flare") != 0 && 
		    strcmp(pszClassname, "CTFProjectile_Arrow") != 0 && 
		    strcmp(pszClassname, "CTFProjectile_Jar") != 0)
			continue;
		
		// Check if the projectile is from an enemy
		CTFBaseProjectile* pProjectile = pEntity->As<CTFBaseProjectile>();
		if (pProjectile && pProjectile->m_iTeamNum() == pLocal->m_iTeamNum())
			continue;
		
		// Check if it's close enough
		Vector vProjectilePos = pEntity->GetAbsOrigin();
		float flDistance = vPosition.DistTo(vProjectilePos);
		
		// Pop uber if dangerous projectile is close
		if (flDistance < 300.0f)
			return true;
	}
	
	return false;
}

bool CAutoHeal::ShouldPop(CTFPlayer* pLocal, CWeaponMedigun* pWeapon)
{
	// Don't pop if we're preserving uber and not in immediate danger
	if (Vars::Aimbot::Healing::PreserveUber.Value)
	{
		// Check if we're in a safe area with no enemies nearby
		bool bEnemiesNearby = ShouldPopOnEnemyCount(pLocal, pWeapon);
		
		// If there are no enemies nearby and our health is good, preserve uber
		if (!bEnemiesNearby && !ShouldPopOnHealth(pLocal, pWeapon))
			return false;
	}
	
	// Now check all the conditions for popping uber
	if (ShouldPopOnHealth(pLocal, pWeapon))
		return true;
	
	if (ShouldPopOnEnemyCount(pLocal, pWeapon))
		return true;
	
	if (ShouldPopOnDangerProjectiles(pLocal, pWeapon))
		return true;
	
	return false;
}

bool CAutoHeal::RunAutoUber(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CUserCmd* pCmd)
{
	if (!Vars::Aimbot::Healing::AutoUber.Value)
		return false;
	
	if (!pWeapon)
		return false;
	
	// Check if we have a heal target and it's valid with our settings
	auto pTarget = pWeapon->m_hHealingTarget().Get();
	if (!IsValidHealTarget(pLocal, pTarget))
		return false;
	
	float flChargeLevel = pWeapon->m_flChargeLevel();
	
	// Different thresholds for different mediguns
	float flMinLevel = 1.0f; // Default for stock, kritz, and quickfix
	int iItemDef = pWeapon->m_iItemDefinitionIndex();
	
	// Vaccinator needs 25% charge for a bubble
	if (iItemDef == 998) // Vaccinator
		flMinLevel = 0.25f;
	
	// Check if we have enough charge
	if (flChargeLevel < flMinLevel)
		return false;
	
	// If we're already ubering, don't need to do anything
	if (pWeapon->m_bChargeRelease())
		return true;
	
	// Check if we should pop uber
	if (ShouldPop(pLocal, pWeapon))
	{
		// Pop uber! Press M2
		pCmd->buttons |= IN_ATTACK2;
		return true;
	}
	
	return false;
}

void CAutoHeal::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (!pLocal || !pWeapon || !pCmd)
		return;
	
	CWeaponMedigun* pMedigun = pWeapon->As<CWeaponMedigun>();
	
	if (!pMedigun)
	{
		m_mMedicCallers.clear();
		m_iResistType = -1;
		m_flDamagedTime = 0.f;
		return;
	}

	if (RunAutoUber(pLocal, pMedigun, pCmd))
		return;

	if (RunVaccinator(pLocal, pMedigun, pCmd))
		return;

	bool bActivated = ActivateOnVoice(pLocal, pMedigun, pCmd);
	m_mMedicCallers.clear();
	if (bActivated)
		return;
	}

	ActivateOnVoice(pLocal, pWeapon->As<CWeaponMedigun>(), pCmd);
	m_mMedicCallers.clear();
	
	AutoVaccinator(pLocal, pWeapon->As<CWeaponMedigun>(), pCmd);
}

void CAutoHeal::Event(IGameEvent* pEvent, uint32_t uHash)
{
	switch (uHash)
	{
	case FNV1A::Hash32Const("player_hurt"):
	{
		if (!Vars::Aimbot::Healing::AutoVaccinator.Value)
			return;

		auto pWeapon = H::Entities.GetWeapon()->As<CWeaponMedigun>();
		if (!pWeapon
			|| pWeapon->GetWeaponID() != TF_WEAPON_MEDIGUN || pWeapon->GetMedigunType() != MEDIGUN_RESIST)
			return;

		int iVictim = I::EngineClient->GetPlayerForUserID(pEvent->GetInt("userid"));
		int iAttacker = I::EngineClient->GetPlayerForUserID(pEvent->GetInt("attacker"));
		int iDamage = pEvent->GetInt("damageamount");
		int iWeaponID = pEvent->GetInt("weaponid");
		bool bCrit = pEvent->GetBool("crit");
		bool bMinicrit = pEvent->GetBool("minicrit");

		int iTarget = pWeapon->m_hHealingTarget().GetIndex();
		if (iVictim == iAttacker || iVictim != I::EngineClient->GetLocalPlayer() && (iVictim != iTarget || Vars::Aimbot::Healing::FriendsOnly.Value && !H::Entities.IsFriend(iTarget) && !H::Entities.InParty(iTarget)))
			return;

		auto pEntity = I::ClientEntityList->GetClientEntity(iAttacker)->As<CTFPlayer>();
		if (!pEntity || !pEntity->IsPlayer())
			return;

		auto pWeapon2 = pEntity->m_hActiveWeapon().Get()->As<CTFWeaponBase>();
		if (!pWeapon2 || pWeapon2->GetWeaponID() != iWeaponID || pWeapon2->GetFireRate() > 1.f)
			return;

		float flFireRate = pWeapon2->GetFireRate();
		float flMult = SDK::AttribHookValue(1, "mult_dmg", pWeapon2);
		switch (SDK::GetWeaponType(pWeapon2))
		{
		case EWeaponType::HITSCAN:
			m_iDamagedType = MEDIGUN_BULLET_RESIST;
			m_flDamagedDPS = iDamage / flFireRate;
			break;
		case EWeaponType::PROJECTILE:
			switch (iWeaponID)
			{
			case TF_WEAPON_FLAMETHROWER:
				if (!bCrit && !bMinicrit || iDamage / flMult < (bCrit ? 48 : 22))
				{
					m_iDamagedType = MEDIGUN_FIRE_RESIST;
					float flBurnMult = SDK::AttribHookValue(1, "mult_wpn_burndmg", pWeapon2);
					if (!bCrit && !bMinicrit && iDamage <= 4 * flBurnMult)
						m_flDamagedDPS = 8.f * flBurnMult;
					else
						m_flDamagedDPS = 80.f * flMult * (bCrit ? 3.f : bMinicrit ? 1.36f : 1.f);
				}
				else // better way to assume reflect?
				{
					m_iDamagedType = MEDIGUN_BLAST_RESIST;
					m_flDamagedDPS = iDamage;
				}
				break;
			case TF_WEAPON_COMPOUND_BOW:
			case TF_WEAPON_CROSSBOW:
			case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
			case TF_WEAPON_SYRINGEGUN_MEDIC:
			case TF_WEAPON_RAYGUN:
				m_iDamagedType = MEDIGUN_BULLET_RESIST;
				m_flDamagedDPS = iDamage / flFireRate;
				break;
			case TF_WEAPON_FLAME_BALL:
				m_iDamagedType = MEDIGUN_FIRE_RESIST;
				m_flDamagedDPS = iDamage / flFireRate;
				break;
			default:
				m_iDamagedType = MEDIGUN_BLAST_RESIST;
				m_flDamagedDPS = iDamage / flFireRate;
			}
			break;
		case EWeaponType::MELEE:
			return;
		}
		m_flDamagedTime = 1.f;

#ifdef DEBUG_VACCINATOR
		SDK::Output("Hurt", std::format("{}, {}", m_iDamagedType, m_flDamagedDPS).c_str(), { 255, 100, 100 });
#endif
		break;
	}
	case FNV1A::Hash32Const("player_spawn"):
		m_flDamagedTime = 0.f;
	}
}

#ifdef DEBUG_VACCINATOR
void CAutoHeal::Draw(CTFPlayer* pLocal)
{
	auto pWeapon = H::Entities.GetWeapon()->As<CWeaponMedigun>();
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_MEDIGUN
		|| !Vars::Aimbot::Healing::AutoVaccinator.Value || pWeapon->GetMedigunType() != MEDIGUN_RESIST)
		return;

	int x = H::Draw.m_nScreenW / 2, y = 100;
	const auto& fFont = H::Fonts.GetFont(FONT_INDICATORS);
	const int nTall = fFont.m_nTall + H::Draw.Scale(1);
	y -= nTall;

	for (auto& [flDanger, iResist] : vResistDangers)
		H::Draw.String(fFont, x, y += nTall, Vars::Menu::Theme::Active.Value, ALIGN_TOP, std::format("{}: {:.3f}", iResist == MEDIGUN_BULLET_RESIST ? "Bullet" : iResist == MEDIGUN_BLAST_RESIST ? "Blast" : "Fire", flDanger).c_str());
}
#endif