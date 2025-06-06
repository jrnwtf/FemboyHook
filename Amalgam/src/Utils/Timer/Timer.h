#pragma once
#include <chrono>

/*
 *	Credits to cathook (nullifiedcat)
 */

class Timer
{
private:
	float m_flLast = 0.f;

public:
	Timer();
	bool Check(float flS) const;
	bool Run(float flS);
	inline void Update();

	inline void operator-=(float flS)
    {
		m_flLast -= flS;
    }

	inline void operator+=(float flS)
    {
        m_flLast += flS;
    }
};