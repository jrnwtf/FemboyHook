#include "../SDK/SDK.h"

MAKE_SIGNATURE(S_StartSound, "engine.dll", "40 53 48 83 EC ? 48 83 79 ? ? 48 8B D9 75 ? 33 C0", 0x0);

const static std::vector<std::string> NOISEMAKER_SOUNDS{ "items\\halloween", "items\\football_manager", "items\\japan_fundraiser", "items\\samurai", "items\\summer", "misc\\happy_birthday_tf", "misc\\jingle_bells" };

MAKE_HOOK(S_StartSound, S::S_StartSound(), int, StartSoundParams_t& params)
{
#ifdef DEBUG_HOOKS
	if (!Vars::Hooks::S_StartSound[DEFAULT_BIND])
		return CALL_ORIGINAL(params);
#endif

	if (!params.pSfx || !params.pSfx->getname())
		return CALL_ORIGINAL(params);

	std::string soundName = params.pSfx->getname();

	if (Vars::Misc::Sound::Block.Value & (1 << 0))
	{
		if (soundName.find("footsteps") != std::string::npos)
			return 0;
	}

	if (Vars::Misc::Sound::Block.Value & (1 << 1))
	{
		for (auto& sound : NOISEMAKER_SOUNDS)
		{
			if (soundName.find(sound) != std::string::npos)
				return 0;
		}
	}

	if (Vars::Misc::Sound::Block.Value & (1 << 2))
	{
		if (soundName.find("weapons\\pan") != std::string::npos)
			return 0;
	}

	return CALL_ORIGINAL(params);
}