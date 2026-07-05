#pragma once

#include "Helper.h"
#include "skse64/PluginAPI.h"

namespace InteractiveLockpickingVR
{
	extern SKSETrampolineInterface* g_trampolineInterface;
	extern HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	extern vrikPluginApi::IVrikInterface001* vrikInterface;
	extern SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;

	void StartMod();
	void RegisterActivateTextSinks(SKSEMessagingInterface* messaging);

}