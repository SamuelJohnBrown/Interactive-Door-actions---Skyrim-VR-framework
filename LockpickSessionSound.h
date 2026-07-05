#pragma once

class TESObjectREFR;

namespace InteractiveLockpickingVR
{
	void InitLockpickSessionSound();
	void StartLockpickSessionSound(TESObjectREFR* doorRef);
	void StopLockpickSessionSound();
	void PlayUnlockSuccessSound(TESObjectREFR* doorRef);
	void PlayLockpickBreakSound(TESObjectREFR* doorRef);
}
