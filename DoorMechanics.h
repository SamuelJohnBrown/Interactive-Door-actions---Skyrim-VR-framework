#pragma once

#include "skse64/GameReferences.h"
#include "skse64/PluginAPI.h"

namespace InteractiveLockpickingVR
{
	// Main hand = right game-hand normally, left game-hand in left-handed VR mode.
	bool IsMainHandLeftGameHand();
	bool GameHandToVRController(bool isLeftGameHand);

	// Spell helpers (EquipSpell / UnequipItem pattern from Shields Unlocked VR).
	void EquipSpellToGameHand(Actor* actor, SpellItem* spell, bool isLeftGameHand);
	void UnequipSpellFromGameHand(Actor* actor, bool isLeftGameHand);

	// Crosshair updates drive start/end of the temporary lockpick-in-main-hand session.
	// Also lazily installs the TESObjectREFR::Activate detour on first use so it
	// safely chains after other plugins' DataLoaded-time hooks (e.g. Fake Edge VR).
	void OnLockpickCrosshairRefChanged(TESObjectREFR* crosshairRef);

	// True when this door is left 100% vanilla by the mod (excluded refs,
	// trap doors, or any locked door with ExcludeLockedDoors=1). Used by the
	// activate-text remover so vanilla doors keep their rollover prompt.
	bool IsDoorLeftVanilla(TESObjectREFR* ref);

	// True when WSActivateRollover should hide its prompt for this door ref.
	bool IsDoorActivateTextSuppressed(TESObjectREFR* ref);

	// SKSE co-save: persists interior doors opened via push that await vanilla close.
	void RegisterDoorMechanicsSerialization(SKSESerializationInterface* serialization, PluginHandle pluginHandle);
	void OnDoorMechanicsPreLoadGame();
	void OnDoorMechanicsPostLoadGame();
}
