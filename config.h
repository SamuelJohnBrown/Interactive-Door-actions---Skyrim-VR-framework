#pragma once
#include <iostream>
#include <string>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <skse64/NiProperties.h>
#include <skse64/NiNodes.h>

#include "skse64\GameSettings.h"
#include "Utility.hpp"

#include <skse64/GameData.h>

#include "higgsinterface001.h"
#include "vrikinterface001.h"
#include "SkyrimVRESLAPI.h"

namespace InteractiveLockpickingVR {

	constexpr const char* MOD_PLUGIN_NAME = "Interactive Door actions";
	constexpr const char* MOD_FILE_NAME = "InteractiveDoorActions";
	constexpr const char* MOD_ESP_NAME = "InteractiveDoorActions.esp";

	const UInt32 MOD_VERSION = 0x10000;
	const std::string MOD_VERSION_STR = "1.0.0";
	extern int leftHandedMode;

	extern int logging;
	extern int removeActivateText;
	extern int doorLockpick;
	extern int excludeLockedDoors;
	extern int lockTierNoviceHoldMs;
	extern int lockTierApprenticeHoldMs;
	extern int lockTierAdeptHoldMs;
	extern int lockTierExpertHoldMs;
	extern int lockTierMasterHoldMs;
	extern int lockTierNoviceXp;
	extern int lockTierApprenticeXp;
	extern int lockTierAdeptXp;
	extern int lockTierExpertXp;
	extern int lockTierMasterXp;
	extern float doorSessionStartDistance;
	extern float extendedSessionStartDistance;
	extern float extendedSessionEndDistance;
	extern float doorSessionEndDistance;
	extern float lockpickTouchDistance;
	extern float lockpickShivMaxDistance;
	extern int lockpickBreakRespawnDelayMs;
	extern int lockpickSessionStartDelayMs;
	extern int keyDoorGrabDelayMs;
	extern int lockpickBreakDuringHold;
	extern int lockpickBreakRollIntervalMs;
	extern float lockpickBreakSoundVolume;
	extern float unlockedDoorPushDistance;
	extern float unlockedDoorTouchDistance;
	extern int unlockedDoorPush;
	extern int unlockedDoorSpawnDummy;
	extern int keyDoorActions;

	// Door refs excluded from physical door interaction (100% vanilla).
	// Each entry is a plugin name + local ref FormID hex from xEdit.
	struct ExcludedDoorRef
	{
		std::string pluginName;
		UInt32 localFormId = 0;
		mutable UInt32 resolvedFormId = 0;
	};
	extern std::vector<ExcludedDoorRef> excludedDoorRefs;

	bool IsExcludedDoorRef(TESObjectREFR* ref);

	// Placed load-door refs (ExtraTeleport) that use back-face pull-to-open.
	// All other load doors always use push, regardless of which side you stand on.
	struct PullLoadDoorRef
	{
		std::string pluginName;
		UInt32 localFormId = 0;
		mutable UInt32 resolvedFormId = 0;
	};
	extern std::vector<PullLoadDoorRef> pullLoadDoorRefs;

	bool IsPullLoadDoorRef(TESObjectREFR* ref);

	// Base DOOR forms (not refs) excluded from interior non-load push/dummy
	// logic. All other interior non-load doors use physical interaction.
	struct ExcludedInteriorDoorBase
	{
		std::string pluginName;
		UInt32 localFormId = 0;
		mutable UInt32 resolvedFormId = 0;
	};
	extern std::vector<ExcludedInteriorDoorBase> excludedInteriorDoorBases;

	bool IsExcludedInteriorDoorBase(TESObjectREFR* ref);

	// Base DOOR forms (and optional refs) that skip witnessed lockpick crime/bounty.
	struct ExcludeLockpickCrimeDoorBase
	{
		std::string pluginName;
		UInt32 localFormId = 0;
		mutable UInt32 resolvedFormId = 0;
	};
	extern std::vector<ExcludeLockpickCrimeDoorBase> excludeLockpickCrimeDoorBases;

	bool IsExcludedFromLockpickCrime(TESObjectREFR* ref);

	// Base DOOR forms that use ExtendedSessionStartDistance / ExtendedSessionEndDistance
	// instead of the global DoorSessionStartDistance / DoorSessionEndDistance.
	struct ExtendedSessionStartDoorBase
	{
		std::string pluginName;
		UInt32 localFormId = 0;
		mutable UInt32 resolvedFormId = 0;
	};
	extern std::vector<ExtendedSessionStartDoorBase> extendedSessionStartDoorBases;

	float GetDoorSessionStartDistance(TESObjectREFR* ref);
	float GetDoorSessionEndDistance(TESObjectREFR* ref);

	void loadConfig();
	
	void Log(const int msgLogLevel, const char* fmt, ...);
	enum eLogLevels
	{
		LOGLEVEL_ERR = 0,
		LOGLEVEL_WARN,
		LOGLEVEL_INFO,
	};


#define LOG(fmt, ...) Log(LOGLEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) Log(LOGLEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) Log(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)


}