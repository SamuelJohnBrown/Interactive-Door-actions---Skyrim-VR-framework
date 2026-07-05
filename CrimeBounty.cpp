#include "CrimeBounty.h"

#include <cstdint>

#include "config.h"

#include "skse64/GameAPI.h"
#include "skse64/GameData.h"
#include "skse64/GameExtraData.h"
#include "skse64/GameForms.h"
#include "skse64/GameObjects.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameTypes.h"
#include "skse64/gamethreads.h"
#include "skse64/PluginAPI.h"
#include "skse64_common/Relocation.h"

namespace InteractiveLockpickingVR
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		constexpr SInt32 kEngineDefaultCrimeGold = -1;

		enum class CrimeOwnerSource : std::uint8_t
		{
			None = 0,
			RefrOwnerForm,
			ExtraOwnership,
			CellOwnerForm,
			DoorLocationFaction,
			CellLocationFaction,
			PlayerLocationFaction,
			WitnessCrimeFaction,
		};

		// CommonLibVR ProcessLists layout — highActorHandles @ 0x30.
		struct CrimeProcessLists
		{
			std::uint8_t pad001[0x30];
			tArray<UInt32> highActorHandles;
			tArray<UInt32> lowActorHandles;
			tArray<UInt32> middleHighActorHandles;
			tArray<UInt32> middleLowActorHandles;
		};

		// SkyrimVR 1.4.15 — Actor::TrespassAlarm (addresslib id 36432).
		typedef void (*Actor_TrespassAlarmFn)(Actor*, TESObjectREFR*, TESForm*, SInt32);
		static RelocAddr<Actor_TrespassAlarmFn> Actor_TrespassAlarm(0x005E7C80);

		// Actor::RequestDetectionLevel (addresslib id 36748) — per-actor detection
		// of a target. Used instead of the global
		// ProcessLists::RequestHighestDetectionLevelAgainstActor scan, which CTDs
		// when the high-actor list contains actors in broken/detached cells
		// (e.g. Helgen intro keeps an Embers XD Wilderness wolf loaded).
		typedef SInt32 (*Actor_RequestDetectionLevelFn)(Actor*, Actor*, SInt32);
		static RelocAddr<Actor_RequestDetectionLevelFn> Actor_RequestDetectionLevel(0x00605190);

		constexpr SInt32 kDetectionPriorityNormal = 3;
		constexpr float kMaxWitnessDistance = 2048.0f;

		// TESObjectREFR::GetOwner — NOT the vtable thunk at 0x002A6670 (returns garbage).
		typedef TESForm* (*TESObjectREFR_GetOwnerFn)(const TESObjectREFR*);
		static RelocAddr<TESObjectREFR_GetOwnerFn> TESObjectREFR_GetOwner(0x002B7DE0);

		// NOTE: the old TESObjectCELL::GetOwnerForm entry (0x00265020) was an SSE
		// offset that lands MID-FUNCTION in the VR binary (inside FUN_140264eb0)
		// and was the cause of every cage-door lockpick CTD. It is intentionally
		// gone: TESObjectREFR::GetOwner already falls back to the cell owner.

		typedef BGSLocation* (*TESObjectREFR_GetCurrentLocationFn)(const TESObjectREFR*);
		static RelocAddr<TESObjectREFR_GetCurrentLocationFn> TESObjectREFR_GetCurrentLocation(0x002AABF0);

		// addresslib id 18474: SSE 0x140262290 -> VR 0x140273830.
		typedef BGSLocation* (*TESObjectCELL_GetLocationFn)(const TESObjectCELL*);
		static RelocAddr<TESObjectCELL_GetLocationFn> TESObjectCELL_GetLocation(0x00273830);

		// NOTE: Actor::GetCrimeFaction at 0x005DA660 was another unverified SSE
		// offset that lands mid-function in VR (inside Actor::Jump). Removed along
		// with the witness-faction owner fallback that used it.

		CrimeProcessLists* GetProcessLists()
		{
			static RelocPtr<CrimeProcessLists*> singleton(0x01F831B0);
			return singleton ? *singleton : nullptr;
		}

		bool IsPlausibleHeapPointer(const void* pointer)
		{
			if (!pointer)
				return false;

			const auto address = reinterpret_cast<std::uintptr_t>(pointer);
			if (address < 0x10000ULL)
				return false;

			// Reject module/code pointers mistaken for heap objects.
			if (address >= 0x00007FF000000000ULL)
				return false;

			return true;
		}

		bool IsPlausibleCrimeOwner(TESForm* owner)
		{
			if (!IsPlausibleHeapPointer(owner))
				return false;

			if (owner->formID == 0)
				return false;

			switch (owner->formType)
			{
			case kFormType_Faction:
			case kFormType_NPC:
				return LookupFormByID(owner->formID) == owner;
			default:
				return false;
			}
		}

		TESForm* GetExtraOwnershipOwner(TESObjectREFR* refr)
		{
			if (!refr)
				return nullptr;

			ExtraOwnership* ownership = static_cast<ExtraOwnership*>(
				refr->extraData.GetByType(kExtraData_Ownership));
			if (!ownership || !ownership->owner)
				return nullptr;

			return ownership->owner;
		}

		TESFaction* GetLocationCrimeFaction(BGSLocation* location)
		{
			while (IsPlausibleHeapPointer(location))
			{
				auto* faction = *reinterpret_cast<TESFaction**>(
					reinterpret_cast<std::uintptr_t>(location) + 0x50);
				if (IsPlausibleCrimeOwner(faction))
					return faction;

				location = *reinterpret_cast<BGSLocation**>(
					reinterpret_cast<std::uintptr_t>(location) + 0x48);
			}

			return nullptr;
		}

		TESForm* GetLocationCrimeOwner(BGSLocation* location)
		{
			TESFaction* faction = GetLocationCrimeFaction(location);
			return faction ? static_cast<TESForm*>(faction) : nullptr;
		}

		bool IsValidPlayerActor(Actor* actor)
		{
			if (!IsPlausibleHeapPointer(actor))
				return false;

			return actor->formType == kFormType_Character;
		}

		// Strict validation: the Helgen intro (and alternate starts) can keep
		// actors loaded in detached cells (e.g. an Embers XD "Wilderness" wolf).
		// Passing those to engine detection code corrupts the scrap heap and CTDs,
		// so anything not fully attached to the player's cell space is skipped.
		bool IsValidWitnessActor(Actor* actor, TESObjectREFR* player)
		{
			if (!IsPlausibleHeapPointer(actor) || !player)
				return false;

			if (actor == reinterpret_cast<Actor*>(player))
				return false;

			if (actor->formType != kFormType_Character)
				return false;

			if (!IsPlausibleHeapPointer(actor->processManager))
				return false;

			if (!IsPlausibleHeapPointer(actor->parentCell))
				return false;

			// Witness must be in the player's own cell; cross-cell "witnesses"
			// are exactly the broken actors that crash the engine scan.
			if (player->parentCell && actor->parentCell != player->parentCell)
				return false;

			if (actor->IsDead(1))
				return false;

			const float dx = actor->pos.x - player->pos.x;
			const float dy = actor->pos.y - player->pos.y;
			const float dz = actor->pos.z - player->pos.z;
			const float distSq = dx * dx + dy * dy + dz * dz;
			if (distSq > kMaxWitnessDistance * kMaxWitnessDistance)
				return false;

			return true;
		}

		TESForm* ResolveLockpickCrimeOwner(
			TESObjectREFR* door,
			TESObjectREFR* player,
			CrimeOwnerSource* outSource)
		{
			if (outSource)
				*outSource = CrimeOwnerSource::None;

			if (!door)
				return nullptr;

			auto tryOwner = [&](TESForm* owner, CrimeOwnerSource source) -> TESForm*
			{
				if (!IsPlausibleCrimeOwner(owner))
					return nullptr;
				if (outSource)
					*outSource = source;
				return owner;
			};

			if (TESObjectREFR_GetOwner)
			{
				if (TESForm* owner = tryOwner(TESObjectREFR_GetOwner(door), CrimeOwnerSource::RefrOwnerForm))
					return owner;
			}

			if (TESForm* owner = tryOwner(GetExtraOwnershipOwner(door), CrimeOwnerSource::ExtraOwnership))
				return owner;

			if (TESObjectREFR_GetCurrentLocation)
			{
				if (TESForm* owner = tryOwner(
						GetLocationCrimeOwner(TESObjectREFR_GetCurrentLocation(door)),
						CrimeOwnerSource::DoorLocationFaction))
					return owner;
			}

			if (door->parentCell && TESObjectCELL_GetLocation)
			{
				if (TESForm* owner = tryOwner(
						GetLocationCrimeOwner(TESObjectCELL_GetLocation(door->parentCell)),
						CrimeOwnerSource::CellLocationFaction))
					return owner;
			}

			if (player && TESObjectREFR_GetCurrentLocation)
			{
				if (TESForm* owner = tryOwner(
						GetLocationCrimeOwner(TESObjectREFR_GetCurrentLocation(player)),
						CrimeOwnerSource::PlayerLocationFaction))
					return owner;
			}

			return nullptr;
		}

		const char* CrimeOwnerSourceName(CrimeOwnerSource source)
		{
			switch (source)
			{
			case CrimeOwnerSource::RefrOwnerForm:
				return "refr GetOwner";
			case CrimeOwnerSource::ExtraOwnership:
				return "ExtraOwnership";
			case CrimeOwnerSource::CellOwnerForm:
				return "cell GetOwnerForm";
			case CrimeOwnerSource::DoorLocationFaction:
				return "door location FNAM";
			case CrimeOwnerSource::CellLocationFaction:
				return "cell location FNAM";
			case CrimeOwnerSource::PlayerLocationFaction:
				return "player location FNAM";
			case CrimeOwnerSource::WitnessCrimeFaction:
				return "witness crime faction";
			default:
				return "none";
			}
		}

		// SEH guards around raw engine calls. The global
		// ProcessLists::RequestHighestDetectionLevelAgainstActor scan CTD'd even
		// with guards because it corrupts thread scrap-heap state on broken
		// actors, so witnesses are now checked one actor at a time with strict
		// validation, and each engine call is individually fenced.
		// No C++ objects with destructors allowed in __try functions.
		bool SafeRequestDetectionLevel(
			Actor_RequestDetectionLevelFn fn,
			Actor* witness,
			Actor* target,
			SInt32* outLevel)
		{
			__try
			{
				*outLevel = fn(witness, target, kDetectionPriorityNormal);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool SafeTrespassAlarm(
			Actor_TrespassAlarmFn fn,
			Actor* player,
			TESObjectREFR* door,
			TESForm* owner,
			SInt32 crimeGold)
		{
			__try
			{
				fn(player, door, owner, crimeGold);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Counts same-cell, alive, nearby NPCs that currently detect the player.
		// Replaces the vanilla global witness scan (see comment above).
		bool HasLockpickCrimeWitnesses(TESObjectREFR* player, UInt32* outWitnessCount)
		{
			if (!player)
				return false;

			Actor* playerActor = static_cast<Actor*>(player);
			if (!IsValidPlayerActor(playerActor))
				return false;

			if (!IsPlausibleHeapPointer(player->parentCell))
				return false;

			CrimeProcessLists* processLists = GetProcessLists();
			if (!processLists)
				return false;

			const tArray<UInt32>& handles = processLists->highActorHandles;
			if (!handles.entries || handles.count == 0)
				return false;

			UInt32 witnessCount = 0;
			for (UInt32 i = 0; i < handles.count; ++i)
			{
				UInt32 handle = handles.entries[i];
				if (!handle)
					continue;

				NiPointer<TESObjectREFR> refr;
				if (!LookupREFRByHandle(handle, refr) || !refr)
					continue;

				Actor* actor = (refr->formType == kFormType_Character)
					? static_cast<Actor*>(refr.get())
					: nullptr;
				if (!IsValidWitnessActor(actor, player))
					continue;

				SInt32 detectionLevel = 0;
				if (!SafeRequestDetectionLevel(
						Actor_RequestDetectionLevel,
						actor,
						playerActor,
						&detectionLevel))
				{
					LOG_INFO(
						"[Crime] Detection check faulted on actor %08X; skipping",
						actor->formID);
					continue;
				}

				if (detectionLevel > 0)
					++witnessCount;
			}

			if (outWitnessCount)
				*outWitnessCount = witnessCount;

			return witnessCount > 0;
		}

		void TriggerVanillaLockpickCrimeAlarm(
			TESObjectREFR* door,
			Actor* player,
			TESForm* owner)
		{
			if (!door || !player || !IsPlausibleCrimeOwner(owner))
				return;

			if (!SafeTrespassAlarm(
					Actor_TrespassAlarm,
					player,
					door,
					owner,
					kEngineDefaultCrimeGold))
			{
				LOG_INFO("[Crime] TrespassAlarm faulted for door %08X; alarm skipped", door->formID);
			}
		}

		class LockpickCrimeAlarmTask : public TaskDelegate
		{
		public:
			LockpickCrimeAlarmTask(
				UInt32 doorFormId,
				UInt32 ownerFormId,
				CrimeOwnerSource ownerSource,
				UInt32 witnessCount)
				: m_doorFormId(doorFormId)
				, m_ownerFormId(ownerFormId)
				, m_ownerSource(ownerSource)
				, m_witnessCount(witnessCount)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				if (!player)
					return;

				TESForm* doorForm = LookupFormByID(m_doorFormId);
				TESObjectREFR* door = doorForm
					? DYNAMIC_CAST(doorForm, TESForm, TESObjectREFR)
					: nullptr;
				TESForm* owner = LookupFormByID(m_ownerFormId);

				if (!door || !IsPlausibleCrimeOwner(owner))
				{
					LOG_INFO(
						"[Crime] Deferred alarm aborted for door %08X (door=%p owner=%p)",
						m_doorFormId,
						door,
						owner);
					return;
				}

				Actor* actor = static_cast<Actor*>(static_cast<TESObjectREFR*>(player));
				TriggerVanillaLockpickCrimeAlarm(door, actor, owner);

				LOG_INFO(
					"[Crime] Witnessed lockpick alarm | door=%08X owner=%08X source=%s witnesses=%u "
					"TrespassAlarm(door, owner, -1)",
					door->formID,
					owner->formID,
					CrimeOwnerSourceName(m_ownerSource),
					m_witnessCount);
			}

			virtual void Dispose() override
			{
				delete this;
			}

		private:
			UInt32 m_doorFormId;
			UInt32 m_ownerFormId;
			CrimeOwnerSource m_ownerSource;
			UInt32 m_witnessCount;
		};

	}  // namespace

	void ApplyLockpickBountyIfWitnessed(PlayerCharacter* player, TESObjectREFR* door)
	{
		if (!player || !door)
			return;

		if (IsExcludedFromLockpickCrime(door))
		{
			LOG_INFO("[Crime] Door %08X excluded from lockpick crime/bounty", door->formID);
			return;
		}

		TESObjectREFR* playerRef = static_cast<TESObjectREFR*>(player);

		UInt32 witnessCount = 0;
		if (!HasLockpickCrimeWitnesses(playerRef, &witnessCount))
		{
			LOG_INFO("[Crime] Unwitnessed lockpick; no alarm for door %08X", door->formID);
			return;
		}

		CrimeOwnerSource ownerSource = CrimeOwnerSource::None;
		TESForm* owner = ResolveLockpickCrimeOwner(door, playerRef, &ownerSource);
		if (!owner)
		{
			LOG_INFO(
				"[Crime] Witnessed lockpick on door %08X but no crime owner resolved (%u witness(s))",
				door->formID,
				witnessCount);
			return;
		}

		if (!g_task)
		{
			LOG_INFO("[Crime] Task interface unavailable; skipping alarm for door %08X", door->formID);
			return;
		}

		// Defer TrespassAlarm until after the lockpick session finishes unlocking.
		// Calling it synchronously from SessionCheckTask/TriggerDoorUnlock was CTD'ing
		// when the engine iterated witnesses during intro scenes (e.g. Helgen cage door).
		g_task->AddTask(new LockpickCrimeAlarmTask(
			door->formID,
			owner->formID,
			ownerSource,
			witnessCount));

		LOG_INFO(
			"[Crime] Queued witnessed lockpick alarm for door %08X owner=%08X source=%s witnesses=%u",
			door->formID,
			owner->formID,
			CrimeOwnerSourceName(ownerSource),
			witnessCount);
	}

}  // namespace InteractiveLockpickingVR
