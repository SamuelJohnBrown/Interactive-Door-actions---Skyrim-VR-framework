#include "LockpickBreak.h"

#include "config.h"

#include "skse64/GameReferences.h"

#include <cstdlib>
#include <cmath>

namespace InteractiveLockpickingVR
{
	namespace
	{
		constexpr UInt32 kActorValueLockpick = 14;

		// Break chance (%) by Lockpicking skill level 1-100, per lock tier.
		constexpr float kNoviceBreakChance[100] = {
			20.00f, 19.80f, 19.60f, 19.39f, 19.19f, 18.99f, 18.79f, 18.59f, 18.38f, 18.18f,
			17.98f, 17.78f, 17.58f, 17.37f, 17.17f, 16.97f, 16.77f, 16.57f, 16.36f, 16.16f,
			15.96f, 15.76f, 15.56f, 15.35f, 15.15f, 14.95f, 14.75f, 14.55f, 14.34f, 14.14f,
			13.94f, 13.74f, 13.54f, 13.33f, 13.13f, 12.93f, 12.73f, 12.53f, 12.32f, 12.12f,
			11.92f, 11.72f, 11.52f, 11.31f, 11.11f, 10.91f, 10.71f, 10.51f, 10.30f, 10.10f,
			9.90f, 9.70f, 9.49f, 9.29f, 9.09f, 8.89f, 8.69f, 8.48f, 8.28f, 8.08f,
			7.88f, 7.68f, 7.47f, 7.27f, 7.07f, 6.87f, 6.67f, 6.46f, 6.26f, 6.06f,
			5.86f, 5.66f, 5.45f, 5.25f, 5.05f, 4.85f, 4.65f, 4.44f, 4.24f, 4.04f,
			3.84f, 3.64f, 3.43f, 3.23f, 3.03f, 2.83f, 2.63f, 2.42f, 2.22f, 2.02f,
			1.82f, 1.62f, 1.41f, 1.21f, 1.01f, 0.81f, 0.61f, 0.40f, 0.20f, 0.00f
		};

		constexpr float kApprenticeBreakChance[100] = {
			30.00f, 29.70f, 29.39f, 29.09f, 28.79f, 28.48f, 28.18f, 27.88f, 27.58f, 27.27f,
			26.97f, 26.67f, 26.36f, 26.06f, 25.76f, 25.45f, 25.15f, 24.85f, 24.55f, 24.24f,
			23.94f, 23.64f, 23.33f, 23.03f, 22.73f, 22.42f, 22.12f, 21.82f, 21.52f, 21.21f,
			20.91f, 20.61f, 20.30f, 20.00f, 19.70f, 19.39f, 19.09f, 18.79f, 18.48f, 18.18f,
			17.88f, 17.58f, 17.27f, 16.97f, 16.67f, 16.36f, 16.06f, 15.76f, 15.45f, 15.15f,
			14.85f, 14.55f, 14.24f, 13.94f, 13.64f, 13.33f, 13.03f, 12.73f, 12.42f, 12.12f,
			11.82f, 11.52f, 11.21f, 10.91f, 10.61f, 10.30f, 10.00f, 9.70f, 9.39f, 9.09f,
			8.79f, 8.48f, 8.18f, 7.88f, 7.58f, 7.27f, 6.97f, 6.67f, 6.36f, 6.06f,
			5.76f, 5.45f, 5.15f, 4.85f, 4.55f, 4.24f, 3.94f, 3.64f, 3.33f, 3.03f,
			2.73f, 2.42f, 2.12f, 1.82f, 1.52f, 1.21f, 0.91f, 0.61f, 0.30f, 0.00f
		};

		constexpr float kAdeptBreakChance[100] = {
			40.00f, 39.60f, 39.19f, 38.79f, 38.38f, 37.98f, 37.58f, 37.17f, 36.77f, 36.36f,
			35.96f, 35.56f, 35.15f, 34.75f, 34.34f, 33.94f, 33.54f, 33.13f, 32.73f, 32.32f,
			31.92f, 31.52f, 31.11f, 30.71f, 30.30f, 29.90f, 29.49f, 29.09f, 28.69f, 28.28f,
			27.88f, 27.47f, 27.07f, 26.67f, 26.26f, 25.86f, 25.45f, 25.05f, 24.65f, 24.24f,
			23.84f, 23.43f, 23.03f, 22.63f, 22.22f, 21.82f, 21.41f, 21.01f, 20.61f, 20.20f,
			19.80f, 19.39f, 18.99f, 18.59f, 18.18f, 17.78f, 17.37f, 16.97f, 16.57f, 16.16f,
			15.76f, 15.35f, 14.95f, 14.55f, 14.14f, 13.74f, 13.33f, 12.93f, 12.53f, 12.12f,
			11.72f, 11.31f, 10.91f, 10.51f, 10.10f, 9.70f, 9.29f, 8.89f, 8.48f, 8.08f,
			7.68f, 7.27f, 6.87f, 6.46f, 6.06f, 5.66f, 5.25f, 4.85f, 4.44f, 4.04f,
			3.64f, 3.23f, 2.83f, 2.42f, 2.02f, 1.62f, 1.21f, 0.81f, 0.40f, 0.00f
		};

		constexpr float kExpertBreakChance[100] = {
			50.00f, 49.49f, 48.99f, 48.48f, 47.98f, 47.47f, 46.97f, 46.46f, 45.96f, 45.45f,
			44.95f, 44.44f, 43.94f, 43.43f, 42.93f, 42.42f, 41.92f, 41.41f, 40.91f, 40.40f,
			39.90f, 39.39f, 38.89f, 38.38f, 37.88f, 37.37f, 36.87f, 36.36f, 35.86f, 35.35f,
			34.85f, 34.34f, 33.84f, 33.33f, 32.83f, 32.32f, 31.82f, 31.31f, 30.81f, 30.30f,
			29.80f, 29.29f, 28.79f, 28.28f, 27.78f, 27.27f, 26.77f, 26.26f, 25.76f, 25.25f,
			24.75f, 24.24f, 23.74f, 23.23f, 22.73f, 22.22f, 21.72f, 21.21f, 20.71f, 20.20f,
			19.70f, 19.19f, 18.69f, 18.18f, 17.68f, 17.17f, 16.67f, 16.16f, 15.66f, 15.15f,
			14.65f, 14.14f, 13.64f, 13.13f, 12.63f, 12.12f, 11.62f, 11.11f, 10.61f, 10.10f,
			9.60f, 9.09f, 8.59f, 8.08f, 7.58f, 7.07f, 6.57f, 6.06f, 5.56f, 5.05f,
			4.55f, 4.04f, 3.54f, 3.03f, 2.53f, 2.02f, 1.52f, 1.01f, 0.51f, 0.00f
		};

		constexpr float kMasterBreakChance[100] = {
			60.00f, 59.39f, 58.79f, 58.18f, 57.58f, 56.97f, 56.36f, 55.76f, 55.15f, 54.55f,
			53.94f, 53.33f, 52.73f, 52.12f, 51.52f, 50.91f, 50.30f, 49.70f, 49.09f, 48.48f,
			47.88f, 47.27f, 46.67f, 46.06f, 45.45f, 44.85f, 44.24f, 43.64f, 43.03f, 42.42f,
			41.82f, 41.21f, 40.61f, 40.00f, 39.39f, 38.79f, 38.18f, 37.58f, 36.97f, 36.36f,
			35.76f, 35.15f, 34.55f, 33.94f, 33.33f, 32.73f, 32.12f, 31.52f, 30.91f, 30.30f,
			29.70f, 29.09f, 28.48f, 27.88f, 27.27f, 26.67f, 26.06f, 25.45f, 24.85f, 24.24f,
			23.64f, 23.03f, 22.42f, 21.82f, 21.21f, 20.61f, 20.00f, 19.39f, 18.79f, 18.18f,
			17.58f, 16.97f, 16.36f, 15.76f, 15.15f, 14.55f, 13.94f, 13.33f, 12.73f, 12.12f,
			11.52f, 10.91f, 10.30f, 9.70f, 9.09f, 8.48f, 7.88f, 7.27f, 6.67f, 6.06f,
			5.45f, 4.85f, 4.24f, 3.64f, 3.03f, 2.42f, 1.82f, 1.21f, 0.61f, 0.00f
		};

		const float* GetBreakTableForLockLevel(SInt32 lockLevel)
		{
			switch (lockLevel)
			{
			case 0: return kNoviceBreakChance;
			case 1: return kApprenticeBreakChance;
			case 2: return kAdeptBreakChance;
			case 3: return kExpertBreakChance;
			case 4: return kMasterBreakChance;
			default: return kAdeptBreakChance;
			}
		}

		const char* GetBreakTierName(SInt32 lockLevel)
		{
			switch (lockLevel)
			{
			case 0: return "Novice";
			case 1: return "Apprentice";
			case 2: return "Adept";
			case 3: return "Expert";
			case 4: return "Master";
			default: return "Unknown";
			}
		}
	}

	int GetPlayerLockpickingSkillLevel(PlayerCharacter* player)
	{
		if (!player)
			return 1;

		const int level = static_cast<int>(std::lround(player->actorValueOwner.GetBase(kActorValueLockpick)));
		if (level < 1)
			return 1;
		if (level > 100)
			return 100;
		return level;
	}

	float GetLockpickBreakChancePercent(SInt32 lockLevel, int skillLevel)
	{
		if (skillLevel < 1)
			skillLevel = 1;
		if (skillLevel > 100)
			skillLevel = 100;

		const float* table = GetBreakTableForLockLevel(lockLevel);
		return table[skillLevel - 1];
	}

	bool RollLockpickBreak(PlayerCharacter* player, SInt32 lockLevel)
	{
		const int skillLevel = GetPlayerLockpickingSkillLevel(player);
		const float chancePercent = GetLockpickBreakChancePercent(lockLevel, skillLevel);
		if (chancePercent <= 0.0f)
			return false;

		const int roll = std::rand() % 10000;
		const int threshold = static_cast<int>(chancePercent * 100.0f + 0.5f);
		const bool broke = roll < threshold;

		if (broke)
		{
			LOG_INFO("Lockpick break: rolled break at skill %d on %s lock (%.2f%% chance)",
				skillLevel, GetBreakTierName(lockLevel), chancePercent);
		}

		return broke;
	}
}
