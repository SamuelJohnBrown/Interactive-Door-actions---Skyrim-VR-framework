#include "LockpickNotify.h"

#include "config.h"

#include "skse64_common/Relocation.h"

namespace InteractiveLockpickingVR
{
	// Game HUD notification (same as Papyrus Debug.Notification). SkyrimVR 1.4.15.
	typedef void (*DebugNotificationFn)(const char* msg, const char* soundToPlay, bool cancelIfAlreadyQueued);
	static RelocAddr<DebugNotificationFn> sub_DebugNotification(0x908170);

	// Skyrim.esm|0003D0D3 — magic/action fail UI cue.
	static constexpr const char* kLockpickAlertSoundEditorId = "MAGFailSD";

	void ShowLockpickNotification(const char* message)
	{
		if (!message || !message[0])
			return;

		sub_DebugNotification(message, nullptr, true);
		LOG_INFO("Lockpick notify: %s", message);
	}

	void ShowLockpickAlertNotification(const char* message)
	{
		if (!message || !message[0])
			return;

		sub_DebugNotification(message, kLockpickAlertSoundEditorId, true);
		LOG_INFO("Lockpick alert notify: %s (sound=%s)", message, kLockpickAlertSoundEditorId);
	}
}
