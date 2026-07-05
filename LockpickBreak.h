#pragma once

class PlayerCharacter;

namespace InteractiveLockpickingVR
{
	int GetPlayerLockpickingSkillLevel(PlayerCharacter* player);
	float GetLockpickBreakChancePercent(SInt32 lockLevel, int skillLevel);
	bool RollLockpickBreak(PlayerCharacter* player, SInt32 lockLevel);
}
