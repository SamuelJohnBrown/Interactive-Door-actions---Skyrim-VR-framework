#include "Engine.h"
#include "DoorMechanics.h"

namespace InteractiveLockpickingVR
{
	SKSETrampolineInterface* g_trampolineInterface = nullptr;

	HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	vrikPluginApi::IVrikInterface001* vrikInterface;

	SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;

	// ------------------------------------------------------------------
	// Activate rollover text remover (doors only)
	//
	// In Skyrim VR the floating "Open / Take / Talk ..." prompt is drawn by
	// a dedicated world-space Scaleform menu named "WSActivateRollover",
	// parented to the controller wand node. It is display-only: the actual
	// activation is driven by CrosshairPickData + PlayerCharacter's
	// ActivatePickRef, which we never touch. Suppressing this menu's text
	// therefore hides the prompt without affecting activation in any way.
	//
	// We patch the menu's vtable entry for IMenu::ProcessMessage (slot 4).
	// SKSE's crosshair ref event tells us what the player is currently
	// pointing at; when that target is a door, we swallow the "Show Text"
	// message that would populate the prompt and keep the movie invisible.
	// For every other target the message passes through and the movie is
	// made visible again, so all non-door prompts behave like vanilla.
	// ------------------------------------------------------------------

	typedef UInt32 (*WSRollover_ProcessMessage_t)(IMenu* menu, UIMessage* message);

	static WSRollover_ProcessMessage_t g_origWSRolloverProcessMessage = nullptr;
	static bool g_activateTextHookInstalled = false;

	// Updated from SKSE's crosshair ref event, read in the menu hook.
	// Both run on the main thread.
	static bool g_crosshairTargetIsDoor = false;

	class CrosshairDoorTracker : public BSTEventSink<SKSECrosshairRefEvent>
	{
	public:
		virtual EventResult ReceiveEvent(SKSECrosshairRefEvent* evn, EventDispatcher<SKSECrosshairRefEvent>* dispatcher) override
		{
			TESObjectREFR* ref = evn ? evn->crosshairRef : nullptr;

			// In VR this fires per tracked device (hands + headset), so a
			// device pointing at nothing must not clear a door flag set by
			// the hand that is actually on a target. With no target there is
			// no rollover text anyway, so keeping the last real value is safe.
			if (ref && ref->baseForm)
			{
				// Doors the mod leaves fully vanilla, or push-excluded bases,
				// keep their rollover prompt.
				g_crosshairTargetIsDoor = IsDoorActivateTextSuppressed(ref);
				LOG_INFO("Crosshair ref changed: formType=%d door=%d", (int)ref->baseForm->formType, (int)g_crosshairTargetIsDoor);
			}

			OnLockpickCrosshairRefChanged(ref);

			return kEvent_Continue;
		}
	};
	static CrosshairDoorTracker g_crosshairDoorTracker;

	static UInt32 WSActivateRollover_ProcessMessage_Hook(IMenu* menu, UIMessage* message)
	{
		// If we can positively identify a "Show Text" message while pointing
		// at a door, swallow it so the prompt content is never even set.
		if (g_crosshairTargetIsDoor && message && message->message == UIMessage::kMessage_Data && message->objData)
		{
			BSUIMessageData* data = DYNAMIC_CAST(message->objData, IUIMessageData, BSUIMessageData);
			UIStringHolder* stringHolder = UIStringHolder::GetSingleton();

			// BSFixedStrings are interned, pointer comparison is enough.
			if (data && stringHolder && data->unk18.data && data->unk18.data == stringHolder->showText.data)
			{
				LOG_INFO("Swallowed door 'Show Text' message");

				if (menu && menu->view)
					menu->view->SetVisible(false);

				return GFxMovieView::kProcessed;
			}
		}

		const UInt32 result = g_origWSRolloverProcessMessage(menu, message);

		// Main layer: re-assert visibility on every message the rollover
		// receives, hidden while pointing at a door and visible otherwise.
		// This is what actually guarantees the text stays hidden - it does
		// not depend on recognizing any particular message format.
		if (menu && menu->view)
			menu->view->SetVisible(!g_crosshairTargetIsDoor);

		return result;
	}

	static void TryInstallActivateTextHook()
	{
		if (g_activateTextHookInstalled || removeActivateText == 0)
			return;

		MenuManager* menuManager = MenuManager::GetSingleton();
		if (!menuManager)
			return;

		// The menu instance only exists once the engine has created the
		// world-space HUD menus, so this can fail until then.
		BSFixedString menuName("WSActivateRollover");
		IMenu* menu = menuManager->GetMenu(&menuName);
		if (!menu)
			return;

		uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(menu);

		// IMenu vtable: 0 dtor, 1 Accept, 2 Unk_02, 3 Unk_03, 4 ProcessMessage
		g_origWSRolloverProcessMessage = reinterpret_cast<WSRollover_ProcessMessage_t>(vtable[4]);
		SafeWrite64(reinterpret_cast<uintptr_t>(&vtable[4]), reinterpret_cast<UInt64>(&WSActivateRollover_ProcessMessage_Hook));

		g_activateTextHookInstalled = true;
		_MESSAGE("WSActivateRollover hooked: activate rollover text disabled for doors (activation untouched).");
	}

	// Installs the hook as soon as the WSActivateRollover menu instance
	// exists. Menu open/close events fire constantly during startup and
	// gameplay, so this catches both new games and loaded saves.
	class ActivateTextMenuEventHandler : public BSTEventSink<MenuOpenCloseEvent>
	{
	public:
		virtual EventResult ReceiveEvent(MenuOpenCloseEvent* evn, EventDispatcher<MenuOpenCloseEvent>* dispatcher) override
		{
			TryInstallActivateTextHook();
			return kEvent_Continue;
		}
	};
	static ActivateTextMenuEventHandler g_activateTextMenuEventHandler;

	// Called from main.cpp once SKSE messaging is available, so we can sink
	// SKSE's crosshair ref event and know what the player is pointing at.
	void RegisterActivateTextSinks(SKSEMessagingInterface* messaging)
	{
		if (removeActivateText == 0 || !messaging)
			return;

		auto* crosshairDispatcher = static_cast<EventDispatcher<SKSECrosshairRefEvent>*>(
			messaging->GetEventDispatcher(SKSEMessagingInterface::kDispatcher_CrosshairEvent));

		if (crosshairDispatcher)
		{
			crosshairDispatcher->AddEventSink(&g_crosshairDoorTracker);
			_MESSAGE("Crosshair ref event sink registered (door tracking).");
		}
		else
		{
			_ERROR("Couldn't get SKSE crosshair event dispatcher, door detection unavailable.");
		}
	}

	void StartMod()
	{
		if (removeActivateText == 0)
		{
			_MESSAGE("RemoveActivateText is disabled in the ini, rollover text left untouched.");
			return;
		}

		// In case the menu already exists (e.g. plugin reloaded mid-session).
		TryInstallActivateTextHook();

		if (!g_activateTextHookInstalled)
		{
			MenuManager* menuManager = MenuManager::GetSingleton();
			if (menuManager)
			{
				menuManager->MenuOpenCloseEventDispatcher()->AddEventSink(&g_activateTextMenuEventHandler);
				_MESSAGE("Waiting for WSActivateRollover menu to be created...");
			}
			else
			{
				_ERROR("MenuManager not available, cannot remove activate rollover text.");
			}
		}
	}
}
