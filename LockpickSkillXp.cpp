#include "LockpickSkillXp.h"

#include "config.h"

#include "skse64/GameReferences.h"

#include <unordered_set>

namespace InteractiveLockpickingVR
{
	namespace
	{
		// Skyrim Lockpicking skill (ActorValue::Lockpick).
		constexpr UInt32 kActorValueLockpick = 14;

		constexpr UInt32 kLockpickXpRecordType = 'LKXP';
		constexpr UInt32 kLockpickXpRecordVersion = 1;

		std::unordered_set<UInt32> s_locksAwardedXp;

		const char* GetLockTierName(SInt32 level)
		{
			switch (level)
			{
			case 0: return "Novice";
			case 1: return "Apprentice";
			case 2: return "Adept";
			case 3: return "Expert";
			case 4: return "Master";
			default: return "Unknown";
			}
		}

		int GetFlatXpForLockLevel(SInt32 level)
		{
			switch (level)
			{
			case 0: return lockTierNoviceXp;
			case 1: return lockTierApprenticeXp;
			case 2: return lockTierAdeptXp;
			case 3: return lockTierExpertXp;
			case 4: return lockTierMasterXp;
			default: return 0;
			}
		}

		bool IsPickableLockLevel(SInt32 level)
		{
			return level >= 0 && level <= 4;
		}
	}

	void ApplyLockpickSkillXpOnUnlock(PlayerCharacter* player, TESObjectREFR* doorRef, SInt32 lockLevel)
	{
		if (!player || !doorRef)
			return;

		if (!IsPickableLockLevel(lockLevel))
		{
			LOG_INFO("Lockpick XP: door %08X lock level %d is not pickable, no XP awarded",
				doorRef->formID, lockLevel);
			return;
		}

		const UInt32 doorFormId = doorRef->formID;
		if (s_locksAwardedXp.count(doorFormId) != 0)
		{
			LOG_INFO("Lockpick XP: door %08X already awarded XP this save, skipping",
				doorFormId);
			return;
		}

		const int xp = GetFlatXpForLockLevel(lockLevel);
		if (xp <= 0)
			return;

		player->AdvanceSkill(kActorValueLockpick, static_cast<float>(xp), 0, 0);
		s_locksAwardedXp.insert(doorFormId);

		LOG_INFO("Lockpick XP: awarded %d Lockpicking XP for %s lock on door %08X",
			xp, GetLockTierName(lockLevel), doorFormId);
	}

	void SaveLockpickSkillXp(SKSESerializationInterface* intfc)
	{
		if (!intfc || !intfc->OpenRecord(kLockpickXpRecordType, kLockpickXpRecordVersion))
			return;

		const UInt32 count = static_cast<UInt32>(s_locksAwardedXp.size());
		intfc->WriteRecordData(&count, sizeof(count));

		for (UInt32 formId : s_locksAwardedXp)
			intfc->WriteRecordData(&formId, sizeof(formId));

		if (count > 0)
		{
			LOG_INFO("Lockpick XP: [save] persisted %u door(s) that already received XP",
				count);
		}
	}

	void LoadLockpickSkillXpRecord(SKSESerializationInterface* intfc, UInt32 /*version*/, UInt32 length)
	{
		if (!intfc || length < sizeof(UInt32))
			return;

		UInt32 count = 0;
		if (intfc->ReadRecordData(&count, sizeof(count)) != sizeof(count))
			return;

		const UInt32 maxCount = (length - sizeof(UInt32)) / sizeof(UInt32);
		if (count > maxCount)
			count = maxCount;

		for (UInt32 i = 0; i < count; ++i)
		{
			UInt32 savedId = 0;
			if (intfc->ReadRecordData(&savedId, sizeof(savedId)) != sizeof(savedId))
				break;

			UInt32 resolvedId = 0;
			if (savedId != 0 && intfc->ResolveFormId(savedId, &resolvedId) && resolvedId != 0)
				s_locksAwardedXp.insert(resolvedId);
		}

		LOG_INFO("Lockpick XP: [load] recovered %u door(s) that already received XP",
			static_cast<UInt32>(s_locksAwardedXp.size()));
	}

	void RevertLockpickSkillXp()
	{
		s_locksAwardedXp.clear();
	}
}
