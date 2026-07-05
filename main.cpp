#include "skse64_common/skse_version.h"
#include <shlobj.h>
#include <intrin.h>
#include <string>
#include <xbyak/xbyak.h>

#include "skse64/PluginAPI.h"	
#include "config.h"
#include "Engine.h"
#include "DoorMechanics.h"

#include "skse64_common/BranchTrampoline.h"

namespace InteractiveLockpickingVR
{
	static SKSEMessagingInterface* g_messaging = NULL;
	static PluginHandle					g_pluginHandle = kPluginHandle_Invalid;
	static SKSESerializationInterface* g_serialization = NULL;
	static SKSEPapyrusInterface* g_papyrus = NULL;
	static SKSEObjectInterface* g_object = NULL;
	SKSETaskInterface* g_task = NULL;

	static SKSEVRInterface* g_vrInterface = nullptr;

	#pragma comment(lib, "Ws2_32.lib")

	void SetupReceptors()
	{
		_MESSAGE("Building Event Sinks...");

		
	}

	extern "C" {

		bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info) {	// Called by SKSE to learn about this plugin and check that it's safe to load it
			{
				const std::string logPath = std::string("\\My Games\\Skyrim VR\\SKSE\\") + MOD_FILE_NAME + ".log";
				gLog.OpenRelative(CSIDL_MYDOCUMENTS, logPath.c_str());
			}
			gLog.SetPrintLevel(IDebugLog::kLevel_Error);
			gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);
			//gLog.SetLogLevel(IDebugLog::kLevel_FatalError);

			std::string logMsg(MOD_PLUGIN_NAME);
			logMsg.append(": ");
			logMsg.append(MOD_VERSION_STR);
			_MESSAGE(logMsg.c_str());

			// populate info structure
			info->infoVersion = PluginInfo::kInfoVersion;
			info->name = MOD_PLUGIN_NAME;
			info->version = MOD_VERSION;

			// store plugin handle so we can identify ourselves later
			g_pluginHandle = skse->GetPluginHandle();

			std::string skseVers = "SKSE Version: ";
			skseVers += std::to_string(skse->runtimeVersion);
			_MESSAGE(skseVers.c_str());

			if (skse->isEditor)
			{
				_MESSAGE("loaded in editor, marking as incompatible");

				return false;
			}
			else if (skse->runtimeVersion < CURRENT_RELEASE_RUNTIME)
			{
				_MESSAGE("unsupported runtime version %08X", skse->runtimeVersion);

				return false;
			}

			// ### do not do anything else in this callback
			// ### only fill out PluginInfo and return true/false

			// supported runtime version
			return true;
		}

		inline bool file_exists(const std::string& name) {
			struct stat buffer;
			return (stat(name.c_str(), &buffer) == 0);
		}

		static const size_t TRAMPOLINE_SIZE = 256;

		//Listener for SKSE Messages
		void OnSKSEMessage(SKSEMessagingInterface::Message* msg)
		{
			if (msg)
			{
				if (msg->type == SKSEMessagingInterface::kMessage_PostLoad)
				{

				}
				else if (msg->type == SKSEMessagingInterface::kMessage_InputLoaded)
					SetupReceptors();
				else if (msg->type == SKSEMessagingInterface::kMessage_DataLoaded)
				{
					InteractiveLockpickingVR::loadConfig();

					// NEW SKSEVR feature: trampoline interface object from QueryInterface() - Use SKSE existing process code memory pool - allow Skyrim to run without ASLR
					if (InteractiveLockpickingVR::g_trampolineInterface)
					{
						void* branch = InteractiveLockpickingVR::g_trampolineInterface->AllocateFromBranchPool(g_pluginHandle, TRAMPOLINE_SIZE);
						if (!branch) {
							_ERROR("couldn't acquire branch trampoline from SKSE. this is fatal. skipping remainder of init process.");
							return;
						}

						g_branchTrampoline.SetBase(TRAMPOLINE_SIZE, branch);

						void* local = InteractiveLockpickingVR::g_trampolineInterface->AllocateFromLocalPool(g_pluginHandle, TRAMPOLINE_SIZE);
						if (!local) {
							_ERROR("couldn't acquire codegen buffer from SKSE. this is fatal. skipping remainder of init process.");
							return;
						}

						g_localTrampoline.SetBase(TRAMPOLINE_SIZE, local);

						_MESSAGE("Using new SKSEVR trampoline interface memory pool alloc for codegen buffers.");
					}
					else  // otherwise if using an older SKSEVR version, fall back to old code
					{

						if (!g_branchTrampoline.Create(TRAMPOLINE_SIZE))  // don't need such large buffers
						{
							_FATALERROR("[ERROR] couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
							return;
						}

						if (!g_localTrampoline.Create(TRAMPOLINE_SIZE, nullptr))
						{
							_FATALERROR("[ERROR] couldn't create codegen buffer. this is fatal. skipping remainder of init process.");
							return;
						}

						_MESSAGE("Using legacy SKSE trampoline creation.");
					}

					InteractiveLockpickingVR::GameLoad();
					InteractiveLockpickingVR::RegisterActivateTextSinks(g_messaging);
					InteractiveLockpickingVR::StartMod();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostPostLoad)
				{
					higgsInterface = HiggsPluginAPI::GetHiggsInterface001(g_pluginHandle, g_messaging);
					if (higgsInterface)
					{
						_MESSAGE("Got HIGGS interface. Buildnumber: %d", higgsInterface->GetBuildNumber());
					}
					else
					{
						_MESSAGE("Did not get HIGGS interface");
					}

					vrikInterface = vrikPluginApi::getVrikInterface001(g_pluginHandle, g_messaging);
					if (vrikInterface)
					{
						unsigned int vrikBuildNumber = vrikInterface->getBuildNumber();
						if (vrikBuildNumber < 80400)
						{
							ShowErrorBoxAndTerminate("[CRITICAL] VRIK's older versions are not compatible with Immersive Crossbow Reload VR. Make sure you have VRIK version 0.8.4 or higher, preferably the latest.");
						}
						_MESSAGE("Got VRIK interface. Buildnumber: %d", vrikBuildNumber);

					}
					else
					{
						_MESSAGE("Did not get VRIK interface");
					}

					skyrimVRESLInterface = SkyrimVRESLPluginAPI::GetSkyrimVRESLInterface001(g_pluginHandle, g_messaging);
					if (skyrimVRESLInterface)
					{
						_MESSAGE("Got SkyrimVRESL interface");
					}
					else
					{
						_MESSAGE("Did not get SkyrimVRESL interface");
					}

				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PreLoadGame)
				{
					InteractiveLockpickingVR::OnDoorMechanicsPreLoadGame();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostLoadGame)
				{
					if ((bool)(msg->data) == true)
					{
						InteractiveLockpickingVR::PostLoadGame();
						InteractiveLockpickingVR::OnDoorMechanicsPostLoadGame();
					}
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_NewGame)
				{
					InteractiveLockpickingVR::OnDoorMechanicsPreLoadGame();
					InteractiveLockpickingVR::OnDoorMechanicsPostLoadGame();
				}
			}
		}

		bool SKSEPlugin_Load(const SKSEInterface* skse) {	// Called by SKSE to load this plugin

			g_task = (SKSETaskInterface*)skse->QueryInterface(kInterface_Task);

			g_papyrus = (SKSEPapyrusInterface*)skse->QueryInterface(kInterface_Papyrus);

			g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

			g_vrInterface = (SKSEVRInterface*)skse->QueryInterface(kInterface_VR);
			if (!g_vrInterface) {
				_MESSAGE("[CRITICAL] Couldn't get SKSE VR interface. You probably have an outdated SKSE version.");
				return false;
			}

			g_serialization = (SKSESerializationInterface*)skse->QueryInterface(kInterface_Serialization);
			InteractiveLockpickingVR::RegisterDoorMechanicsSerialization(g_serialization, g_pluginHandle);

			return true;
		}
	};
}