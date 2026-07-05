#pragma once

#include "skse64/PluginAPI.h"

class PlayerCharacter;
class TESObjectREFR;

namespace InteractiveLockpickingVR
{
	// Awards flat Lockpicking skill XP once per door ref per save.
	void ApplyLockpickSkillXpOnUnlock(PlayerCharacter* player, TESObjectREFR* doorRef, SInt32 lockLevel);

	void SaveLockpickSkillXp(SKSESerializationInterface* intfc);
	void LoadLockpickSkillXpRecord(SKSESerializationInterface* intfc, UInt32 version, UInt32 length);
	void RevertLockpickSkillXp();
}
