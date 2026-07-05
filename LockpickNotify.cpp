#include "LockpickNotify.h"

#include "config.h"

#include "skse64_common/Relocation.h"

namespace InteractiveLockpickingVR
{
	// Game HUD notification (same as Papyrus Debug.Notification). SkyrimVR 1.4.15.
	typedef void (*DebugNotificationFn)(const char* msg, const char* soundToPlay, bool cancelIfAlreadyQueued);
	static RelocAddr<DebugNotificationFn> sub_DebugNotification(0x908170);

	void ShowLockpickNotification(const char* message)
	{
		if (!message || !message[0])
			return;

		sub_DebugNotification(message, nullptr, true);
		LOG_INFO("Lockpick notify: %s", message);
	}
}
