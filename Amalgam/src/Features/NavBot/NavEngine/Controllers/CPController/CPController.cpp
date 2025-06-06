#include "CPController.h"

//TODO: fix

void CCPController::UpdateObjectiveResource()
{
	// Get ObjectiveResource
	m_pObjectiveResource = H::Entities.GetOR();
}

// Don't constantly update the cap status
static Timer tCapStatusUpdate{};
void CCPController::UpdateControlPoints()
{
	// No objective resource, can't run
	if (!m_pObjectiveResource)
		return;

	const int iNumControlPoints = m_pObjectiveResource->m_iNumControlPoints();
	// No control points
	if (!iNumControlPoints)
		return;

	// Clear the invalid controlpoints
	if (iNumControlPoints <= MAX_CONTROL_POINTS)
	{
		for (int i = iNumControlPoints; i < MAX_CONTROL_POINTS; ++i)
			m_aControlPointData[i] = CPInfo();
	}

	for (int i = 0; i < iNumControlPoints; ++i)
	{
		auto& tData = m_aControlPointData[i];
		tData.m_iIdx = i;

		// Update position
		tData.m_vPos = m_pObjectiveResource->m_vCPPositions(i);
	}

	if (tCapStatusUpdate.Run(1.f))
	{
		for (int i = 0; i < iNumControlPoints; ++i)
		{
			auto& tData = m_aControlPointData[i];

			// Check accessibility for both teams, requires alot of checks
			const bool bCanCapRED = IsPointUseable(i, TF_TEAM_RED);
			const bool bCanCapBLU = IsPointUseable(i, TF_TEAM_BLUE);

			tData.m_bCanCap.at(0) = bCanCapRED;
			tData.m_bCanCap.at(1) = bCanCapBLU;
		}
	}
}

bool CCPController::TeamCanCapPoint(int iIndex, int iTeam)
{
	return m_pObjectiveResource->m_bTeamCanCap(iIndex + iTeam * MAX_CONTROL_POINTS);
}

int CCPController::GetPreviousPointForPoint(int iIndex, int iTeam, int iPrevIdx)
{
	return m_pObjectiveResource->m_iPreviousPoints(iPrevIdx + (iIndex * MAX_PREVIOUS_POINTS) + (iTeam * MAX_CONTROL_POINTS * MAX_PREVIOUS_POINTS));
}

int CCPController::GetFarthestOwnedControlPoint(int iTeam)
{
	int iOwnedEnd = m_pObjectiveResource->m_iBaseControlPoints(iTeam);
	if (iOwnedEnd == -1)
		return -1;

	int iNumControlPoints = m_pObjectiveResource->m_iNumControlPoints();
	int iWalk = 1;
	int iEnemyEnd = iNumControlPoints - 1;
	if (iOwnedEnd != 0)
	{
		iWalk = -1;
		iEnemyEnd = 0;
	}

	// Walk towards the other side, and find the farthest owned point
	int iFarthestPoint = iOwnedEnd;
	for (int iPoint = iOwnedEnd; iPoint != iEnemyEnd; iPoint += iWalk)
	{
		// If we've hit a point we don't own, we're done
		if (m_pObjectiveResource->m_iOwner(iPoint) != iTeam)
			break;

		iFarthestPoint = iPoint;
	}

	return iFarthestPoint;
}

bool CCPController::IsPointUseable(int iIndex, int iTeam)
{
	// We Own it, can't cap it
	if (m_pObjectiveResource->m_iOwner(iIndex) == iTeam)
		return false;

	// Can we cap the point?
	if (!TeamCanCapPoint(iIndex, iTeam))
		return false;

	// We are playing a sectioned map, check if the CP is in it
	if (m_pObjectiveResource->m_bPlayingMiniRounds() && !m_pObjectiveResource->m_bInMiniRound(iIndex))
		return false;

	// Is the point locked?
	if (m_pObjectiveResource->m_bCPLocked(iIndex))
		return false;

	// Linear cap means that it won't require previous points, bail
	static auto tf_caplinear = U::ConVars.FindVar("tf_caplinear");
	if (tf_caplinear->GetBool())
		return true;

	// Any previous points necessary?
	int iPointNeeded = GetPreviousPointForPoint(iIndex, iTeam, 0);

	// Points set to require themselves are always cappable
	if (iPointNeeded == iIndex)
		return true;

	// No required points specified? Require all previous points.
	if (iPointNeeded == -1)
	{
		// No Mini rounds
		if (!m_pObjectiveResource->m_bPlayingMiniRounds())
		{
			// No custom previous point, team must own all previous points
			int iFarthestPoint = GetFarthestOwnedControlPoint(iTeam);
			return (abs(iFarthestPoint - iIndex) <= 1);
		}
		// We got a section map
		else
			// Tf2 itself does not seem to have any more code for this, so here goes
			return true;
	}

	// Loop through each previous point and see if the team owns it
	for (int iPrevPoint = 0; iPrevPoint < MAX_PREVIOUS_POINTS; iPrevPoint++)
	{
		iPointNeeded = GetPreviousPointForPoint(iIndex, iTeam, iPrevPoint);
		if (iPointNeeded != -1)
		{
			// We don't own the needed points
			if (m_pObjectiveResource->m_iOwner(iPointNeeded) != iTeam)
				return false;
		}
	}
	return true;
}

std::optional<Vector> CCPController::GetClosestControlPoint(Vector vPos, int iTeam)
{
	// No resource for it
	if (!m_pObjectiveResource)
		return std::nullopt;

	// Map team to 0-1 and check If Valid
	int iTeamIdx = iTeam - TF_TEAM_RED;
	if (iTeamIdx < 0 || iTeamIdx > 1)
		return std::nullopt;

	// No controlpoints
	if (!m_pObjectiveResource->m_iNumControlPoints())
		return std::nullopt;

	int IgnoreIdx = -1;
	// Do the points need checking because of the map?
	auto sLevelName = SDK::GetLevelName();
	for (auto tIgnore : m_aIgnorePoints)
	{
		// Try to find map name in bad point array
		if (sLevelName.find(tIgnore.m_sMapName) != std::string::npos)
			IgnoreIdx = tIgnore.m_iPointIdx;
	}

	// Find the best and closest control point
	std::optional<Vector> BestControlPoint;
	float flBestDist = FLT_MAX;
	for (auto tControlPoint : m_aControlPointData)
	{
		// Ignore this point
		if (tControlPoint.m_iIdx == IgnoreIdx)
			continue;

		// They can cap
		if (tControlPoint.m_bCanCap.at(iTeamIdx))
		{
			const auto flDist = (*tControlPoint.m_vPos).DistToSqr(vPos);
			// Is it closer?
			if (tControlPoint.m_vPos && flDist < flBestDist)
			{
				flBestDist = flDist;
				BestControlPoint = tControlPoint.m_vPos;
			}
		}
	}

	return BestControlPoint;
}

void CCPController::Init()
{
	for (auto& cp : m_aControlPointData)
		cp = CPInfo();

	m_pObjectiveResource = nullptr;
}

void CCPController::Update()
{
	UpdateObjectiveResource();
	UpdateControlPoints();
}
