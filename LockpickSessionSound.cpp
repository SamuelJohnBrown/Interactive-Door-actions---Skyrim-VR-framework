#include "LockpickSessionSound.h"

#include "SkyrimVRESLAPI.h"
#include "config.h"

#include "skse64/GameForms.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/NiNodes.h"

namespace InteractiveLockpickingVR
{
	namespace
	{
		static constexpr UInt32 kLockpickSndrLocalFormId = 0x00000801;
		static constexpr UInt32 kLockpickSndrFallbackFormId = 0xFE000801;
		static constexpr UInt32 kUnlockSuccessSndrLocalFormId = 0x00000802;
		static constexpr UInt32 kUnlockSuccessSndrFallbackFormId = 0xFE000802;
		static constexpr UInt32 kLockpickBreakSndrFormId = 0x000C1916;

		typedef void* (*GetAudioManagerSingletonFn)();
		typedef bool (*BuildSoundDataFromDescriptorFn)(void* manager, BSSoundHandle* handle, void* soundDescriptor, UInt32 flags);
		typedef void (*SoundHandleSetObjectToFollowFn)(BSSoundHandle* handle, NiAVObject* node);
		typedef bool (*SoundHandleSetVolumeFn)(BSSoundHandle* handle, float volume);

		static RelocAddr<GetAudioManagerSingletonFn> GetAudioManagerSingleton(0x00C29430);
		static RelocAddr<BuildSoundDataFromDescriptorFn> BuildSoundDataFromDescriptor(0x00C29F60);
		static RelocAddr<SoundHandleSetObjectToFollowFn> SoundHandleSetObjectToFollow(0x00C289C0);
		static RelocAddr<SoundHandleSetVolumeFn> SoundHandleSetVolume(0x00C28660);

		BGSSoundDescriptorForm* g_lockpickLoopSound = nullptr;
		UInt32 g_lockpickLoopSoundFormId = 0;
		BGSSoundDescriptorForm* g_unlockSuccessSound = nullptr;
		UInt32 g_unlockSuccessSoundFormId = 0;
		BGSSoundDescriptorForm* g_lockpickBreakSound = nullptr;
		UInt32 g_lockpickBreakSoundFormId = 0;
		BSSoundHandle g_activeLockpickSound{};
		bool g_lockpickSoundPlaying = false;

		BGSSoundDescriptorForm* ResolveSoundDescriptor(UInt32 formId)
		{
			if (formId == 0)
				return nullptr;

			TESForm* form = LookupFormByID(formId);
			if (!form)
				return nullptr;

			return DYNAMIC_CAST(form, TESForm, BGSSoundDescriptorForm);
		}

		void ResetActiveHandle()
		{
			g_activeLockpickSound = {};
			g_lockpickSoundPlaying = false;
		}

		BGSSoundDescriptorForm* LoadSoundFromEsp(UInt32 localFormId, UInt32 fallbackFormId, UInt32& outFormId)
		{
			outFormId = 0;

			const UInt32 resolvedFormId =
				GetFullFormIdFromEspAndFormId(MOD_ESP_NAME, localFormId);
			if (resolvedFormId != 0)
			{
				BGSSoundDescriptorForm* sndr = ResolveSoundDescriptor(resolvedFormId);
				if (sndr)
				{
					outFormId = resolvedFormId;
					return sndr;
				}
			}

			BGSSoundDescriptorForm* sndr = ResolveSoundDescriptor(fallbackFormId);
			if (sndr)
				outFormId = fallbackFormId;

			return sndr;
		}

		void PlaySoundOnDoor(BGSSoundDescriptorForm* sndr, UInt32 sndrFormId, TESObjectREFR* doorRef, const char* label, float volume = 1.0f)
		{
			if (!sndr || !doorRef)
				return;

			void* audioManager = GetAudioManagerSingleton();
			if (!audioManager)
			{
				LOG_ERR("[Sound] BSAudioManager unavailable for %s SNDR", label);
				return;
			}

			void* soundDescriptor = &sndr->soundDescriptor;
			BSSoundHandle handle{};
			if (!BuildSoundDataFromDescriptor(audioManager, &handle, soundDescriptor, 0x10))
			{
				LOG_ERR("[Sound] BuildSoundDataFromDescriptor failed for %s SNDR", label);
				return;
			}

			if (NiNode* node = doorRef->GetNiNode())
				SoundHandleSetObjectToFollow(&handle, node);

			if (volume != 1.0f && SoundHandleSetVolume)
				SoundHandleSetVolume(&handle, volume);

			if (CALL_MEMBER_FN(&handle, Play)())
				LOG_INFO("[Sound] Playing %s SNDR (formId=%08X, volume=%.2f) on door %08X", label, sndrFormId, volume, doorRef->formID);
			else
				LOG_ERR("[Sound] Failed to play %s SNDR", label);
		}

	}  // namespace

	void InitLockpickSessionSound()
	{
		g_lockpickLoopSound = nullptr;
		g_lockpickLoopSoundFormId = 0;
		g_unlockSuccessSound = nullptr;
		g_unlockSuccessSoundFormId = 0;
		g_lockpickBreakSound = nullptr;
		g_lockpickBreakSoundFormId = 0;
		ResetActiveHandle();

		g_lockpickLoopSound = LoadSoundFromEsp(
			kLockpickSndrLocalFormId, kLockpickSndrFallbackFormId, g_lockpickLoopSoundFormId);
		if (g_lockpickLoopSound)
			LOG_INFO("[Sound] Loaded lockpick loop SNDR formId=%08X", g_lockpickLoopSoundFormId);
		else
			LOG_ERR(
				"[Sound] Failed to load lockpick loop SNDR from %s (base=%08X fallback=%08X)",
				MOD_ESP_NAME,
				kLockpickSndrLocalFormId,
				kLockpickSndrFallbackFormId);

		g_unlockSuccessSound = LoadSoundFromEsp(
			kUnlockSuccessSndrLocalFormId, kUnlockSuccessSndrFallbackFormId, g_unlockSuccessSoundFormId);
		if (g_unlockSuccessSound)
			LOG_INFO("[Sound] Loaded unlock success SNDR formId=%08X", g_unlockSuccessSoundFormId);
		else
			LOG_ERR(
				"[Sound] Failed to load unlock success SNDR from %s (base=%08X fallback=%08X)",
				MOD_ESP_NAME,
				kUnlockSuccessSndrLocalFormId,
				kUnlockSuccessSndrFallbackFormId);

		g_lockpickBreakSound = ResolveSoundDescriptor(kLockpickBreakSndrFormId);
		if (g_lockpickBreakSound)
		{
			g_lockpickBreakSoundFormId = kLockpickBreakSndrFormId;
			LOG_INFO("[Sound] Loaded lockpick break SNDR formId=%08X", g_lockpickBreakSoundFormId);
		}
		else
		{
			LOG_ERR("[Sound] Failed to load lockpick break SNDR formId=%08X", kLockpickBreakSndrFormId);
		}
	}

	void StopLockpickSessionSound()
	{
		if (!g_lockpickSoundPlaying)
			return;

		if (g_activeLockpickSound.soundID != BSSoundHandle::kInvalidID)
			CALL_MEMBER_FN(&g_activeLockpickSound, Stop)();

		ResetActiveHandle();
	}

	void StartLockpickSessionSound(TESObjectREFR* doorRef)
	{
		if (g_lockpickSoundPlaying || !g_lockpickLoopSound || !doorRef)
			return;

		void* audioManager = GetAudioManagerSingleton();
		if (!audioManager)
		{
			LOG_ERR("[Sound] BSAudioManager unavailable for lockpick loop SNDR");
			return;
		}

		void* soundDescriptor = &g_lockpickLoopSound->soundDescriptor;
		BSSoundHandle handle{};
		if (!BuildSoundDataFromDescriptor(audioManager, &handle, soundDescriptor, 0x10))
		{
			LOG_ERR("[Sound] BuildSoundDataFromDescriptor failed for lockpick loop SNDR");
			return;
		}

		if (NiNode* node = doorRef->GetNiNode())
			SoundHandleSetObjectToFollow(&handle, node);

		if (CALL_MEMBER_FN(&handle, Play)())
		{
			g_activeLockpickSound = handle;
			g_lockpickSoundPlaying = true;
			LOG_INFO("[Sound] Playing lockpick loop SNDR on door %08X", doorRef->formID);
		}
		else
		{
			LOG_ERR("[Sound] Failed to play lockpick loop SNDR");
		}
	}

	void PlayUnlockSuccessSound(TESObjectREFR* doorRef)
	{
		PlaySoundOnDoor(g_unlockSuccessSound, g_unlockSuccessSoundFormId, doorRef, "unlock success");
	}

	void PlayLockpickBreakSound(TESObjectREFR* doorRef)
	{
		PlaySoundOnDoor(g_lockpickBreakSound, g_lockpickBreakSoundFormId, doorRef, "lockpick break", lockpickBreakSoundVolume);
	}

}  // namespace InteractiveLockpickingVR
