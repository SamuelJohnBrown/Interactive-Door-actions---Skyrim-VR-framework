#include "config.h"
#include "SkyrimVRESLAPI.h"

#include <algorithm>
#include <sstream>

namespace InteractiveLockpickingVR {
		
	int logging = 0;
    int leftHandedMode = 0;
    int removeActivateText = 1;
    int doorLockpick = 1;
    int excludeLockedDoors = 0;
    int lockTierNoviceHoldMs = 3000;
    int lockTierApprenticeHoldMs = 4000;
    int lockTierAdeptHoldMs = 5000;
    int lockTierExpertHoldMs = 6500;
    int lockTierMasterHoldMs = 7500;
    int lockTierNoviceXp = 10;
    int lockTierApprenticeXp = 20;
    int lockTierAdeptXp = 35;
    int lockTierExpertXp = 60;
    int lockTierMasterXp = 100;
    float doorSessionStartDistance = 120.0f;
    float extendedSessionStartDistance = 165.0f;
    float extendedSessionEndDistance = 170.0f;
    float shortSessionStartDistance = 70.0f;
    float shortSessionEndDistance = 80.0f;
    float doorSessionEndDistance = 150.0f;
    float lockpickTouchDistance = 3.0f;
    float lockpickShivMaxDistance = 28.0f;
    int lockpickBreakRespawnDelayMs = 2000;
    int lockpickSessionStartDelayMs = 2000;
    int keyDoorGrabDelayMs = 1500;
    float keyDoorTurnDegrees = 20.0f;
    int lockpickBreakDuringHold = 0;
    int lockpickBreakRollIntervalMs = 1500;
    int lockpickBreakMinGapMs = 2000;
    float lockpickBreakSoundVolume = 2.0f;
    float unlockedDoorPushDistance = 4.0f;
    float unlockedDoorTouchDistance = 8.0f;
    int unlockedDoorPush = 1;
    int unlockedDoorSpawnDummy = 1;
    int keyDoorActions = 1;

	// Helgen Keep entrance/exit (Skyrim.esm) - hinge origin is far from the
	// visible gate; excluded so vanilla activation works normally.
	std::vector<ExcludedDoorRef> excludedDoorRefs = {
		{ "skyrim.esm", 0x0005DF01, 0 },  // Helgen Keep entrance
		{ "skyrim.esm", 0x000F8237, 0 },  // Helgen Keep exit
		{ "skyrim.esm", 0x0001C386, 0 },  // Solitude main gate entrance
		{ "skyrim.esm", 0x00037F1B, 0 },  // Solitude main gate exit
		{ "skyrim.esm", 0x00037603, 0 },  // Bleak Falls Barrow exit
		{ "skyrim.esm", 0x0002B83B, 0 },  // Bleak Falls Barrow entrance
		{ "skyrim.esm", 0x00025C00, 0 },
		{ "skyrim.esm", 0x000CD66F, 0 },  // Labyrinth double door
	};

	// Load-door base forms that keep vanilla activate (cave mouths, etc.).
	std::vector<ExcludedLoadDoorBase> excludedLoadDoorBases = {
		{ "skyrim.esm", 0x00016383, 0 },
		{ "skyrim.esm", 0x00016384, 0 },
		{ "skyrim.esm", 0x00031897, 0 },  // AutoLoadDoor01
		{ "skyrim.esm", 0x0002ED73, 0 },
		{ "skyrim.esm", 0x0002ED74, 0 },
		{ "skyrim.esm", 0x0002ED75, 0 },
		{ "skyrim.esm", 0x0002ED76, 0 },
	};

	// Interactive Doors mods - interior bases that stay vanilla.
	std::vector<ExcludedInteriorDoorBase> excludedInteriorDoorBases = {
		{ "interactive doors solitude.esp", 0x05005900, 0 },
		{ "interactive doors solitude.esp", 0x0501EE13, 0 },
		{ "interactive doors (physics based farmdoors).esp", 0x05005900, 0 },
	};

	// Base doors whose origin is far from the visible mesh — use a larger
	// session-start radius than DoorSessionStartDistance.
	std::vector<ExtendedSessionStartDoorBase> extendedSessionStartDoorBases = {
		{ "skyrim.esm", 0x00060D85, 0 },
	};

	std::vector<ShortSessionDoorRef> shortSessionDoorRefs = {};

	std::vector<ShortSessionStartDoorBase> shortSessionStartDoorBases = {};

	// Cage doors and similar — no witnessed lockpick crime/bounty (Helgen intro CTD).
	std::vector<ExcludeLockpickCrimeDoorBase> excludeLockpickCrimeDoorBases = {
		{ "skyrim.esm", 0x000AA043, 0 },  // Cage Door
	};

	static void AppendPluginFormEntry(const std::string& value, const char* settingName,
		std::string& outPlugin, UInt32& outLocalFormId)
	{
		std::string entry = value;
		trim(entry);
		if (entry.empty())
			return;

		size_t sep = entry.find('|');
		if (sep == std::string::npos)
			sep = entry.find(':');
		if (sep == std::string::npos)
		{
			_MESSAGE("InteractiveDoorActions.ini: invalid %s entry '%s' (expected Plugin.esp|FormIdHex)", settingName, entry.c_str());
			return;
		}

		outPlugin = entry.substr(0, sep);
		std::string formHex = entry.substr(sep + 1);
		trim(outPlugin);
		trim(formHex);
		std::transform(outPlugin.begin(), outPlugin.end(), outPlugin.begin(), ::tolower);

		if (outPlugin.empty() || formHex.empty())
		{
			_MESSAGE("InteractiveDoorActions.ini: invalid %s entry '%s'", settingName, value.c_str());
			return;
		}

		try
		{
			outLocalFormId = static_cast<UInt32>(std::stoul(formHex, nullptr, 16));
		}
		catch (...)
		{
			_MESSAGE("InteractiveDoorActions.ini: invalid form ID in %s entry '%s'", settingName, value.c_str());
			outLocalFormId = 0;
		}
	}

	static void AppendExcludedDoorRef(const std::string& value)
	{
		std::stringstream stream(value);
		std::string token;
		while (std::getline(stream, token, ','))
		{
			trim(token);
			if (token.empty())
				continue;

			ExcludedDoorRef entry;
			AppendPluginFormEntry(token, "ExcludeDoorRefs", entry.pluginName, entry.localFormId);
			if (entry.localFormId != 0)
				excludedDoorRefs.push_back(entry);
		}
	}

	static void AppendExcludedInteriorDoorBase(const std::string& value)
	{
		std::stringstream stream(value);
		std::string token;
		while (std::getline(stream, token, ','))
		{
			trim(token);
			if (token.empty())
				continue;

			ExcludedInteriorDoorBase entry;
			AppendPluginFormEntry(token, "ExcludeInteriorDoorBases", entry.pluginName, entry.localFormId);
			if (entry.localFormId != 0)
				excludedInteriorDoorBases.push_back(entry);
		}
	}

	static void AppendExcludedLoadDoorBase(const std::string& value)
	{
		std::stringstream stream(value);
		std::string token;
		while (std::getline(stream, token, ','))
		{
			trim(token);
			if (token.empty())
				continue;

			ExcludedLoadDoorBase entry;
			AppendPluginFormEntry(token, "ExcludeLoadDoorBases", entry.pluginName, entry.localFormId);
			if (entry.localFormId != 0)
				excludedLoadDoorBases.push_back(entry);
		}
	}

	static void AppendExtendedSessionStartDoorBase(const std::string& value)
	{
		std::string entryLine = value;
		trim(entryLine);
		if (entryLine.empty())
			return;

		ExtendedSessionStartDoorBase entry;
		AppendPluginFormEntry(entryLine, "ExtendedSessionStartDoorBases", entry.pluginName, entry.localFormId);
		if (entry.localFormId != 0)
			extendedSessionStartDoorBases.push_back(entry);
	}

	static void AppendShortSessionDoorRef(const std::string& value)
	{
		std::stringstream stream(value);
		std::string token;
		while (std::getline(stream, token, ','))
		{
			trim(token);
			if (token.empty())
				continue;

			ShortSessionDoorRef entry;
			AppendPluginFormEntry(token, "ShortSessionDoorRefs", entry.pluginName, entry.localFormId);
			if (entry.localFormId != 0)
				shortSessionDoorRefs.push_back(entry);
		}
	}

	static void AppendShortSessionStartDoorBase(const std::string& value)
	{
		std::string entryLine = value;
		trim(entryLine);
		if (entryLine.empty())
			return;

		ShortSessionStartDoorBase entry;
		AppendPluginFormEntry(entryLine, "ShortSessionStartDoorBases", entry.pluginName, entry.localFormId);
		if (entry.localFormId != 0)
			shortSessionStartDoorBases.push_back(entry);
	}

	static void AppendExcludeLockpickCrimeDoorBase(const std::string& value)
	{
		std::stringstream stream(value);
		std::string token;
		while (std::getline(stream, token, ','))
		{
			trim(token);
			if (token.empty())
				continue;

			ExcludeLockpickCrimeDoorBase entry;
			AppendPluginFormEntry(token, "ExcludeLockpickCrimeDoorBases", entry.pluginName, entry.localFormId);
			if (entry.localFormId != 0)
				excludeLockpickCrimeDoorBases.push_back(entry);
		}
	}

	bool UsesExtendedSessionStartDistance(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm || extendedSessionStartDoorBases.empty())
			return false;

		for (ExtendedSessionStartDoorBase& entry : extendedSessionStartDoorBases)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && ref->baseForm->formID == entry.resolvedFormId)
				return true;
		}

		return false;
	}

	bool IsShortSessionDoorRef(TESObjectREFR* ref)
	{
		if (!ref || shortSessionDoorRefs.empty())
			return false;

		for (ShortSessionDoorRef& entry : shortSessionDoorRefs)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && ref->formID == entry.resolvedFormId)
				return true;

			if (entry.localFormId != 0 && (ref->formID & 0x00FFFFFF) == entry.localFormId)
				return true;
		}

		return false;
	}

	bool UsesShortSessionStartDoorBase(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm || shortSessionStartDoorBases.empty())
			return false;

		const UInt32 baseFormId = ref->baseForm->formID;

		for (ShortSessionStartDoorBase& entry : shortSessionStartDoorBases)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && baseFormId == entry.resolvedFormId)
				return true;

			if (entry.localFormId != 0 && (baseFormId & 0x00FFFFFF) == entry.localFormId)
				return true;
		}

		return false;
	}

	bool UsesShortSessionDistance(TESObjectREFR* ref)
	{
		return IsShortSessionDoorRef(ref) || UsesShortSessionStartDoorBase(ref);
	}

	float GetDoorSessionStartDistance(TESObjectREFR* ref)
	{
		if (UsesExtendedSessionStartDistance(ref))
			return extendedSessionStartDistance;

		if (UsesShortSessionDistance(ref))
			return shortSessionStartDistance;

		return doorSessionStartDistance;
	}

	float GetDoorSessionEndDistance(TESObjectREFR* ref)
	{
		if (UsesExtendedSessionStartDistance(ref))
			return extendedSessionEndDistance;

		if (UsesShortSessionDistance(ref))
			return shortSessionEndDistance;

		return doorSessionEndDistance;
	}

	bool IsExcludedDoorRef(TESObjectREFR* ref)
	{
		if (!ref || excludedDoorRefs.empty())
			return false;

		for (ExcludedDoorRef& entry : excludedDoorRefs)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && ref->formID == entry.resolvedFormId)
				return true;
		}

		return false;
	}

	bool IsExcludedInteriorDoorBase(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm || excludedInteriorDoorBases.empty())
			return false;

		for (ExcludedInteriorDoorBase& entry : excludedInteriorDoorBases)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && ref->baseForm->formID == entry.resolvedFormId)
				return true;
		}

		return false;
	}

	bool IsExcludedLoadDoorBase(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm || excludedLoadDoorBases.empty())
			return false;

		const UInt32 baseFormId = ref->baseForm->formID;

		for (ExcludedLoadDoorBase& entry : excludedLoadDoorBases)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && baseFormId == entry.resolvedFormId)
				return true;

			// Skyrim.esm master records: also match bare local ID.
			if (entry.localFormId != 0 && (baseFormId & 0x00FFFFFF) == entry.localFormId)
				return true;
		}

		return false;
	}

	static bool NameContainsCageDoor(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm)
			return false;

		const char* name = ref->baseForm->GetName();
		if (!name || !name[0])
			return false;

		std::string lowered(name);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
		return lowered.find("cage door") != std::string::npos;
	}

	bool IsExcludedFromLockpickCrime(TESObjectREFR* ref)
	{
		if (!ref)
			return false;

		if (NameContainsCageDoor(ref))
			return true;

		if (!ref->baseForm || excludeLockpickCrimeDoorBases.empty())
			return false;

		for (ExcludeLockpickCrimeDoorBase& entry : excludeLockpickCrimeDoorBases)
		{
			if (entry.resolvedFormId == 0)
				entry.resolvedFormId = GetFullFormIdFromEspAndFormId(entry.pluginName.c_str(), entry.localFormId);

			if (entry.resolvedFormId != 0 && ref->baseForm->formID == entry.resolvedFormId)
				return true;
		}

		return false;
	}

    void loadConfig()
    {
        std::string runtimeDirectory = GetRuntimeDirectory();

        if (!runtimeDirectory.empty()) 
        {
            std::string filepath = runtimeDirectory + "Data\\SKSE\\Plugins\\" + std::string(MOD_FILE_NAME) + ".ini";
            std::ifstream file(filepath);

            if (!file.is_open()) 
            {
                transform(filepath.begin(), filepath.end(), filepath.begin(), ::tolower);
                file.open(filepath);
            }

            if (file.is_open()) 
            {
                std::string line;
                std::string currentSection;
                bool excludeDoorRefsFromIni = false;
                bool excludeInteriorDoorBasesFromIni = false;
                bool excludeLoadDoorBasesFromIni = false;
                bool excludeLockpickCrimeDoorBasesFromIni = false;
                bool extendedSessionStartDoorBasesFromIni = false;
                bool shortSessionStartDoorBasesFromIni = false;
                bool shortSessionDoorRefsFromIni = false;

                while (std::getline(file, line))
                {
                    trim(line);
                    skipComments(line);

                    if (line.empty()) continue;

                    if (line[0] == '[') 
                    {
                        // New section
                        size_t endBracket = line.find(']');
                        if (endBracket != std::string::npos) 
                        {
                            currentSection = line.substr(1, endBracket - 1);
                            trim(currentSection);

                            if (currentSection == "ExtendedSessionStartDoorBases"
                                && !extendedSessionStartDoorBasesFromIni)
                            {
                                extendedSessionStartDoorBases.clear();
                                extendedSessionStartDoorBasesFromIni = true;
                            }
                            else if (currentSection == "ShortSessionStartDoorBases"
                                && !shortSessionStartDoorBasesFromIni)
                            {
                                shortSessionStartDoorBases.clear();
                                shortSessionStartDoorBasesFromIni = true;
                            }
                        }
                    }
                    else if (currentSection == "ExtendedSessionStartDoorBases")
                    {
                        if (!line.empty() && (line[0] == ';' || line[0] == '#'))
                            continue;

                        std::string variableName;
                        std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);
                        if (variableName == "ExtendedSessionStartDistance")
                        {
                            extendedSessionStartDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "ExtendedSessionEndDistance")
                        {
                            extendedSessionEndDistance = std::stof(variableValueStr);
                        }
                        else if (line.find('=') != std::string::npos)
                        {
                            _MESSAGE("InteractiveDoorActions.ini: unknown key '%s' in [ExtendedSessionStartDoorBases] (expected ExtendedSessionStartDistance, ExtendedSessionEndDistance, or Plugin.esp|FormIdHex)", variableName.c_str());
                        }
                        else
                        {
                            AppendExtendedSessionStartDoorBase(line);
                        }
                    }
                    else if (currentSection == "ShortSessionStartDoorBases")
                    {
                        if (!line.empty() && (line[0] == ';' || line[0] == '#'))
                            continue;

                        std::string variableName;
                        std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);
                        if (variableName == "ShortSessionStartDistance")
                        {
                            shortSessionStartDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "ShortSessionEndDistance")
                        {
                            shortSessionEndDistance = std::stof(variableValueStr);
                        }
                        else if (line.find('=') != std::string::npos)
                        {
                            _MESSAGE("InteractiveDoorActions.ini: unknown key '%s' in [ShortSessionStartDoorBases] (expected ShortSessionStartDistance, ShortSessionEndDistance, or Plugin.esp|FormIdHex)", variableName.c_str());
                        }
                        else
                        {
                            AppendShortSessionStartDoorBase(line);
                        }
                    }
                    else if (currentSection == "Settings")
                    {
                        std::string variableName;
                        std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

                        if (variableName == "Logging") 
                        {
                            logging = std::stoi(variableValueStr);
                        }
                        else if (variableName == "RemoveActivateText") 
                        {
                            removeActivateText = std::stoi(variableValueStr);
                        }
                        else if (variableName == "DoorLockpick")
                        {
                            doorLockpick = std::stoi(variableValueStr);
                        }
                        else if (variableName == "ExcludeLockedDoors")
                        {
                            excludeLockedDoors = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierNoviceHoldMs")
                        {
                            lockTierNoviceHoldMs = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierApprenticeHoldMs")
                        {
                            lockTierApprenticeHoldMs = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierAdeptHoldMs")
                        {
                            lockTierAdeptHoldMs = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierExpertHoldMs")
                        {
                            lockTierExpertHoldMs = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierMasterHoldMs")
                        {
                            lockTierMasterHoldMs = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierNoviceXp")
                        {
                            lockTierNoviceXp = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierApprenticeXp")
                        {
                            lockTierApprenticeXp = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierAdeptXp")
                        {
                            lockTierAdeptXp = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierExpertXp")
                        {
                            lockTierExpertXp = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockTierMasterXp")
                        {
                            lockTierMasterXp = std::stoi(variableValueStr);
                        }
                        else if (variableName == "DoorSessionStartDistance")
                        {
                            doorSessionStartDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "ExtendedSessionStartDistance")
                        {
                            extendedSessionStartDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "ExtendedSessionEndDistance")
                        {
                            extendedSessionEndDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "ShortSessionStartDistance")
                        {
                            shortSessionStartDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "ShortSessionEndDistance")
                        {
                            shortSessionEndDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "DoorSessionEndDistance")
                        {
                            doorSessionEndDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "LockpickTouchDistance")
                        {
                            lockpickTouchDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "LockpickShivMaxDistance")
                        {
                            lockpickShivMaxDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "LockpickBreakRespawnDelayMs")
                        {
                            lockpickBreakRespawnDelayMs = std::stoi(variableValueStr);
                            if (lockpickBreakRespawnDelayMs < 0)
                                lockpickBreakRespawnDelayMs = 0;
                        }
                        else if (variableName == "LockpickSessionStartDelayMs")
                        {
                            lockpickSessionStartDelayMs = std::stoi(variableValueStr);
                            if (lockpickSessionStartDelayMs < 0)
                                lockpickSessionStartDelayMs = 0;
                        }
                        else if (variableName == "KeyDoorGrabDelayMs")
                        {
                            keyDoorGrabDelayMs = std::stoi(variableValueStr);
                            if (keyDoorGrabDelayMs < 0)
                                keyDoorGrabDelayMs = 0;
                        }
                        else if (variableName == "KeyDoorTurnDegrees")
                        {
                            keyDoorTurnDegrees = std::stof(variableValueStr);
                            if (keyDoorTurnDegrees < 5.0f)
                                keyDoorTurnDegrees = 5.0f;
                            if (keyDoorTurnDegrees > 180.0f)
                                keyDoorTurnDegrees = 180.0f;
                        }
                        else if (variableName == "LockpickBreakDuringHold")
                        {
                            lockpickBreakDuringHold = std::stoi(variableValueStr);
                        }
                        else if (variableName == "LockpickBreakRollIntervalMs")
                        {
                            lockpickBreakRollIntervalMs = std::stoi(variableValueStr);
                            if (lockpickBreakRollIntervalMs < 100)
                                lockpickBreakRollIntervalMs = 100;
                        }
                        else if (variableName == "LockpickBreakMinGapMs")
                        {
                            lockpickBreakMinGapMs = std::stoi(variableValueStr);
                            if (lockpickBreakMinGapMs < 0)
                                lockpickBreakMinGapMs = 0;
                        }
                        else if (variableName == "LockpickBreakSoundVolume")
                        {
                            lockpickBreakSoundVolume = std::stof(variableValueStr);
                            if (lockpickBreakSoundVolume < 0.1f)
                                lockpickBreakSoundVolume = 0.1f;
                            else if (lockpickBreakSoundVolume > 5.0f)
                                lockpickBreakSoundVolume = 5.0f;
                        }
                        else if (variableName == "UnlockedDoorPush")
                        {
                            unlockedDoorPush = std::stoi(variableValueStr);
                        }
                        else if (variableName == "UnlockedDoorPushDistance")
                        {
                            unlockedDoorPushDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "UnlockedDoorTouchDistance")
                        {
                            unlockedDoorTouchDistance = std::stof(variableValueStr);
                        }
                        else if (variableName == "UnlockedDoorSpawnDummy")
                        {
                            unlockedDoorSpawnDummy = std::stoi(variableValueStr);
                        }
                        else if (variableName == "KeyDoorActions")
                        {
                            keyDoorActions = std::stoi(variableValueStr);
                        }
                        else if (variableName == "ExcludeDoorRefs")
                        {
                            if (!excludeDoorRefsFromIni)
                            {
                                excludedDoorRefs.clear();
                                excludeDoorRefsFromIni = true;
                            }
                            AppendExcludedDoorRef(variableValueStr);
                        }
                        else if (variableName == "ShortSessionDoorRefs")
                        {
                            if (!shortSessionDoorRefsFromIni)
                            {
                                shortSessionDoorRefs.clear();
                                shortSessionDoorRefsFromIni = true;
                            }
                            AppendShortSessionDoorRef(variableValueStr);
                        }
                        else if (variableName == "ExcludeInteriorDoorBases")
                        {
                            if (!excludeInteriorDoorBasesFromIni)
                            {
                                excludedInteriorDoorBases.clear();
                                excludeInteriorDoorBasesFromIni = true;
                            }
                            AppendExcludedInteriorDoorBase(variableValueStr);
                        }
                        else if (variableName == "ExcludeLoadDoorBases")
                        {
                            if (!excludeLoadDoorBasesFromIni)
                            {
                                excludedLoadDoorBases.clear();
                                excludeLoadDoorBasesFromIni = true;
                            }
                            AppendExcludedLoadDoorBase(variableValueStr);
                        }
                        else if (variableName == "ExcludeLockpickCrimeDoorBases")
                        {
                            if (!excludeLockpickCrimeDoorBasesFromIni)
                            {
                                excludeLockpickCrimeDoorBases.clear();
                                excludeLockpickCrimeDoorBasesFromIni = true;
                            }
                            AppendExcludeLockpickCrimeDoorBase(variableValueStr);
                        }
                    }                    
                } 
            }
            _MESSAGE("Config file is loaded successfully.");
            return;
        }
        return;
    }

	void Log(const int msgLogLevel, const char* fmt, ...)
	{
		if (msgLogLevel > logging)
		{
			return;
		}

		va_list args;
		char logBuffer[4096];

		va_start(args, fmt);
		vsprintf_s(logBuffer, sizeof(logBuffer), fmt, args);
		va_end(args);

		_MESSAGE(logBuffer);
	}

}
