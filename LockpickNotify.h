#pragma once

namespace InteractiveLockpickingVR
{
	void ShowLockpickNotification(const char* message);

	// Failure / blocked feedback: HUD text plus Skyrim.esm|0003D0D3 (MAGFailSD).
	void ShowLockpickAlertNotification(const char* message);
}
