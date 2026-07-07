#include "DoorMechanics.h"

#include "config.h"
#include "CrimeBounty.h"
#include "LockpickBreak.h"
#include "LockpickNotify.h"
#include "LockpickSkillXp.h"
#include "LockpickSessionSound.h"
#include "Engine.h"
#include "Helper.h"

#include "skse64/PluginAPI.h"

#include "skse64/GameData.h"
#include "skse64/GameExtraData.h"
#include "skse64/GameForms.h"
#include "skse64/GameObjects.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameVR.h"
#include "skse64/NiNodes.h"
#include "skse64/NiObjects.h"
#include "skse64/NiRTTI.h"
#include "skse64/NiTypes.h"
#include "skse64/PapyrusVM.h"
#include "skse64_common/BranchTrampoline.h"
#include "skse64_common/Relocation.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace InteractiveLockpickingVR
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		// Skyrim.esm lockpick (0000000A).
		constexpr UInt32 kLockpickFormId = 0x0000000A;
		// Skyrim.esm shiv (000426C8) - held in the off hand during lockpick sessions.
		constexpr UInt32 kShivFormId = 0x000426C8;
		// Skyrim.esm door bases excluded from unlocked push-to-open only (activate
		// text stays visible; lockpicking unchanged).
		constexpr UInt32 kExcludedDoorPushBaseFormIds[] = {
			0x00064090,
			0x0002E4C3,
			0x0006597B,
		};

		// Skyrim.esm door bases fully excluded (100% vanilla: no push, no lockpick).
		constexpr UInt32 kFullyExcludedDoorBaseFormIds[] = {
			0x00097BE6,
		};

		// DOOR base forms used by wilderness cave mouths (NorDoor* etc.). Matched on
		// the lower 24 bits so load-order prefixes do not matter.
		constexpr UInt32 kCaveLoadDoorBaseFormIds[] = {
			0x00016383,
			0x00016384,
			0x00031897,  // AutoLoadDoor01 — standard wilderness cave autoload
			0x0002ED73,  // NorDoor01
			0x0002ED74,  // NorDoor02
			0x0002ED75,  // NorDoor03
			0x0002ED76,  // NorDoor04
		};

		// TESObjectDOOR FNAM flags (see UESP DOOR record).
		constexpr UInt8 kDoorFlagAutomatic = 0x02;
		constexpr UInt8 kDoorFlagSliding = 0x10;

		// AutoLoadDoor01 places the REF origin at the teleport point, not the
		// visible cave mouth — normal DoorSessionStartDistance never matches.
		constexpr float kAutoLoadDoorApproachDistance = 400.0f;

		// Invisible dummy MISC item spawned for UNLOCKED doors so the hand
		// gets collision with the door without a visible lockpick. Lives in
		// InteractiveDoorActions.esp (ESL-flagged) as record
		// xx000800; the runtime FormID depends on load order, so it is resolved
		// by plugin name on first use.
		constexpr UInt32 kDummyLocalFormId = 0x00000800;
		UInt32 s_resolvedDummyFormId = 0;

		// Lock data on a reference (layout from CommonLibSSE/VR REFR_LOCK).
		// GetLockLevel() only reports the lock's base DIFFICULTY, which never
		// changes when the lock is picked - the actual locked/unlocked state
		// lives in the kLocked flag; clearing it unlocks the ref (same as
		// CommonLib REFR_LOCK::SetLocked(false) / TESObjectREFR::IsLocked()).
		struct REFR_LOCK
		{
			enum : UInt8
			{
				kFlag_Locked = 1 << 0,
				kFlag_Leveled = 1 << 2
			};

			SInt8 baseLevel;	// 00
			UInt8 pad01;		// 01
			UInt16 pad02;		// 02
			UInt32 pad04;		// 04
			void* key;			// 08 (TESKey*)
			UInt8 flags;		// 10
			UInt8 pad11;		// 11
			UInt16 pad12;		// 12
			UInt32 numTries;	// 14
			UInt32 unk18;		// 18
			UInt32 pad1C;		// 1C
		};

		enum LockLevel : SInt32
		{
			kUnlocked = -1,
			kVeryEasy = 0,
			kEasy = 1,
			kAverage = 2,
			kHard = 3,
			kVeryHard = 4,
			kRequiresKey = 5
		};

		// CommonLibVR VR offsets for Skyrim VR 1.4.15
		typedef REFR_LOCK* (*_TESObjectREFR_GetLock)(const TESObjectREFR* refr);
		typedef SInt32 (*_REFR_LOCK_GetLockLevel)(const REFR_LOCK* lock, const TESObjectREFR* refr);
		static RelocAddr<_TESObjectREFR_GetLock> TESObjectREFR_GetLock(0x002B8C30);
		static RelocAddr<_REFR_LOCK_GetLockLevel> REFR_LOCK_GetLockLevel(0x00145380);

		typedef bool (*_TESObjectREFR_Activate)(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly);
		static RelocAddr<_TESObjectREFR_Activate> Activate_Native(0x002A8300);

		// BGSOpenCloseForm::GetOpenState - CommonLibVR VR 1.4.15 offset.
		enum DoorOpenState : UInt32
		{
			kDoorOpenStateNone = 0,
			kDoorOpenStateOpen = 1,
			kDoorOpenStateOpening = 2,
			kDoorOpenStateClosed = 3,
			kDoorOpenStateClosing = 4
		};

		typedef UInt32 (*_BGSOpenCloseForm_GetOpenState)(const TESObjectREFR* refr);
		static RelocAddr<_BGSOpenCloseForm_GetOpenState> BGSOpenCloseForm_GetOpenState(0x00199480);

		// TESObjectREFR::Activate detour (Fake Edge VR pattern). Blocks vanilla
		// player activation of the held session item and of load doors; our own
		// code activates through the bypass flag.
		_TESObjectREFR_Activate OriginalActivate = nullptr;
		bool s_bypassActivate = false;

		typedef void (*_RemoveItem_Native)(VMClassRegistry* registry, UInt32 stackId, TESObjectREFR* akSource, TESForm* akItemToRemove, SInt32 aiCount, bool abSilent, TESObjectREFR* akOtherContainer);
		static RelocAddr<_RemoveItem_Native> RemoveItem_Native(0x009D1190);

		// Papyrus ObjectReference.Delete - removes a spawned ref from the world.
		// Same address and calling pattern as Fake Edge VR's DeleteWorldObject.
		typedef void (*_DeleteObject_Native)(VMClassRegistry* registry, UInt32 stackId, TESObjectREFR* obj);
		static RelocAddr<_DeleteObject_Native> DeleteObject_Native(0x009CE380);

		// Read by the monitor heartbeat thread, written on the game thread.
		std::atomic<bool> s_sessionActive{ false };
		UInt32 s_targetDoorFormId = 0;
		UInt32 s_lastDoorDebugFormId = 0;
		// Key-required door under crosshair (any door ref, including vanilla-only).
		UInt32 s_crosshairKeyDoorFormId = 0;
		// Last key door ref we logged while in front; cleared when player leaves.
		UInt32 s_keyDoorLoggedFormId = 0;
		// Last door the crosshair landed on (by FormID). Kept when a VR hand
		// ray goes empty so approaching an already-targeted door still works.
		UInt32 s_crosshairDoorFormId = 0;
		// Cave / excluded load door under crosshair (diagnostic logging).
		UInt32 s_crosshairCaveDoorFormId = 0;
		UInt32 s_caveEntranceLoggedFormId = 0;
		TESObjectREFR* s_spawnedLockpick = nullptr;
		TESObjectREFR* s_spawnedShiv = nullptr;
		// True when the current session holds the invisible dummy item
		// (unlocked door) instead of a real lockpick (locked door).
		bool s_sessionIsDummy = false;
		// Unlocked-door push without spawning the dummy: contact/push uses the
		// main hand bone position instead of a grabbed invisible item.
		bool s_sessionIsHandPush = false;
		// Game hand (left/right) driving a hand-push session; either hand can push.
		bool s_sessionPushHandIsLeft = false;
		bool s_sessionPushHandActive = false;
		// Locked-door session: lockpick in main hand, shiv in off hand.
		bool s_sessionIsLockpick = false;
		// Key-required door: spawned key in main hand only.
		bool s_sessionIsKeyDoor = false;
		bool s_keyDoorConsumedOnUnlock = false;
		bool s_keyDoorHandAtDoor = false;
		std::chrono::steady_clock::time_point s_keyDoorGrabReadyTime{};
		bool s_keyDoorTurnBaselineValid = false;
		NiPoint3 s_keyDoorTwistAxis{};
		NiPoint3 s_keyDoorTwistRefBaseline{};
		int s_keyDoorTwistRefColumn = 1;
		float s_keyDoorTurnPeakDeg = 0.0f;
		UInt32 s_cachedHandFormId = 0;
		bool s_cachedHandIsSpell = false;
		bool s_mainHandIsLeft = false;
		UInt32 s_cachedOffHandFormId = 0;
		bool s_cachedOffHandIsSpell = false;
		bool s_offHandIsLeft = false;

		// While the lockpick is grabbed it blocks the hand's crosshair ray, so
		// crosshair events can no longer be trusted to say "still at the door".
		// A periodic monitor check instead ends the session when the player
		// physically moves away from the door.
		//
		// IMPORTANT: this SKSE build's BSTaskPool::ProcessTasks drains the task
		// queue until EMPTY in one frame, so a task that re-queues itself spins
		// the game thread forever (hard freeze). The monitor is therefore driven
		// by a background thread that posts a SINGLE-SHOT check task every tick.
		bool s_monitorThreadStarted = false;
		constexpr int kMonitorIntervalMs = 100;

		// "No longer in front of the door" for dummy sessions: farther than
		// DoorSessionEndDistance, or facing away from the door by more than
		// ~90 degrees. Within kDummyNearDistance the facing test is skipped:
		// at point-blank range the direction to the door's origin (its hinge)
		// is unreliable.
		constexpr float kDummyNearDistance = 150.0f;
		constexpr float kDummyFacingDotMin = 0.0f;

		// Touch hysteresis: entering contact uses lockpickTouchDistance, but
		// once touching, contact only breaks beyond that + this slack. Without
		// it, hand jitter right at the threshold keeps resetting the hold timer.
		constexpr float kTouchExitSlack = 4.0f;

		// Short time-based cooldown so a just-ended session doesn't instantly
		// respawn a lockpick at the same door.
		std::chrono::steady_clock::time_point s_lastSessionEndTime{};
		constexpr int kRestartCooldownMs = 1500;

		// After a load-door activation, block new door sessions until the cell
		// transition finishes. Without this the monitor thread can start a fresh
		// hand-push session during the fade and stall the teleport (black screen,
		// old-cell audio).
		std::chrono::steady_clock::time_point s_loadDoorTransitionCooldownUntil{};
		constexpr int kLoadDoorTransitionCooldownMs = 5000;
		constexpr int kLoadDoorActivateDelayMs = 350;

		// While a load-door push is activating, skip re-equipping the cached weapon.
		// Doing that during the fade stalls the cell transition (black screen).
		bool s_suppressHandRestore = false;
		UInt32 s_deferredHandRestoreFormId = 0;
		bool s_deferredHandRestoreIsSpell = false;
		bool s_deferredHandRestoreIsLeft = false;

		bool IsLoadDoorTransitionCooldownActive()
		{
			return s_loadDoorTransitionCooldownUntil != std::chrono::steady_clock::time_point{}
				&& std::chrono::steady_clock::now() < s_loadDoorTransitionCooldownUntil;
		}

		void ArmLoadDoorTransitionCooldown()
		{
			s_loadDoorTransitionCooldownUntil = std::chrono::steady_clock::now()
				+ std::chrono::milliseconds(kLoadDoorTransitionCooldownMs);
			s_crosshairDoorFormId = 0;
			s_crosshairKeyDoorFormId = 0;
			LOG_INFO("Door lockpick: load-door transition cooldown armed (%d ms)",
				kLoadDoorTransitionCooldownMs);
		}

		// Touch-to-unlock: lockpick + shiv both at the door, close together,
		// for a duration based on lock difficulty clears REFR_LOCK::kFlag_Locked.
		bool s_touching = false;
		bool s_pickAtDoor = false;
		bool s_shivAtDoor = false;
		std::chrono::steady_clock::time_point s_touchStartTime{};
		// Last mid-hold break roll (LockpickBreakDuringHold=1 rolls every
		// LockpickBreakRollIntervalMs while the unlock timer runs).
		std::chrono::steady_clock::time_point s_lastBreakRollTime{};
		int s_sessionUnlockHoldMs = 3000;
		int s_unlockHapticPulsesSent = 0;
		bool s_lockpickRespawnPending = false;
		bool s_lockpickSessionToolsGrabbed = false;
		std::chrono::steady_clock::time_point s_lockpickSessionReadyTime{};

		// Max separation between lockpick and shiv while both touch the door
		// (LockpickShivMaxDistance in INI).

		// Push-to-open (dummy sessions): hand position captured when contact
		// with the door began. A push is cumulative hand movement toward the
		// door from this anchor while contact holds (Throat Slit-style gesture
		// tracking: measure displacement, no timing gate). Reaching toward the
		// door cannot false-trigger because the anchor is only set once the
		// dummy is already pressed against the door.
		NiPoint3 s_touchAnchorHandPos{};
		bool s_hasTouchAnchor = false;

		// Dummy-door contact is detected via HIGGS's collision callback (real
		// physics contact, so ANY part of the door mesh works) gated by a
		// proximity check against the door's bounds. The callback just stamps
		// the time of the last collision on the holding hand.
		bool s_higgsCollisionCallbackRegistered = false;
		std::atomic<long long> s_lastDummyCollisionMs{ 0 };
		std::atomic<long long> s_lastLeftGameHandDoorCollisionMs{ 0 };
		std::atomic<long long> s_lastRightGameHandDoorCollisionMs{ 0 };
		std::atomic<long long> s_lastPickCollisionMs{ 0 };
		std::atomic<long long> s_lastShivCollisionMs{ 0 };
		constexpr long long kCollisionFreshMs = 400;
		constexpr float kDummyNearBoundsMargin = 10.0f;

		// Drop protection (Fake Edge VR pattern): if HIGGS reports the held
		// session item was dropped while the session is still active, it was
		// an accidental grip release - teleport it back to the hand and
		// re-grab it. Only EndLockpickSession (leaving the door vicinity)
		// legitimately releases the item, and it clears the session state
		// BEFORE releasing so this callback stays out of the way.
		bool s_higgsDropCallbacksRegistered = false;
		// Base form of the currently held session item, for the stashed /
		// consumed callbacks (the ref itself no longer exists at that point).
		UInt32 s_heldItemBaseFormId = 0;

		// The door slab's thickness axis, captured ONCE at session start while
		// the player is squarely in front of the door (the per-tick guess from
		// the player's current position degrades at point-blank range, which
		// made parts of the door unresponsive). The door is closed and static
		// for the whole session, so the axis stays valid.
		NiPoint3 s_doorThinAxis{};
		bool s_hasDoorThinAxis = false;

		// "Into the door" direction for push measurement, also captured once
		// at session start: the thin axis oriented away from the player. The
		// per-tick direction to the door's ORIGIN (its hinge) is skewed
		// sideways at point-blank range, which made close-range pushes
		// project to ~zero and never trigger.
		NiPoint3 s_doorPushDir{};
		bool s_hasDoorPushDir = false;

		// Signed mesh face normal (thin axis before player-side flip) and the
		// horizontal direction from the door to the player at session start.
		// Front face -> push open (hand moves toward the door). Back face ->
		// pull open (hand moves toward the player).
		NiPoint3 s_doorMeshFrontNormal{};
		bool s_hasDoorMeshFrontNormal = false;
		NiPoint3 s_sessionToPlayerDir{};
		bool s_hasSessionToPlayerDir = false;
		bool s_sessionOpenViaPull = false;

		// Per placed REFR FormID: interior non-load doors we opened via push
		// stay in vanilla-close mode until closed (A button or auto-close sync).
		std::unordered_set<UInt32> s_interiorDoorsAwaitingVanillaClose;

		long long NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		bool IsRestartCooldownActive()
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - s_lastSessionEndTime).count();
			return elapsed >= 0 && elapsed < kRestartCooldownMs;
		}

		float DistanceBetween(const NiPoint3& a, const NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		float Dot3(const NiPoint3& a, const NiPoint3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		NiPoint3 Cross3(const NiPoint3& a, const NiPoint3& b)
		{
			return NiPoint3(
				a.y * b.z - a.z * b.y,
				a.z * b.x - a.x * b.z,
				a.x * b.y - a.y * b.x);
		}

		NiPoint3 Normalize3(const NiPoint3& v)
		{
			const float len = std::sqrt(Dot3(v, v));
			if (len < 1.0e-4f)
				return NiPoint3(0.0f, 0.0f, 1.0f);

			return NiPoint3(v.x / len, v.y / len, v.z / len);
		}

		NiPoint3 MatrixColumn(const NiMatrix33& rot, int column)
		{
			if (column < 0)
				column = 0;
			if (column > 2)
				column = 2;

			return NiPoint3(rot.data[0][column], rot.data[1][column], rot.data[2][column]);
		}

		void MatrixFromOpenVRPose(const vr_1_0_12::HmdMatrix34_t& pose, NiMatrix33& outRot)
		{
			for (int col = 0; col < 3; ++col)
			{
				outRot.data[0][col] = pose.m[0][col];
				outRot.data[1][col] = pose.m[1][col];
				outRot.data[2][col] = pose.m[2][col];
			}
		}

		float SignedAngleDegreesAroundAxis(const NiPoint3& from, const NiPoint3& to, const NiPoint3& axis)
		{
			const NiPoint3 unitAxis = Normalize3(axis);
			NiPoint3 f = NiPoint3(
				from.x - unitAxis.x * Dot3(from, unitAxis),
				from.y - unitAxis.y * Dot3(from, unitAxis),
				from.z - unitAxis.z * Dot3(from, unitAxis));
			NiPoint3 t = NiPoint3(
				to.x - unitAxis.x * Dot3(to, unitAxis),
				to.y - unitAxis.y * Dot3(to, unitAxis),
				to.z - unitAxis.z * Dot3(to, unitAxis));
			f = Normalize3(f);
			t = Normalize3(t);

			const float sinA = Dot3(unitAxis, Cross3(f, t));
			const float cosA = Dot3(f, t);
			return std::atan2(sinA, cosA) * (180.0f / 3.14159265f);
		}

		bool GetSessionControllerIndices(vr_1_0_12::IVRSystem* ivrSystem,
			vr_1_0_12::TrackedDeviceIndex_t& mainOut,
			vr_1_0_12::TrackedDeviceIndex_t& offOut,
			bool mainHandIsLeft,
			bool offHandIsLeft);
		bool TryGetMainHandWorldRot(PlayerCharacter* player, NiMatrix33& outRot);

		bool TryGetMainHandControllerRot(NiMatrix33& outRot)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
				return false;

			vr_1_0_12::IVRCompositor* compositor = openVR->vrContext.m_pVRCompositor;
			if (!compositor)
				return false;

			vr_1_0_12::TrackedDeviceIndex_t mainController = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
			vr_1_0_12::TrackedDeviceIndex_t offController = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
			if (!GetSessionControllerIndices(openVR->vrSystem, mainController, offController,
				s_mainHandIsLeft, s_offHandIsLeft))
			{
				return false;
			}

			if (mainController == vr_1_0_12::k_unTrackedDeviceIndexInvalid)
				return false;

			vr_1_0_12::TrackedDevicePose_t gamePose{};
			const vr_1_0_12::EVRCompositorError err = compositor->GetLastPoseForTrackedDeviceIndex(
				mainController, nullptr, &gamePose);
			if (err != vr_1_0_12::VRCompositorError_None || !gamePose.bPoseIsValid)
				return false;

			MatrixFromOpenVRPose(gamePose.mDeviceToAbsoluteTracking, outRot);
			return true;
		}

		bool TryGetKeyDoorHandRot(PlayerCharacter* player, NiMatrix33& outRot)
		{
			if (TryGetMainHandControllerRot(outRot))
				return true;

			return TryGetMainHandWorldRot(player, outRot);
		}

		// Wrist twist: roll around the axis along the key into the lock (forearm axis).
		void BeginKeyDoorWristTwist(const NiMatrix33& rot, const NiPoint3& handPos, TESObjectREFR* door)
		{
			NiPoint3 toDoor = door ? (door->pos - handPos) : NiPoint3(0.0f, 1.0f, 0.0f);
			toDoor = Normalize3(toDoor);

			int fwdCol = 0;
			float bestFwdDot = -1.0f;
			for (int i = 0; i < 3; ++i)
			{
				const float fwdDot = std::fabs(Dot3(MatrixColumn(rot, i), toDoor));
				if (fwdDot > bestFwdDot)
				{
					bestFwdDot = fwdDot;
					fwdCol = i;
				}
			}

			s_keyDoorTwistAxis = MatrixColumn(rot, fwdCol);
			if (Dot3(s_keyDoorTwistAxis, toDoor) < 0.0f)
			{
				s_keyDoorTwistAxis.x = -s_keyDoorTwistAxis.x;
				s_keyDoorTwistAxis.y = -s_keyDoorTwistAxis.y;
				s_keyDoorTwistAxis.z = -s_keyDoorTwistAxis.z;
			}
			s_keyDoorTwistAxis = Normalize3(s_keyDoorTwistAxis);

			int refCol = (fwdCol + 1) % 3;
			float bestRefParallel = 2.0f;
			for (int i = 0; i < 3; ++i)
			{
				if (i == fwdCol)
					continue;

				const float parallel = std::fabs(Dot3(MatrixColumn(rot, i), s_keyDoorTwistAxis));
				if (parallel < bestRefParallel)
				{
					bestRefParallel = parallel;
					refCol = i;
				}
			}

			s_keyDoorTwistRefColumn = refCol;
			s_keyDoorTwistRefBaseline = MatrixColumn(rot, refCol);
			s_keyDoorTurnBaselineValid = true;
			s_keyDoorTurnPeakDeg = 0.0f;
		}

		float GetWristTwistDegrees(const NiMatrix33& currentRot)
		{
			const NiPoint3 currentRef = MatrixColumn(currentRot, s_keyDoorTwistRefColumn);
			return SignedAngleDegreesAroundAxis(s_keyDoorTwistRefBaseline, currentRef, s_keyDoorTwistAxis);
		}

		void ResetKeyDoorTurnState()
		{
			s_keyDoorTurnBaselineValid = false;
			s_keyDoorTurnPeakDeg = 0.0f;
			s_keyDoorTwistAxis = NiPoint3();
			s_keyDoorTwistRefBaseline = NiPoint3();
			s_keyDoorTwistRefColumn = 1;
		}

		SInt32 GetRefLockLevel(TESObjectREFR* ref)
		{
			if (!ref)
				return kUnlocked;

			REFR_LOCK* lock = TESObjectREFR_GetLock(ref);
			if (!lock)
				return kUnlocked;

			// Mirror CommonLib's REFR_LOCK::GetLockLevel: the native function
			// at 0x145380 only returns difficulty and ignores the locked flag.
			// When the lock is open (flag cleared) we must report kUnlocked
			// report kUnlocked without calling native.
			if ((lock->flags & REFR_LOCK::kFlag_Locked) == 0)
				return kUnlocked;

			return REFR_LOCK_GetLockLevel(lock, ref);
		}

		// The CURRENT locked state. GetRefLockLevel alone is not enough:
		// picking the lock open clears this flag but keeps the difficulty
		// level, so a picked-open door still reports e.g. "Average" forever.
		bool IsRefLocked(TESObjectREFR* ref)
		{
			if (!ref)
				return false;

			REFR_LOCK* lock = TESObjectREFR_GetLock(ref);
			return lock && (lock->flags & REFR_LOCK::kFlag_Locked) != 0;
		}

		// Mirror CommonLib REFR_LOCK::SetLocked(false): clears kFlag_Locked and
		// resets numTries. Interactive Activators uses TESObjectREFR::IsLocked(),
		// which reads this same flag via GetLock().
		bool UnlockRef(TESObjectREFR* ref)
		{
			if (!ref)
				return false;

			REFR_LOCK* lock = TESObjectREFR_GetLock(ref);
			if (!lock)
				return false;

			if ((lock->flags & REFR_LOCK::kFlag_Locked) == 0)
				return true;

			lock->flags &= static_cast<UInt8>(~REFR_LOCK::kFlag_Locked);
			lock->numTries = 0;
			return true;
		}

		bool IsPickableLockLevel(SInt32 level)
		{
			return level >= kVeryEasy && level <= kVeryHard;
		}

		// Hold time before direct unlock, keyed to Skyrim lock difficulty.
		int GetUnlockHoldMsForLockLevel(SInt32 level)
		{
			switch (level)
			{
			case kVeryEasy: return lockTierNoviceHoldMs;
			case kEasy: return lockTierApprenticeHoldMs;
			case kAverage: return lockTierAdeptHoldMs;
			case kHard: return lockTierExpertHoldMs;
			case kVeryHard: return lockTierMasterHoldMs;
			default: return lockTierAdeptHoldMs;
			}
		}

		const char* GetLockTierName(SInt32 level)
		{
			switch (level)
			{
			case kVeryEasy: return "Novice";
			case kEasy: return "Apprentice";
			case kAverage: return "Adept";
			case kHard: return "Expert";
			case kVeryHard: return "Master";
			default: return "Unknown";
			}
		}

		// HapticSkyrimVR pattern: OpenVR TriggerHapticPulse on both held hands.
		bool GetSessionControllerIndices(vr_1_0_12::IVRSystem* ivrSystem,
			vr_1_0_12::TrackedDeviceIndex_t& mainOut,
			vr_1_0_12::TrackedDeviceIndex_t& offOut,
			bool mainHandIsLeft,
			bool offHandIsLeft)
		{
			if (!ivrSystem)
				return false;

			const bool mainLeft = GameHandToVRController(mainHandIsLeft);
			const bool offLeft = GameHandToVRController(offHandIsLeft);

			const vr_1_0_12::ETrackedControllerRole mainRole = mainLeft
				? vr_1_0_12::TrackedControllerRole_LeftHand
				: vr_1_0_12::TrackedControllerRole_RightHand;
			const vr_1_0_12::ETrackedControllerRole offRole = offLeft
				? vr_1_0_12::TrackedControllerRole_LeftHand
				: vr_1_0_12::TrackedControllerRole_RightHand;

			mainOut = ivrSystem->GetTrackedDeviceIndexForControllerRole(mainRole);
			offOut = ivrSystem->GetTrackedDeviceIndexForControllerRole(offRole);
			return mainOut != vr_1_0_12::k_unTrackedDeviceIndexInvalid
				|| offOut != vr_1_0_12::k_unTrackedDeviceIndexInvalid;
		}

		void TriggerHapticOnControllers(vr_1_0_12::IVRSystem* ivrSystem,
			vr_1_0_12::TrackedDeviceIndex_t mainController,
			vr_1_0_12::TrackedDeviceIndex_t offController,
			unsigned short pulseDurationUs)
		{
			if (!ivrSystem)
				return;

			if (mainController != vr_1_0_12::k_unTrackedDeviceIndexInvalid)
				ivrSystem->TriggerHapticPulse(mainController, 0, pulseDurationUs);
			if (offController != vr_1_0_12::k_unTrackedDeviceIndexInvalid)
				ivrSystem->TriggerHapticPulse(offController, 0, pulseDurationUs);
		}

		void PulseLockpickSessionHaptics()
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
				return;

			vr_1_0_12::TrackedDeviceIndex_t mainController = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
			vr_1_0_12::TrackedDeviceIndex_t offController = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
			if (!GetSessionControllerIndices(openVR->vrSystem, mainController, offController,
				s_mainHandIsLeft, s_offHandIsLeft))
			{
				return;
			}

			TriggerHapticOnControllers(openVR->vrSystem, mainController, offController, 3199);
		}

		// Short pulse on the hand that just pushed/pulled the door open.
		void PulseDoorOpenGestureHaptic(bool mainHandIsLeft)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
				return;

			const bool isLeftVR = GameHandToVRController(mainHandIsLeft);
			const vr_1_0_12::ETrackedControllerRole role = isLeftVR
				? vr_1_0_12::TrackedControllerRole_LeftHand
				: vr_1_0_12::TrackedControllerRole_RightHand;

			const vr_1_0_12::TrackedDeviceIndex_t controller =
				openVR->vrSystem->GetTrackedDeviceIndexForControllerRole(role);
			if (controller == vr_1_0_12::k_unTrackedDeviceIndexInvalid)
				return;

			constexpr unsigned short kDoorPushOpenPulseUs = 1200;
			openVR->vrSystem->TriggerHapticPulse(controller, 0, kDoorPushOpenPulseUs);
		}

		// Sustained strong rumble on successful unlock (runs off the game thread).
		void PlayUnlockSuccessHaptics(bool mainHandIsLeft, bool offHandIsLeft)
		{
			std::thread([mainHandIsLeft, offHandIsLeft]()
			{
				BSOpenVR* openVR = *g_openVR;
				if (!openVR || !openVR->vrSystem)
					return;

				vr_1_0_12::IVRSystem* ivrSystem = openVR->vrSystem;
				vr_1_0_12::TrackedDeviceIndex_t mainController = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
				vr_1_0_12::TrackedDeviceIndex_t offController = vr_1_0_12::k_unTrackedDeviceIndexInvalid;
				if (!GetSessionControllerIndices(ivrSystem, mainController, offController,
					mainHandIsLeft, offHandIsLeft))
				{
					return;
				}

				constexpr unsigned short kStrongPulseUs = 3999;
				constexpr int kDurationMs = 2000;
				constexpr int kPulseIntervalMs = 8;

				const auto start = std::chrono::steady_clock::now();
				while (std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - start).count() < kDurationMs)
				{
					TriggerHapticOnControllers(ivrSystem, mainController, offController, kStrongPulseUs);
					std::this_thread::sleep_for(std::chrono::milliseconds(kPulseIntervalMs));
				}
			}).detach();
		}

		bool IsDoor(TESObjectREFR* ref)
		{
			return ref && ref->baseForm && ref->baseForm->formType == kFormType_Door;
		}

		bool IsDoorLockedAndRequiresKey(TESObjectREFR* ref)
		{
			if (!IsDoor(ref) || !IsRefLocked(ref))
				return false;

			const SInt32 level = GetRefLockLevel(ref);
			if (level == kRequiresKey)
				return true;

			REFR_LOCK* lock = TESObjectREFR_GetLock(ref);
			return lock && lock->baseLevel == static_cast<SInt8>(kRequiresKey);
		}

		TESForm* GetDoorRequiredKeyForm(TESObjectREFR* ref)
		{
			if (!ref)
				return nullptr;

			REFR_LOCK* lock = TESObjectREFR_GetLock(ref);
			if (!lock || !lock->key)
				return nullptr;

			return static_cast<TESForm*>(lock->key);
		}

		const char* GetFormLogName(TESForm* form)
		{
			if (!form)
				return "(none)";

			const char* name = form->GetFullName();
			if (name && name[0])
				return name;

			return "(unnamed)";
		}

		bool PlayerHasItem(PlayerCharacter* player, TESForm* itemForm)
		{
			if (!player || !itemForm)
				return false;

			ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
				return false;

			InventoryEntryData* entry = containerChanges->data->FindItemEntry(itemForm);
			return entry && entry->countDelta > 0;
		}

		void LogKeyRequiredDoor(TESObjectREFR* doorRef, PlayerCharacter* player)
		{
			if (!doorRef || !player)
				return;

			TESForm* keyForm = GetDoorRequiredKeyForm(doorRef);
			const UInt32 baseFormId = doorRef->baseForm ? doorRef->baseForm->formID : 0;

			if (keyForm)
			{
				const bool hasKey = PlayerHasItem(player, keyForm);
				LOG_INFO(
					"[KeyDoor] door ref %08X (base %08X) requires key %08X (%s) — player has key: %s",
					doorRef->formID,
					baseFormId,
					keyForm->formID,
					GetFormLogName(keyForm),
					hasKey ? "yes" : "no");
			}
			else
			{
				LOG_INFO(
					"[KeyDoor] door ref %08X (base %08X) requires a key (lock level RequiresKey) but no key item is set on the lock — player has key: no",
					doorRef->formID,
					baseFormId);
			}
		}

		static bool CiSubstring(const char* haystack, const char* needle)
		{
			if (!haystack || !needle || !needle[0])
				return false;

			for (const char* h = haystack; *h; ++h)
			{
				const char* p = h;
				const char* n = needle;
				while (*p && *n
					&& std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*n)))
				{
					++p;
					++n;
				}
				if (!*n)
					return true;
			}
			return false;
		}

		static bool DisplayNameIsTrapDoor(const char* name)
		{
			return CiSubstring(name, "trap door") || CiSubstring(name, "trapdoor");
		}

		// Vanilla trap doors use the FULL record display name (e.g. "Trap Door").
		bool IsTrapDoorByDisplayName(TESObjectREFR* ref)
		{
			if (!IsDoor(ref))
				return false;

			const char* refName = CALL_MEMBER_FN(ref, GetReferenceName)();
			if (DisplayNameIsTrapDoor(refName))
				return true;

			if (ref->baseForm)
			{
				TESFullName* fullName = DYNAMIC_CAST(ref->baseForm, TESForm, TESFullName);
				if (fullName && fullName->name.data)
					return DisplayNameIsTrapDoor(fullName->name.data);
			}

			return false;
		}

		bool IsDoorLockedAndPickable(TESObjectREFR* ref)
		{
			if (!IsDoor(ref))
				return false;

			return IsPickableLockLevel(GetRefLockLevel(ref));
		}

		bool IsDoorUnlocked(TESObjectREFR* ref)
		{
			if (!IsDoor(ref))
				return false;

			return GetRefLockLevel(ref) == kUnlocked;
		}

		// Load door = activating it loads into another cell (the ref carries
		// teleport data). Interior doors that just swing open do not.
		bool IsLoadDoor(TESObjectREFR* ref)
		{
			return ref && ref->extraData.HasType(kExtraData_Teleport);
		}

		// Load doors listed on ExcludeLoadDoorBases / ExcludeDoorRefs, hard-coded
		// cave bases, or the sliding-door FNAM flag (cave mouths) skip push-to-open.
		bool IsCaveLoadDoor(TESObjectREFR* ref)
		{
			if (!IsLoadDoor(ref) || !ref->baseForm)
				return false;

			if (IsExcludedDoorRef(ref) || IsExcludedLoadDoorBase(ref))
				return true;

			const UInt32 baseLocal = ref->baseForm->formID & 0x00FFFFFF;
			for (UInt32 caveBaseId : kCaveLoadDoorBaseFormIds)
			{
				if (baseLocal == caveBaseId)
					return true;
			}

			const TESObjectDOOR* doorBase = DYNAMIC_CAST(ref->baseForm, TESForm, TESObjectDOOR);
			if (doorBase)
			{
				if ((doorBase->unkB0 & kDoorFlagAutomatic) != 0)
					return true;
				if ((doorBase->unkB0 & kDoorFlagSliding) != 0)
					return true;
			}

			return false;
		}

		bool IsLoadDoorExcludedFromPush(TESObjectREFR* ref)
		{
			return IsCaveLoadDoor(ref);
		}

		// TESObjectCELL::cellFlags at offset 0x40 (CommonLib Flag::kIsInteriorCell).
		constexpr UInt16 kCellFlagInterior = 1 << 0;

		bool IsInteriorCell(TESObjectCELL* cell)
		{
			return cell && (cell->unk040 & kCellFlagInterior) != 0;
		}

		bool IsInteriorNonLoadDoor(TESObjectREFR* ref)
		{
			if (!ref || IsLoadDoor(ref))
				return false;

			return ref->parentCell && IsInteriorCell(ref->parentCell);
		}

		bool IsFullyExcludedDoorBase(TESObjectREFR* ref)
		{
			if (!ref || !ref->baseForm)
				return false;

			const UInt32 baseFormId = ref->baseForm->formID;
			for (UInt32 excludedId : kFullyExcludedDoorBaseFormIds)
			{
				if (baseFormId == excludedId)
					return true;
			}

			return false;
		}

		// Excluded refs, trap doors, or interior non-load bases on ExcludeInteriorDoorBases.
		// With ExcludeLockedDoors=1, every locked door is 100% vanilla too: no
		// lockpick sessions, no blocked activation, vanilla minigame untouched.
		bool IsVanillaDoorOnly(TESObjectREFR* ref)
		{
			if (IsExcludedDoorRef(ref) || IsTrapDoorByDisplayName(ref) || IsFullyExcludedDoorBase(ref))
				return true;

			if (IsLoadDoorExcludedFromPush(ref))
				return true;

			if (excludeLockedDoors != 0 && IsDoor(ref) && IsRefLocked(ref))
				return true;

			return IsInteriorNonLoadDoor(ref) && IsExcludedInteriorDoorBase(ref);
		}

		bool IsInteriorDoorAwaitingVanillaClose(TESObjectREFR* ref)
		{
			return ref && s_interiorDoorsAwaitingVanillaClose.count(ref->formID) != 0;
		}

		void MarkInteriorDoorAwaitingVanillaClose(UInt32 doorFormId)
		{
			s_interiorDoorsAwaitingVanillaClose.insert(doorFormId);
		}

		void ClearInteriorDoorAwaitingVanillaClose(UInt32 doorFormId)
		{
			s_interiorDoorsAwaitingVanillaClose.erase(doorFormId);
		}

		DoorOpenState GetDoorOpenState(TESObjectREFR* ref)
		{
			if (!ref)
				return kDoorOpenStateNone;

			return static_cast<DoorOpenState>(BGSOpenCloseForm_GetOpenState(ref));
		}

		bool IsDoorPhysicallyOpen(TESObjectREFR* ref)
		{
			const DoorOpenState state = GetDoorOpenState(ref);
			return state == kDoorOpenStateOpen || state == kDoorOpenStateOpening;
		}

		bool IsDoorPhysicallyClosed(TESObjectREFR* ref)
		{
			const DoorOpenState state = GetDoorOpenState(ref);
			return state == kDoorOpenStateClosed || state == kDoorOpenStateClosing;
		}

		bool IsNonLoadDoorWithVanillaCloseToggle(TESObjectREFR* ref)
		{
			return IsInteriorNonLoadDoor(ref) && !IsExcludedInteriorDoorBase(ref);
		}

		// If this ref was left in vanilla-close mode but the door is physically
		// closed again (auto-close, load, etc.), restore push logic for it only.
		void SyncDoorAwaitingVanillaCloseState(TESObjectREFR* ref)
		{
			if (!ref || !IsNonLoadDoorWithVanillaCloseToggle(ref))
				return;

			if (IsInteriorDoorAwaitingVanillaClose(ref) && IsDoorPhysicallyClosed(ref))
			{
				ClearInteriorDoorAwaitingVanillaClose(ref->formID);
				LOG_INFO("Door lockpick: door %08X is closed, push logic restored for this ref",
					ref->formID);
			}
		}

		bool IsInteriorNonLoadDoorUsingPushLogic(TESObjectREFR* ref)
		{
			return IsInteriorNonLoadDoor(ref)
				&& !IsExcludedInteriorDoorBase(ref)
				&& !IsInteriorDoorAwaitingVanillaClose(ref);
		}

		// Hard-coded push exclusions — lockpicking unchanged; push and hidden
		// activate text do not apply.
		bool IsExcludedFromDoorPushBase(TESObjectREFR* ref)
		{
			if (!ref || !ref->baseForm)
				return false;

			const UInt32 baseFormId = ref->baseForm->formID;
			for (UInt32 excludedId : kExcludedDoorPushBaseFormIds)
			{
				if (baseFormId == excludedId)
					return true;
			}

			return false;
		}

		// Unlocked doors that get the dummy/push session:
		//  - load doors (teleport), except bases/refs on ExcludeLoadDoorBases
		//  - non-load doors in outdoor/exterior world space
		//  - interior non-load doors unless base DOOR is on ExcludeInteriorDoorBases
		// Hard-coded base exclusions (kExcludedDoorPushBaseFormIds) apply here only.
		bool IsUnlockedDoorSessionEligible(TESObjectREFR* ref)
		{
			if (unlockedDoorPush == 0)
				return false;

			if (!IsDoorUnlocked(ref))
				return false;

			if (IsExcludedFromDoorPushBase(ref))
				return false;

			if (IsLoadDoor(ref))
				return !IsLoadDoorExcludedFromPush(ref);

			if (ref->parentCell && !IsInteriorCell(ref->parentCell))
				return true;

			return IsInteriorNonLoadDoorUsingPushLogic(ref);
		}

		// "In front of the door": close enough AND still roughly facing it.
		bool IsPlayerFacingDoor(PlayerCharacter* player, TESObjectREFR* doorRef)
		{
			if (!player || !doorRef)
				return false;

			NiPoint3 toDoor = doorRef->pos - player->pos;
			toDoor.z = 0.0f;
			const float len = std::sqrt(toDoor.x * toDoor.x + toDoor.y * toDoor.y);
			if (len < 1.0f)
				return true;

			// Skyrim heading: 0 = +Y (north), increasing clockwise.
			const float yaw = player->rot.z;
			const float dot = (toDoor.x / len) * std::sin(yaw) + (toDoor.y / len) * std::cos(yaw);
			return dot >= kDummyFacingDotMin;
		}

		bool IsPlayerStillInFrontOfDoor(PlayerCharacter* player, TESObjectREFR* doorRef)
		{
			if (!player || !doorRef)
				return false;

			const float dist = DistanceBetween(player->pos, doorRef->pos);
			if (dist > GetDoorSessionEndDistance(doorRef))
				return false;

			if (dist <= kDummyNearDistance)
				return true;

			return IsPlayerFacingDoor(player, doorRef);
		}

		bool IsAutoLoadStyleDoor(TESObjectREFR* ref)
		{
			if (!IsLoadDoor(ref) || !ref->baseForm)
				return false;

			const UInt32 baseLocal = ref->baseForm->formID & 0x00FFFFFF;
			if (baseLocal == 0x00031897)
				return true;

			const TESObjectDOOR* doorBase = DYNAMIC_CAST(ref->baseForm, TESForm, TESObjectDOOR);
			return doorBase && (doorBase->unkB0 & kDoorFlagAutomatic) != 0;
		}

		float GetLoadDoorApproachDistance(TESObjectREFR* ref)
		{
			if (IsAutoLoadStyleDoor(ref))
				return kAutoLoadDoorApproachDistance;

			return GetDoorSessionStartDistance(ref);
		}

		bool IsPlayerInLoadDoorApproachRange(PlayerCharacter* player, TESObjectREFR* doorRef)
		{
			if (!player || !doorRef)
				return false;

			return DistanceBetween(player->pos, doorRef->pos) <= GetLoadDoorApproachDistance(doorRef);
		}

		// Normal doors: session distance + facing. AutoLoad cave doors: large
		// radius only (REF origin is at the load point, not the visible mouth).
		bool IsPlayerInFrontOfLoadDoor(PlayerCharacter* player, TESObjectREFR* doorRef)
		{
			if (!IsPlayerInLoadDoorApproachRange(player, doorRef))
				return false;

			if (IsAutoLoadStyleDoor(doorRef))
				return true;

			return IsPlayerFacingDoor(player, doorRef);
		}

		void LogLoadDoorState(TESObjectREFR* doorRef, const char* tag, float dist,
			float startDist, float endDist)
		{
			if (!doorRef || !tag)
				return;

			const TESObjectDOOR* doorBase = doorRef->baseForm
				? DYNAMIC_CAST(doorRef->baseForm, TESForm, TESObjectDOOR)
				: nullptr;
			const UInt8 doorFlags = doorBase ? doorBase->unkB0 : 0;
			const UInt32 baseLocal = doorRef->baseForm ? (doorRef->baseForm->formID & 0x00FFFFFF) : 0;

			LOG_INFO(
				"[%s] ref=%08X base=%08X baseLocal=%06X dist=%.1f startDist=%.1f endDist=%.1f "
				"load=%d cave=%d autoload=%d automatic=%d sliding=%d excludedBase=%d excludedRef=%d "
				"vanillaOnly=%d pushEligible=%d",
				tag,
				doorRef->formID,
				doorRef->baseForm ? doorRef->baseForm->formID : 0,
				baseLocal,
				dist,
				startDist,
				endDist,
				IsLoadDoor(doorRef) ? 1 : 0,
				IsCaveLoadDoor(doorRef) ? 1 : 0,
				IsAutoLoadStyleDoor(doorRef) ? 1 : 0,
				(doorFlags & kDoorFlagAutomatic) != 0 ? 1 : 0,
				(doorFlags & kDoorFlagSliding) != 0 ? 1 : 0,
				IsExcludedLoadDoorBase(doorRef) ? 1 : 0,
				IsExcludedDoorRef(doorRef) ? 1 : 0,
				IsVanillaDoorOnly(doorRef) ? 1 : 0,
				IsUnlockedDoorSessionEligible(doorRef) ? 1 : 0);
		}

		// VR crosshair often hits foliage in front of cave mouths; scan the
		// player's cell for nearby load doors (AutoLoad doors use a larger radius).
		TESObjectREFR* FindNearestLoadDoorInFront(PlayerCharacter* player)
		{
			if (!player || !player->parentCell)
				return nullptr;

			TESObjectREFR* best = nullptr;
			float bestDist = FLT_MAX;
			TESObjectCELL* cell = player->parentCell;

			for (UInt32 i = 0; i < cell->refData.maxSize; ++i)
			{
				const auto& entry = cell->refData.refArray[i];
				if (!entry.unk08 || !entry.ref || !IsLoadDoor(entry.ref))
					continue;

				if (!IsPlayerInFrontOfLoadDoor(player, entry.ref))
					continue;

				const float dist = DistanceBetween(player->pos, entry.ref->pos);
				if (dist < bestDist)
				{
					bestDist = dist;
					best = entry.ref;
				}
			}

			return best;
		}

		TESObjectREFR* ResolveLoadDoorForEntranceLogging()
		{
			if (s_crosshairCaveDoorFormId != 0)
			{
				TESForm* doorForm = LookupFormByID(s_crosshairCaveDoorFormId);
				TESObjectREFR* doorRef = doorForm ? DYNAMIC_CAST(doorForm, TESForm, TESObjectREFR) : nullptr;
				if (doorRef && IsLoadDoor(doorRef))
					return doorRef;
			}

			PlayerCharacter* player = *g_thePlayer;
			return player ? FindNearestLoadDoorInFront(player) : nullptr;
		}

		// Resolves the dummy item's runtime FormID (ESL-flagged plugin, so the
		// load-order-dependent prefix has to be looked up by plugin name).
		TESForm* LookupDummyForm()
		{
			if (s_resolvedDummyFormId == 0)
			{
				s_resolvedDummyFormId = GetFullFormIdFromEspAndFormId(MOD_ESP_NAME, kDummyLocalFormId);
				if (s_resolvedDummyFormId != 0)
					LOG_INFO("Door lockpick: dummy item resolved to %08X (%s)", s_resolvedDummyFormId, MOD_ESP_NAME);
			}

			if (s_resolvedDummyFormId == 0)
				return nullptr;

			return LookupFormByID(s_resolvedDummyFormId);
		}

		// NiAVObject::unkE4..unkF0 is the world bound (NiBound: center + radius).
		NiPoint3 WorldBoundCenter(const NiAVObject* obj)
		{
			return NiPoint3(obj->unkE4, obj->unkE8, obj->unkEC);
		}

		float WorldBoundRadius(const NiAVObject* obj)
		{
			return obj->unkF0;
		}

		// Approximate "pick touches the door slab" test against one scene-graph
		// leaf. The pick must be inside the leaf's world bound sphere AND within
		// lockpickTouchDistance of the slab's surface plane. The slab's thickness axis
		// is the local axis most aligned with the player's approach direction
		// (an upright door faces the player; a trapdoor faces up at them), so
		// distance measured along it is distance to the door surface, not just
		// distance to the door in general.
		bool PointTouchesLeafBound(const NiAVObject* leaf, const NiPoint3& point, const NiPoint3& playerPos, float maxPlaneDist, float& outPlaneDist)
		{
			const float radius = WorldBoundRadius(leaf);
			if (radius < 1.0f)
				return false;

			const NiPoint3 center = WorldBoundCenter(leaf);
			const NiPoint3 delta = point - center;

			if (DistanceBetween(point, center) > radius)
				return false;

			NiPoint3 approach = center - playerPos;
			const float approachLen = std::sqrt(approach.x * approach.x + approach.y * approach.y + approach.z * approach.z);
			if (approachLen < 1.0f)
				return false;
			approach.x /= approachLen;
			approach.y /= approachLen;
			approach.z /= approachLen;

			const NiMatrix33& rot = leaf->m_worldTransform.rot;
			float bestAlignment = -1.0f;
			float planeDist = 9999.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				const NiPoint3 axisDir(rot.data[0][axis], rot.data[1][axis], rot.data[2][axis]);
				const float alignment = std::fabs(approach.x * axisDir.x + approach.y * axisDir.y + approach.z * axisDir.z);
				if (alignment > bestAlignment)
				{
					bestAlignment = alignment;
					planeDist = std::fabs(delta.x * axisDir.x + delta.y * axisDir.y + delta.z * axisDir.z);
				}
			}

			outPlaneDist = planeDist;
			return planeDist <= maxPlaneDist;
		}

		bool ProbeDoorGeometryRecursive(NiAVObject* obj, const NiPoint3& point, const NiPoint3& playerPos, int depth, float maxPlaneDist, float& outPlaneDist)
		{
			if (!obj || depth > 8)
				return false;

			NiNode* node = ni_cast(obj, NiNode);
			if (node)
			{
				bool hadChild = false;
				for (UInt16 i = 0; i < node->m_children.m_emptyRunStart; ++i)
				{
					NiAVObject* child = node->m_children.m_data ? node->m_children.m_data[i] : nullptr;
					if (!child)
						continue;

					hadChild = true;
					if (ProbeDoorGeometryRecursive(child, point, playerPos, depth + 1, maxPlaneDist, outPlaneDist))
						return true;
				}

				if (hadChild)
					return false;
				// Childless node: fall through and test its own bound.
			}

			return PointTouchesLeafBound(obj, point, playerPos, maxPlaneDist, outPlaneDist);
		}

		// Hysteresis: entering contact requires touchDistance, but once
		// touching, contact holds up to +kTouchExitSlack so jitter at the
		// threshold cannot keep resetting the touch state. The lockpick and
		// the unlocked-door dummy use their own touchDistance INI settings.
		bool IsPickTouchingDoor(TESObjectREFR* doorRef, const NiPoint3& pickPos, const NiPoint3& playerPos, float touchDistance, bool alreadyTouching, float& outPlaneDist)
		{
			if (!doorRef)
				return false;

			NiNode* doorNode = doorRef->GetNiNode();
			if (!doorNode)
				return false;

			const float maxPlaneDist = alreadyTouching
				? touchDistance + kTouchExitSlack
				: touchDistance;

			return ProbeDoorGeometryRecursive(doorNode, pickPos, playerPos, 0, maxPlaneDist, outPlaneDist);
		}

		// Captures the door's thickness axis: the door node's local axis most
		// aligned with the horizontal direction from the player to the door.
		bool ComputeDoorThinAxis(TESObjectREFR* doorRef, const NiPoint3& playerPos, NiPoint3& outAxis)
		{
			if (!doorRef)
				return false;

			NiNode* doorNode = doorRef->GetNiNode();
			if (!doorNode)
				return false;

			NiPoint3 approach = doorRef->pos - playerPos;
			approach.z = 0.0f;
			const float len = std::sqrt(approach.x * approach.x + approach.y * approach.y);
			if (len < 1.0f)
				return false;
			approach.x /= len;
			approach.y /= len;

			const NiMatrix33& rot = doorNode->m_worldTransform.rot;
			float bestAlignment = -1.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				const NiPoint3 axisDir(rot.data[0][axis], rot.data[1][axis], rot.data[2][axis]);
				const float alignment = std::fabs(approach.x * axisDir.x + approach.y * axisDir.y);
				if (alignment > bestAlignment)
				{
					bestAlignment = alignment;
					outAxis = axisDir;
				}
			}

			return bestAlignment > 0.0f;
		}

		bool NormalizeHorizontalDir(NiPoint3& dir)
		{
			dir.z = 0.0f;
			const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			if (len < 1.0f)
				return false;

			dir.x /= len;
			dir.y /= len;
			return true;
		}

		NiPoint3 GetDoorGeometryAnchor(TESObjectREFR* doorRef)
		{
			if (!doorRef)
				return NiPoint3();

			NiNode* doorNode = doorRef->GetNiNode();
			if (doorNode)
				return WorldBoundCenter(doorNode);

			return doorRef->pos;
		}

		// True when the player stands on the mesh "front" half-space (exterior /
		// push-open side). Skyrim door thin-axis sign does not match CK "front",
		// so the exterior side is where dot(toPlayer, meshNormal) <= 0.
		bool IsPlayerOnDoorFrontFace(const NiPoint3& playerPos, const NiPoint3& doorAnchor, const NiPoint3& meshFrontNormal)
		{
			NiPoint3 toPlayer = playerPos - doorAnchor;
			if (!NormalizeHorizontalDir(toPlayer))
				return true;

			NiPoint3 normal = meshFrontNormal;
			if (!NormalizeHorizontalDir(normal))
				return true;

			return toPlayer.x * normal.x + toPlayer.y * normal.y <= 0.0f;
		}

		// True when the point is at the door's surface anywhere on its face:
		// inside a geometry leaf's bound sphere AND within maxPlaneDist of the
		// slab plane through that leaf's center along the stored thin axis.
		// The plane spans the entire door face, so every part of the mesh
		// registers equally.
		bool NearDoorSlabRecursive(NiAVObject* obj, const NiPoint3& point, const NiPoint3& thinAxis, float maxPlaneDist, int depth)
		{
			if (!obj || depth > 8)
				return false;

			NiNode* node = ni_cast(obj, NiNode);
			if (node)
			{
				bool hadChild = false;
				for (UInt16 i = 0; i < node->m_children.m_emptyRunStart; ++i)
				{
					NiAVObject* child = node->m_children.m_data ? node->m_children.m_data[i] : nullptr;
					if (!child)
						continue;

					hadChild = true;
					if (NearDoorSlabRecursive(child, point, thinAxis, maxPlaneDist, depth + 1))
						return true;
				}

				if (hadChild)
					return false;
			}

			const float radius = WorldBoundRadius(obj);
			if (radius < 1.0f)
				return false;

			const NiPoint3 center = WorldBoundCenter(obj);
			if (DistanceBetween(point, center) > radius + 2.0f)
				return false;

			const NiPoint3 delta = point - center;
			const float planeDist = std::fabs(delta.x * thinAxis.x + delta.y * thinAxis.y + delta.z * thinAxis.z);
			return planeDist <= maxPlaneDist;
		}

		bool IsPickAtDoorSlab(TESObjectREFR* doorRef, const NiPoint3& pickPos, float maxPlaneDist)
		{
			if (!doorRef || !s_hasDoorThinAxis)
				return false;

			NiNode* doorNode = doorRef->GetNiNode();
			if (!doorNode)
				return false;

			return NearDoorSlabRecursive(doorNode, pickPos, s_doorThinAxis, maxPlaneDist, 0);
		}

		// True when the point is inside (or within margin of) any geometry
		// leaf's world bound sphere - "somewhere at the door", any part of it.
		bool NearLeafBoundsRecursive(NiAVObject* obj, const NiPoint3& point, float margin, int depth)
		{
			if (!obj || depth > 8)
				return false;

			NiNode* node = ni_cast(obj, NiNode);
			if (node)
			{
				bool hadChild = false;
				for (UInt16 i = 0; i < node->m_children.m_emptyRunStart; ++i)
				{
					NiAVObject* child = node->m_children.m_data ? node->m_children.m_data[i] : nullptr;
					if (!child)
						continue;

					hadChild = true;
					if (NearLeafBoundsRecursive(child, point, margin, depth + 1))
						return true;
				}

				if (hadChild)
					return false;
			}

			const float radius = WorldBoundRadius(obj);
			if (radius < 1.0f)
				return false;

			return DistanceBetween(point, WorldBoundCenter(obj)) <= radius + margin;
		}

		bool IsPickNearDoorBounds(TESObjectREFR* doorRef, const NiPoint3& pickPos, float margin)
		{
			if (!doorRef)
				return false;

			NiNode* doorNode = doorRef->GetNiNode();
			if (!doorNode)
				return false;

			return NearLeafBoundsRecursive(doorNode, pickPos, margin, 0);
		}

		// HIGGS fires this when a hand or its held object collides with
		// anything. During a session (lockpick or dummy), a collision on the
		// holding hand while the held item is near the door = real contact
		// with the door mesh.
		void OnHiggsCollision(bool isLeft, float mass, float separatingVelocity)
		{
			if (!s_sessionActive)
				return;

			if (s_sessionIsHandPush)
			{
				if (GameHandToVRController(true) == isLeft)
					s_lastLeftGameHandDoorCollisionMs = NowMs();
				else if (GameHandToVRController(false) == isLeft)
					s_lastRightGameHandDoorCollisionMs = NowMs();
				return;
			}

			if (s_sessionIsDummy)
			{
				const bool holdingHandIsLeft = GameHandToVRController(s_mainHandIsLeft);
				if (isLeft == holdingHandIsLeft)
					s_lastDummyCollisionMs = NowMs();
				return;
			}

			if (s_sessionIsKeyDoor)
			{
				const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
				if (isLeft == mainHandIsLeftVR)
					s_lastPickCollisionMs = NowMs();
				return;
			}

			const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
			const bool offHandIsLeftVR = GameHandToVRController(s_offHandIsLeft);
			if (isLeft == mainHandIsLeftVR)
				s_lastPickCollisionMs = NowMs();
			if (isLeft == offHandIsLeftVR)
				s_lastShivCollisionMs = NowMs();
		}

		bool TryGetRefWorldPos(TESObjectREFR* ref, NiPoint3& outPos)
		{
			if (!ref)
				return false;

			// Reject stale pointers if the ref was deleted between ticks.
			TESForm* form = LookupFormByID(ref->formID);
			TESObjectREFR* live = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
			if (live != ref)
				return false;

			NiNode* node = ref->GetNiNode();
			if (!node)
				return false;

			outPos = node->m_worldTransform.pos;
			return true;
		}

		bool TryGetLockpickWorldPos(NiPoint3& outPos)
		{
			return TryGetRefWorldPos(s_spawnedLockpick, outPos);
		}

		bool TryGetShivWorldPos(NiPoint3& outPos)
		{
			return TryGetRefWorldPos(s_spawnedShiv, outPos);
		}

		bool TryGetGameHandWorldPos(PlayerCharacter* player, bool isLeftGameHand, NiPoint3& outPos)
		{
			if (!player)
				return false;

			NiNode* root = player->GetNiNode();
			if (!root)
				return false;

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
			const char* boneName = isLeftVRController ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]";
			BSFixedString boneNameStr(boneName);
			NiAVObject* hand = root->GetObjectByName(&boneNameStr.data);
			if (!hand)
				return false;

			outPos = hand->m_worldTransform.pos;
			return true;
		}

		bool TryGetGameHandWorldRot(PlayerCharacter* player, bool isLeftGameHand, NiMatrix33& outRot)
		{
			if (!player)
				return false;

			NiNode* root = player->GetNiNode();
			if (!root)
				return false;

			const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
			const char* boneName = isLeftVRController ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]";
			BSFixedString boneNameStr(boneName);
			NiAVObject* hand = root->GetObjectByName(&boneNameStr.data);
			if (!hand)
				return false;

			outRot = hand->m_worldTransform.rot;
			return true;
		}

		// World position of the hand bone holding the spawned item. The held
		// object itself gets stopped by the door's collision, so pushes are
		// measured from the controller-driven hand bone instead.
		bool TryGetMainHandWorldPos(PlayerCharacter* player, NiPoint3& outPos)
		{
			const bool isLeftGameHand = (s_sessionActive && (s_sessionIsDummy || s_heldItemBaseFormId != 0))
				? s_mainHandIsLeft
				: IsMainHandLeftGameHand();
			return TryGetGameHandWorldPos(player, isLeftGameHand, outPos);
		}

		bool TryGetMainHandWorldRot(PlayerCharacter* player, NiMatrix33& outRot)
		{
			const bool isLeftGameHand = (s_sessionActive && (s_sessionIsDummy || s_heldItemBaseFormId != 0))
				? s_mainHandIsLeft
				: IsMainHandLeftGameHand();
			return TryGetGameHandWorldRot(player, isLeftGameHand, outRot);
		}

		bool TryGetOffHandWorldPos(PlayerCharacter* player, NiPoint3& outPos)
		{
			return TryGetGameHandWorldPos(player, s_offHandIsLeft, outPos);
		}

		bool IsToolTouchingDoor(TESObjectREFR* doorRef, PlayerCharacter* player, const NiPoint3& toolPos,
			bool haveToolPos, bool& wasTouching, long long lastCollisionMs)
		{
			if (!doorRef || !haveToolPos)
				return false;

			const float slabDist = wasTouching
				? lockpickTouchDistance + kTouchExitSlack
				: lockpickTouchDistance;

			float planeDist = 9999.0f;
			const bool atSlab = IsPickAtDoorSlab(doorRef, toolPos, slabDist);
			const bool probeHit = IsPickTouchingDoor(doorRef, toolPos, player->pos, lockpickTouchDistance, wasTouching, planeDist);
			const bool nearDoor = IsPickNearDoorBounds(doorRef, toolPos, kDummyNearBoundsMargin);
			const bool collisionFresh = (NowMs() - lastCollisionMs) <= kCollisionFreshMs;

			return atSlab || probeHit || (nearDoor && (wasTouching || collisionFresh));
		}

		// Either game hand can start/maintain a hand-push session; locks to the active hand until contact breaks.
		bool EvaluateHandPushDoorContact(PlayerCharacter* player, TESObjectREFR* doorRef,
			NiPoint3& outHandPos, float& outPlaneDist, bool& outAtSlab, bool& outProbeHit)
		{
			auto tryHand = [&](bool isLeftGameHand) -> bool
			{
				NiPoint3 handPos;
				if (!TryGetGameHandWorldPos(player, isLeftGameHand, handPos))
					return false;

				const bool handWasTouching = s_touching && s_sessionPushHandActive && s_sessionPushHandIsLeft == isLeftGameHand;
				const float slabDist = handWasTouching
					? unlockedDoorTouchDistance + kTouchExitSlack
					: unlockedDoorTouchDistance;
				const bool atSlab = IsPickAtDoorSlab(doorRef, handPos, slabDist);

				float planeDist = 9999.0f;
				bool probeTouchState = handWasTouching;
				const bool probeHit = IsPickTouchingDoor(doorRef, handPos, player->pos,
					unlockedDoorTouchDistance, probeTouchState, planeDist);
				const bool nearDoor = IsPickNearDoorBounds(doorRef, handPos, kDummyNearBoundsMargin);
				const long long collisionMs = isLeftGameHand
					? s_lastLeftGameHandDoorCollisionMs.load()
					: s_lastRightGameHandDoorCollisionMs.load();
				const bool collisionFresh = (NowMs() - collisionMs) <= kCollisionFreshMs;
				const bool touching = atSlab || probeHit || (nearDoor && (handWasTouching || collisionFresh));

				if (!touching)
					return false;

				outHandPos = handPos;
				outPlaneDist = planeDist;
				outAtSlab = atSlab;
				outProbeHit = probeHit;

				if (!s_sessionPushHandActive)
				{
					s_sessionPushHandIsLeft = isLeftGameHand;
					s_sessionPushHandActive = true;
				}
				return true;
			};

			if (s_sessionPushHandActive)
			{
				if (!tryHand(s_sessionPushHandIsLeft))
					s_sessionPushHandActive = false;
				return s_sessionPushHandActive;
			}

			if (tryHand(true))
				return true;
			return tryHand(false);
		}

		void RemoveItemSilent(TESObjectREFR* target, TESForm* item, SInt32 count)
		{
			if (!target || !item)
				return;

			RemoveItem_Native(nullptr, 0, target, item, count, true, nullptr);
		}

		void DeleteWorldObject(TESObjectREFR* objRef)
		{
			if (!objRef)
				return;

			DeleteObject_Native((*g_skyrimVM)->GetClassRegistry(), 0, objRef);
		}

		void ReleaseSessionItemFromHiggs(TESObjectREFR* ref, bool isLeftVR)
		{
			if (!ref || !higgsInterface)
				return;

			if (higgsInterface->GetGrabbedObject(isLeftVR) == ref)
				higgsInterface->DisableHand(isLeftVR);
		}

		void DeleteSessionRefFromWorld(TESObjectREFR* ref, bool isLeftVR)
		{
			if (!ref)
				return;

			ReleaseSessionItemFromHiggs(ref, isLeftVR);
			DeleteWorldObject(ref);

			if (higgsInterface)
				higgsInterface->EnableHand(isLeftVR);
		}

		bool CallOriginalActivate(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly)
		{
			if (OriginalActivate)
				return OriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);

			LOG_ERR("Door lockpick: Activate hook has no trampoline, calling native directly");
			return Activate_Native(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
		}

		bool ActivateRef(TESObjectREFR* activatee, TESObjectREFR* activator)
		{
			if (!activatee || !activator)
				return false;

			// Our own activations go through the bypass flag so the detour
			// hook below lets them pass. Everything runs on the game thread.
			s_bypassActivate = true;
			const bool result = CallOriginalActivate(activatee, activator, 0, 0, 1, false);
			s_bypassActivate = false;
			return result;
		}

		// ---- TESObjectREFR::Activate detour (Fake Edge VR pattern) ----

		int AnalyzeFunctionPrologSize(unsigned char* funcStart)
		{
			int prologSize = 0;
			for (int i = 0; i < 20 && prologSize < 14; )
			{
				unsigned char b = funcStart[i];

				if (b >= 0x40 && b <= 0x4F)
				{
					unsigned char nextB = funcStart[i + 1];

					if (nextB >= 0x50 && nextB <= 0x57)
					{
						prologSize += 2;
						i += 2;
					}
					else if (nextB == 0x83)
					{
						prologSize += 4;
						i += 4;
					}
					else if (nextB == 0x81)
					{
						prologSize += 7;
						i += 7;
					}
					else if (nextB == 0x89 || nextB == 0x8B || nextB == 0x8D)
					{
						prologSize += 5;
						i += 5;
					}
					else
					{
						prologSize += 2;
						i += 2;
					}
				}
				else if (b >= 0x50 && b <= 0x57)
				{
					prologSize += 1;
					i += 1;
				}
				else
				{
					break;
				}
			}

			if (prologSize < 5)
				prologSize = 14;

			return prologSize;
		}

		bool InstallDetourHook(uintptr_t funcAddr, void* hookFunc, void** outTrampolineFunc, const char* hookName)
		{
			unsigned char* funcStart = (unsigned char*)funcAddr;
			int prologSize = AnalyzeFunctionPrologSize(funcStart);

			void* trampMem = g_localTrampoline.Allocate(prologSize + 14);
			if (!trampMem)
			{
				LOG_ERR("%s: failed to allocate trampoline memory", hookName);
				return false;
			}

			unsigned char* tramp = (unsigned char*)trampMem;
			memcpy(tramp, funcStart, prologSize);

			int offset = prologSize;
			tramp[offset++] = 0xFF;
			tramp[offset++] = 0x25;
			tramp[offset++] = 0x00;
			tramp[offset++] = 0x00;
			tramp[offset++] = 0x00;
			tramp[offset++] = 0x00;

			uintptr_t jumpBack = funcAddr + prologSize;
			memcpy(&tramp[offset], &jumpBack, 8);

			*outTrampolineFunc = trampMem;
			g_branchTrampoline.Write5Branch(funcAddr, (uintptr_t)hookFunc);
			return true;
		}

		// Blocks vanilla player activation of:
		//  - the held session item (lockpick / invisible dummy): pressing the
		//    activate button on it would add it to inventory mid-session. Only
		//    leaving the door vicinity may remove it.
		//  - LOAD doors (doors with teleport data): those must be opened with
		//    the physical push / lockpick mechanic, not the A button.
		// Our own code activates through ActivateRef, which sets the bypass.
		bool DoorActivateHook(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly);

		// Installs the Activate detour. Other plugins (Fake Edge VR) detour
		// the SAME function at kMessage_DataLoaded with a prolog-copying hook:
		// whoever installs SECOND would copy the first plugin's relative jmp
		// into its trampoline, where it points at garbage -> CTD on the first
		// door activation. So this runs lazily on the game thread at the
		// first crosshair event (long after every DataLoaded hook), and if
		// the function already starts with a jmp it CHAINS onto the existing
		// hook instead of copying bytes: our jmp replaces theirs, and calling
		// "the original" means calling their hook (which ends in the true
		// original code via their own, still-valid trampoline).
		void EnsureActivateHookInstalled()
		{
			static bool attempted = false;
			if (attempted)
				return;
			attempted = true;

			const uintptr_t funcAddr = Activate_Native.GetUIntPtr();
			unsigned char* funcStart = (unsigned char*)funcAddr;

			if (funcStart[0] == 0xE9)
			{
				const SInt32 rel = *(SInt32*)(funcStart + 1);
				const uintptr_t existingHook = funcAddr + 5 + (intptr_t)rel;
				OriginalActivate = (_TESObjectREFR_Activate)existingHook;
				g_branchTrampoline.Write5Branch(funcAddr, (uintptr_t)DoorActivateHook);
				LOG_INFO("Door lockpick: Activate already detoured (e.g. Fake Edge VR), chained onto existing hook at %p", (void*)existingHook);
				return;
			}

			void* trampoline = nullptr;
			if (InstallDetourHook(funcAddr, (void*)DoorActivateHook, &trampoline, "DoorActivateHook"))
			{
				OriginalActivate = (_TESObjectREFR_Activate)trampoline;
				LOG_INFO("Door lockpick: TESObjectREFR::Activate hook installed");
			}
		}

		bool DoorActivateHook(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly)
		{
			if (s_bypassActivate)
				return CallOriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);

			PlayerCharacter* player = *g_thePlayer;
			if (player && activator == player && activatee)
			{
				if (s_sessionActive && (activatee == s_spawnedLockpick || activatee == s_spawnedShiv))
				{
					LOG_INFO("Door lockpick: blocked activation of held session item %08X", activatee->formID);
					return false;
				}

				// Per-ref toggle: while this ref is open from our push, vanilla A
				// closes it and clears only this ref's state.
				if (doorLockpick != 0 && IsNonLoadDoorWithVanillaCloseToggle(activatee))
				{
					SyncDoorAwaitingVanillaCloseState(activatee);

					if (IsInteriorDoorAwaitingVanillaClose(activatee))
					{
						const bool wasOpen = IsDoorPhysicallyOpen(activatee);
						const bool result = CallOriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
						if (wasOpen)
						{
							ClearInteriorDoorAwaitingVanillaClose(activatee->formID);
							LOG_INFO("Door lockpick: door %08X closed via vanilla, push logic restored for this ref",
								activatee->formID);
						}
						return result;
					}
				}

				// Block the A button on unlocked doors that open through the
				// physical push mechanic (load doors, outdoor non-load doors,
				// and interior non-load doors not on ExcludeInteriorDoorBases).
				if (doorLockpick != 0 && higgsInterface
					&& IsUnlockedDoorSessionEligible(activatee)
					&& !IsVanillaDoorOnly(activatee))
				{
					PlayerCharacter* blockPlayer = *g_thePlayer;
					const float blockDist = blockPlayer
						? DistanceBetween(blockPlayer->pos, activatee->pos)
						: 0.0f;
					LogLoadDoorState(
						activatee,
						"BlockedActivate",
						blockDist,
						GetDoorSessionStartDistance(activatee),
						GetDoorSessionEndDistance(activatee));
					return false;
				}
			}

			return CallOriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
		}

		bool UnequipWeaponFromGameHand(PlayerCharacter* player, bool isLeftGameHand)
		{
			TESForm* weapon = player->GetEquippedObject(isLeftGameHand);
			if (!weapon || weapon->formType != kFormType_Weapon)
				return false;

			EquipManager* equipMan = EquipManager::GetSingleton();
			if (!equipMan)
				return false;

			BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
			CALL_MEMBER_FN(equipMan, UnequipItem)(player, weapon, nullptr, 1, slot, true, true, true, false, nullptr);
			return true;
		}

		void EquipWeaponToGameHand(PlayerCharacter* player, TESForm* weapon, bool isLeftGameHand)
		{
			if (!player || !weapon || weapon->formType != kFormType_Weapon)
				return;

			EquipManager* equipMan = EquipManager::GetSingleton();
			if (!equipMan)
				return;

			BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
			CALL_MEMBER_FN(equipMan, EquipItem)(player, weapon, nullptr, 1, slot, false, false, false, nullptr);
		}

		bool CacheAndClearMainHand(PlayerCharacter* player)
		{
			s_cachedHandFormId = 0;
			s_cachedHandIsSpell = false;
			s_mainHandIsLeft = IsMainHandLeftGameHand();

			SpellItem* spell = s_mainHandIsLeft ? player->leftHandSpell : player->rightHandSpell;
			if (spell)
			{
				s_cachedHandFormId = spell->formID;
				s_cachedHandIsSpell = true;
				UnequipSpellFromGameHand(player, s_mainHandIsLeft);
				return true;
			}

			TESForm* weapon = player->GetEquippedObject(s_mainHandIsLeft);
			if (weapon && weapon->formType == kFormType_Weapon)
			{
				s_cachedHandFormId = weapon->formID;
				s_cachedHandIsSpell = false;
				UnequipWeaponFromGameHand(player, s_mainHandIsLeft);
				return true;
			}

			return false;
		}

		bool CacheAndClearOffHand(PlayerCharacter* player)
		{
			s_cachedOffHandFormId = 0;
			s_cachedOffHandIsSpell = false;
			s_offHandIsLeft = !IsMainHandLeftGameHand();

			SpellItem* spell = s_offHandIsLeft ? player->leftHandSpell : player->rightHandSpell;
			if (spell)
			{
				s_cachedOffHandFormId = spell->formID;
				s_cachedOffHandIsSpell = true;
				UnequipSpellFromGameHand(player, s_offHandIsLeft);
				return true;
			}

			TESForm* weapon = player->GetEquippedObject(s_offHandIsLeft);
			if (weapon && weapon->formType == kFormType_Weapon)
			{
				s_cachedOffHandFormId = weapon->formID;
				s_cachedOffHandIsSpell = false;
				UnequipWeaponFromGameHand(player, s_offHandIsLeft);
				return true;
			}

			return false;
		}

		void ResetLockpickSessionReadyDelay()
		{
			s_lockpickSessionToolsGrabbed = false;
			s_lockpickSessionReadyTime = std::chrono::steady_clock::time_point{};
		}

		void ArmLockpickSessionReadyDelay()
		{
			s_lockpickSessionToolsGrabbed = true;
			if (lockpickSessionStartDelayMs <= 0)
				s_lockpickSessionReadyTime = std::chrono::steady_clock::time_point{};
			else
				s_lockpickSessionReadyTime = std::chrono::steady_clock::now()
					+ std::chrono::milliseconds(lockpickSessionStartDelayMs);
		}

		bool IsLockpickSessionReady()
		{
			if (!s_lockpickSessionToolsGrabbed)
				return false;

			return std::chrono::steady_clock::now() >= s_lockpickSessionReadyTime;
		}

		// Single-shot, runs on the game thread. Posted by a delay thread so the
		// session items' 3D has time to load first.
		class GrabSessionItemTask : public TaskDelegate
		{
		public:
			TESObjectREFR* m_item = nullptr;
			bool m_isLeftVRController = false;
			TESObjectREFR* m_expectedItem = nullptr;

			GrabSessionItemTask(TESObjectREFR* item, bool isLeftVRController, TESObjectREFR* expectedItem)
				: m_item(item)
				, m_isLeftVRController(isLeftVRController)
				, m_expectedItem(expectedItem)
			{
			}

			virtual void Run() override
			{
				if (!s_sessionActive || !m_item || m_item != m_expectedItem || !higgsInterface)
					return;

				if (higgsInterface->CanGrabObject(m_isLeftVRController))
				{
					higgsInterface->GrabObject(m_item, m_isLeftVRController);
					LOG_INFO("Door lockpick: HIGGS grabbed session item %08X in %s VR controller",
						m_item->formID, m_isLeftVRController ? "left" : "right");

					if (s_sessionIsKeyDoor && m_item == s_spawnedLockpick)
					{
						if (keyDoorGrabDelayMs <= 0)
							s_keyDoorGrabReadyTime = std::chrono::steady_clock::time_point{};
						else
							s_keyDoorGrabReadyTime = std::chrono::steady_clock::now()
								+ std::chrono::milliseconds(keyDoorGrabDelayMs);
						LOG_INFO("[KeyDoor] touch unlock armed in %d ms", keyDoorGrabDelayMs);
					}
				}
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		class GrabLockpickSessionTask : public TaskDelegate
		{
		public:
			TESObjectREFR* m_lockpick = nullptr;
			TESObjectREFR* m_shiv = nullptr;
			bool m_mainHandIsLeftVR = false;
			bool m_offHandIsLeftVR = false;
			bool m_armStartDelay = false;

			GrabLockpickSessionTask(TESObjectREFR* lockpick, TESObjectREFR* shiv,
				bool mainHandIsLeftVR, bool offHandIsLeftVR, bool armStartDelay = false)
				: m_lockpick(lockpick)
				, m_shiv(shiv)
				, m_mainHandIsLeftVR(mainHandIsLeftVR)
				, m_offHandIsLeftVR(offHandIsLeftVR)
				, m_armStartDelay(armStartDelay)
			{
			}

			virtual void Run() override
			{
				if (!s_sessionActive || !higgsInterface)
					return;

				if (m_lockpick && m_lockpick == s_spawnedLockpick
					&& higgsInterface->CanGrabObject(m_mainHandIsLeftVR))
				{
					higgsInterface->GrabObject(m_lockpick, m_mainHandIsLeftVR);
					LOG_INFO("Door lockpick: HIGGS grabbed lockpick in %s VR controller",
						m_mainHandIsLeftVR ? "left" : "right");
				}

				if (m_shiv && m_shiv == s_spawnedShiv
					&& higgsInterface->CanGrabObject(m_offHandIsLeftVR))
				{
					higgsInterface->GrabObject(m_shiv, m_offHandIsLeftVR);
					LOG_INFO("Door lockpick: HIGGS grabbed shiv in %s VR controller",
						m_offHandIsLeftVR ? "left" : "right");
				}

				if (m_armStartDelay)
				{
					ArmLockpickSessionReadyDelay();
					LOG_INFO("Door lockpick: lockpicking enabled in %d ms", lockpickSessionStartDelayMs);
				}
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		// HIGGS fires this when a hand releases an object. If it is our held
		// session item while the session is still active, the release was
		// accidental (grip slip): teleport it back to the hand and re-grab it
		// immediately, exactly like Fake Edge VR does for dropped weapons.
		// Deliberate removal (leaving the door vicinity) clears the session
		// state before releasing, so it never reaches the re-grab.
		void OnHiggsDropped(bool isLeft, TESObjectREFR* droppedRefr)
		{
			if (!s_sessionActive || !droppedRefr || !higgsInterface)
				return;

			const bool isLockpick = droppedRefr == s_spawnedLockpick;
			const bool isShiv = droppedRefr == s_spawnedShiv;
			if (!isLockpick && !isShiv)
				return;

			PlayerCharacter* player = *g_thePlayer;

			NiPoint3 handPos;
			if (player)
			{
				if (isLockpick)
					TryGetMainHandWorldPos(player, handPos);
				else
					TryGetOffHandWorldPos(player, handPos);

				droppedRefr->pos = handPos;
				NiNode* itemNode = droppedRefr->GetNiNode();
				if (itemNode)
					itemNode->m_worldTransform.pos = handPos;
			}

			if (higgsInterface->CanGrabObject(isLeft))
			{
				higgsInterface->GrabObject(droppedRefr, isLeft);
				LOG_INFO("Door lockpick: accidental drop, re-grabbed session item %08X",
					droppedRefr->formID);
			}
			else
			{
				TESObjectREFR* itemRef = droppedRefr;
				const bool isLeftVR = isLeft;
				TESObjectREFR* expectedItem = droppedRefr;
				std::thread([itemRef, isLeftVR, expectedItem]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (g_task)
						g_task->AddTask(new GrabSessionItemTask(itemRef, isLeftVR, expectedItem));
				}).detach();
				LOG_INFO("Door lockpick: accidental drop, re-grab scheduled for %08X",
					droppedRefr->formID);
			}
		}

		void EndLockpickSession();

		// Stashed (over the shoulder) / consumed (at the mouth): the ref no
		// longer exists, so the session cannot continue. Clear the dangling
		// pointer and end the session cleanly (restores the cached hand item).
		void OnHiggsStashedOrConsumed(TESForm* form)
		{
			if (!s_sessionActive || !form)
				return;

			if (form->formID != s_heldItemBaseFormId && form->formID != kShivFormId)
				return;

			LOG_INFO("Door lockpick: held session item stashed/consumed, ending session");
			s_spawnedLockpick = nullptr;
			s_spawnedShiv = nullptr;
			EndLockpickSession();
		}

		void OnHiggsStashed(bool isLeft, TESForm* stashedForm)
		{
			OnHiggsStashedOrConsumed(stashedForm);
		}

		void OnHiggsConsumed(bool isLeft, TESForm* consumedForm)
		{
			OnHiggsStashedOrConsumed(consumedForm);
		}

		// Single-shot, runs on the game thread. Posted by a delay thread.
		class RestoreHandTask : public TaskDelegate
		{
		public:
			UInt32 m_formId = 0;
			bool m_isSpell = false;
			bool m_isLeftGameHand = false;

			RestoreHandTask(UInt32 formId, bool isSpell, bool isLeftGameHand)
				: m_formId(formId)
				, m_isSpell(isSpell)
				, m_isLeftGameHand(isLeftGameHand)
			{
			}

			virtual void Run() override
			{
				if (m_formId == 0)
					return;

				PlayerCharacter* player = *g_thePlayer;
				TESForm* form = LookupFormByID(m_formId);
				if (!player || !form)
					return;

				if (m_isSpell)
				{
					SpellItem* spell = DYNAMIC_CAST(form, TESForm, SpellItem);
					if (!spell)
						return;

					SpellItem* held = m_isLeftGameHand ? player->leftHandSpell : player->rightHandSpell;
					if (held == spell)
						return;

					EquipSpellToGameHand(player, spell, m_isLeftGameHand);
					LOG_INFO("Door lockpick: re-equipped spell %08X to main hand", m_formId);
					return;
				}

				if (form->formType != kFormType_Weapon)
					return;

				if (player->GetEquippedObject(m_isLeftGameHand) == form)
					return;

				EquipWeaponToGameHand(player, form, m_isLeftGameHand);
				LOG_INFO("Door lockpick: re-equipped weapon %08X to main hand", m_formId);
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		void EndLockpickSession()
		{
			if (!s_sessionActive)
				return;

			// Clear the session state BEFORE releasing the item so the HIGGS
			// dropped callback sees no active session and does not re-grab
			// what we are deliberately removing.
			TESObjectREFR* heldItem = s_spawnedLockpick;
			TESObjectREFR* heldShiv = s_spawnedShiv;
			const bool wasDummy = s_sessionIsDummy;
			const bool wasHandPush = s_sessionIsHandPush;
			const bool wasKeyDoor = s_sessionIsKeyDoor;
			const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
			const bool offHandIsLeftVR = GameHandToVRController(s_offHandIsLeft);
			s_sessionActive = false;
			s_spawnedLockpick = nullptr;
			s_spawnedShiv = nullptr;
			s_heldItemBaseFormId = 0;

			PlayerCharacter* player = *g_thePlayer;
			if (player && heldItem)
			{
				if (wasDummy || (wasKeyDoor && s_keyDoorConsumedOnUnlock))
				{
					DeleteSessionRefFromWorld(heldItem, mainHandIsLeftVR);
					LOG_INFO("Door lockpick: %s deleted from world",
						wasKeyDoor ? "key" : "dummy item");
				}
				else
				{
					ReleaseSessionItemFromHiggs(heldItem, mainHandIsLeftVR);
					ActivateRef(heldItem, player);
					if (higgsInterface)
						higgsInterface->EnableHand(mainHandIsLeftVR);
					LOG_INFO("Door lockpick: activated spawned %s back into inventory",
						wasKeyDoor ? "key" : "lockpick");
				}
			}
			else if (wasHandPush)
			{
				LOG_INFO("Door lockpick: hand-push session ended");
			}

			if (heldShiv)
			{
				DeleteSessionRefFromWorld(heldShiv, offHandIsLeftVR);
				LOG_INFO("Door lockpick: shiv deleted from world");
			}

			if (s_cachedHandFormId != 0 && !s_suppressHandRestore)
			{
				// Small real-time delay so the lockpick pickup settles before the
				// weapon/spell is put back in the hand.
				const UInt32 formId = s_cachedHandFormId;
				const bool isSpell = s_cachedHandIsSpell;
				const bool isLeft = s_mainHandIsLeft;
				std::thread([formId, isSpell, isLeft]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
					if (g_task)
						g_task->AddTask(new RestoreHandTask(formId, isSpell, isLeft));
				}).detach();
			}

			if (s_cachedOffHandFormId != 0 && !s_suppressHandRestore)
			{
				const UInt32 formId = s_cachedOffHandFormId;
				const bool isSpell = s_cachedOffHandIsSpell;
				const bool isLeft = s_offHandIsLeft;
				std::thread([formId, isSpell, isLeft]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
					if (g_task)
						g_task->AddTask(new RestoreHandTask(formId, isSpell, isLeft));
				}).detach();
			}

			StopLockpickSessionSound();

			s_sessionActive = false;
			s_targetDoorFormId = 0;
			s_spawnedLockpick = nullptr;
			s_spawnedShiv = nullptr;
			s_sessionIsDummy = false;
			s_sessionIsHandPush = false;
			s_sessionPushHandIsLeft = false;
			s_sessionPushHandActive = false;
			s_sessionIsLockpick = false;
			s_sessionIsKeyDoor = false;
			s_keyDoorConsumedOnUnlock = false;
			s_keyDoorHandAtDoor = false;
			s_keyDoorGrabReadyTime = {};
			ResetKeyDoorTurnState();
			s_cachedHandFormId = 0;
			s_cachedHandIsSpell = false;
			s_cachedOffHandFormId = 0;
			s_cachedOffHandIsSpell = false;
			s_touching = false;
			s_pickAtDoor = false;
			s_shivAtDoor = false;
			s_unlockHapticPulsesSent = 0;
			s_lockpickRespawnPending = false;
			ResetLockpickSessionReadyDelay();
			s_sessionUnlockHoldMs = 3000;
			s_hasTouchAnchor = false;
			s_hasDoorThinAxis = false;
			s_hasDoorPushDir = false;
			s_lastSessionEndTime = std::chrono::steady_clock::now();
		}

		bool PlayerHasLockpick(PlayerCharacter* player, TESForm* lockpickForm)
		{
			ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
				return false;

			InventoryEntryData* entry = containerChanges->data->FindItemEntry(lockpickForm);
			return entry && entry->countDelta > 0;
		}

		bool SpawnAndGrabSessionLockpick(PlayerCharacter* player, TESForm* lockpickForm)
		{
			if (!player || !lockpickForm || !s_sessionActive || !s_sessionIsLockpick)
				return false;

			if (!PlayerHasLockpick(player, lockpickForm))
				return false;

			RemoveItemSilent(player, lockpickForm, 1);

			TESObjectREFR* itemRef = PlaceAtMe_Native(nullptr, 0, player, lockpickForm, 1, false, false);
			if (!itemRef)
			{
				LOG_ERR("Door lockpick: PlaceAtMe failed while spawning session lockpick");
				return false;
			}

			itemRef->pos = player->pos;
			itemRef->pos.z += 20.0f;
			s_spawnedLockpick = itemRef;

			const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
			const bool offHandIsLeftVR = GameHandToVRController(s_offHandIsLeft);
			TESObjectREFR* lockpickRef = itemRef;
			TESObjectREFR* shivRef = s_spawnedShiv;

			std::thread([lockpickRef, shivRef, mainHandIsLeftVR, offHandIsLeftVR]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (g_task)
					g_task->AddTask(new GrabLockpickSessionTask(lockpickRef, shivRef,
						mainHandIsLeftVR, offHandIsLeftVR));
			}).detach();

			return true;
		}

		void HandleLockpickBreak(TESObjectREFR* doorRef, PlayerCharacter* player, TESForm* lockpickForm)
		{
			if (!doorRef || !player || !lockpickForm || !s_sessionActive || !s_sessionIsLockpick)
				return;

			LOG_INFO("Door lockpick: lockpick broke on door %08X", doorRef->formID);

			StopLockpickSessionSound();
			s_touching = false;
			s_pickAtDoor = false;
			s_unlockHapticPulsesSent = 0;

			PlayLockpickBreakSound(doorRef);

			// 0 = no despawn/regrab — keep the held pick and only consume inventory.
			if (lockpickBreakRespawnDelayMs <= 0)
			{
				RemoveItemSilent(player, lockpickForm, 1);

				if (!PlayerHasLockpick(player, lockpickForm))
				{
					// Single message — a second notification in the same frame replaces the first.
					ShowLockpickAlertNotification("You broke your last lockpick");
					LOG_INFO("Door lockpick: no lockpicks left after break, ending session");
					EndLockpickSession();
				}
				else
				{
					ShowLockpickNotification("You broke a lockpick");
					LOG_INFO("Door lockpick: lockpick broke (held, inventory -1, no respawn)");
				}
				return;
			}

			s_lockpickRespawnPending = true;

			if (s_spawnedLockpick)
			{
				const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
				DeleteSessionRefFromWorld(s_spawnedLockpick, mainHandIsLeftVR);
				s_spawnedLockpick = nullptr;
			}

			// After HIGGS release / world delete so the HUD message is not cleared immediately.
			ShowLockpickNotification("You broke a lockpick");

			std::thread([lockpickForm]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(lockpickBreakRespawnDelayMs));
				if (!g_task)
					return;

				class RespawnLockpickTask : public TaskDelegate
				{
				public:
					TESForm* m_lockpickForm = nullptr;

					explicit RespawnLockpickTask(TESForm* lockpickForm)
						: m_lockpickForm(lockpickForm)
					{
					}

					virtual void Run() override
					{
						s_lockpickRespawnPending = false;

						if (!s_sessionActive || !s_sessionIsLockpick)
							return;

						PlayerCharacter* player = *g_thePlayer;
						if (!player || !m_lockpickForm)
							return;

						if (!PlayerHasLockpick(player, m_lockpickForm))
						{
							ShowLockpickAlertNotification("You have no more lockpicks in your inventory");
							LOG_INFO("Door lockpick: no lockpicks left after break, ending session");
							EndLockpickSession();
							return;
						}

						if (!SpawnAndGrabSessionLockpick(player, m_lockpickForm))
						{
							ShowLockpickAlertNotification("You have no more lockpicks in your inventory");
							LOG_INFO("Door lockpick: failed to respawn lockpick after break, ending session");
							EndLockpickSession();
							return;
						}

						LOG_INFO("Door lockpick: respawned lockpick after break");
					}

					virtual void Dispose() override
					{
						delete this;
					}
				};

				g_task->AddTask(new RespawnLockpickTask(lockpickForm));
			}).detach();
		}

		// Single-shot, runs on the game thread. Activates the door after the
		// released pick / deleted dummy has had time to actually leave the
		// hand and the world (an activation fired while the held item is
		// still settling gets silently eaten).
		class ActivateDoorTask : public TaskDelegate
		{
		public:
			UInt32 m_doorFormId = 0;

			explicit ActivateDoorTask(UInt32 doorFormId)
				: m_doorFormId(doorFormId)
			{
			}

			virtual void Run() override
			{
				PlayerCharacter* player = *g_thePlayer;
				TESForm* doorForm = LookupFormByID(m_doorFormId);
				TESObjectREFR* doorRef = doorForm ? DYNAMIC_CAST(doorForm, TESForm, TESObjectREFR) : nullptr;
				if (!player || !doorRef)
				{
					s_suppressHandRestore = false;
					return;
				}

				const bool loadDoor = IsLoadDoor(doorRef);
				if (loadDoor)
					ArmLoadDoorTransitionCooldown();

				const bool activated = ActivateRef(doorRef, player);
				if (loadDoor)
				{
					LOG_INFO("Door lockpick: load door %08X activated via push (result=%d, base=%08X)",
						m_doorFormId, activated ? 1 : 0,
						doorRef->baseForm ? doorRef->baseForm->formID : 0);

					s_suppressHandRestore = false;

					if (s_deferredHandRestoreFormId != 0)
					{
						const UInt32 formId = s_deferredHandRestoreFormId;
						const bool isSpell = s_deferredHandRestoreIsSpell;
						const bool isLeft = s_deferredHandRestoreIsLeft;
						s_deferredHandRestoreFormId = 0;
						std::thread([formId, isSpell, isLeft]()
						{
							std::this_thread::sleep_for(std::chrono::milliseconds(500));
							if (g_task)
								g_task->AddTask(new RestoreHandTask(formId, isSpell, isLeft));
						}).detach();
					}
				}
				else if (IsNonLoadDoorWithVanillaCloseToggle(doorRef) && IsDoorPhysicallyOpen(doorRef))
				{
					MarkInteriorDoorAwaitingVanillaClose(m_doorFormId);
					LOG_INFO("Door lockpick: door %08X opened via push, vanilla close until toggled (this ref only)",
						m_doorFormId);
				}
				else
				{
					LOG_INFO("Door lockpick: door %08X activated", m_doorFormId);
				}
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		// Pick+shiv held against the door long enough: unlock directly by clearing
		// REFR_LOCK::kFlag_Locked (same field CommonLib IsLocked() reads).
		bool IsKeyDoorSessionReady()
		{
			if (!s_sessionIsKeyDoor)
				return false;

			return std::chrono::steady_clock::now() >= s_keyDoorGrabReadyTime;
		}

		void TriggerKeyDoorUnlock(TESObjectREFR* doorRef)
		{
			if (!doorRef)
				return;

			LOG_INFO("[KeyDoor] key turned %.1f deg on door %08X, unlocking",
				s_keyDoorTurnPeakDeg, doorRef->formID);

			if (!UnlockRef(doorRef))
			{
				LOG_INFO("[KeyDoor] door %08X has no lock data", doorRef->formID);
			}
			else
			{
				LOG_INFO("[KeyDoor] door %08X unlocked", doorRef->formID);
				PlayUnlockSuccessSound(doorRef);
				PlayUnlockSuccessHaptics(s_mainHandIsLeft, s_offHandIsLeft);
			}

			// Keep the key: EndLockpickSession returns the world ref to inventory
			// (same path as walking away). Do not consume on unlock.
			s_keyDoorConsumedOnUnlock = false;
			EndLockpickSession();
		}

		void TriggerDoorUnlock(TESObjectREFR* doorRef)
		{
			if (!doorRef)
				return;

			LOG_INFO("Door lockpick: pick+shiv held against door %08X for %d ms, unlocking",
				doorRef->formID, s_sessionUnlockHoldMs);

			const SInt32 lockLevelBeforeUnlock = GetRefLockLevel(doorRef);

			if (!UnlockRef(doorRef))
			{
				LOG_INFO("Door lockpick: door %08X has no lock data", doorRef->formID);
				StopLockpickSessionSound();
			}
			else
			{
				LOG_INFO("Door lockpick: door %08X unlocked", doorRef->formID);
				PlayerCharacter* player = *g_thePlayer;
				if (player)
				{
					if (IsExcludedFromLockpickCrime(doorRef))
					{
						LOG_INFO("[Crime] Door %08X excluded from lockpick crime/bounty", doorRef->formID);
					}
					else
					{
						ApplyLockpickBountyIfWitnessed(player, doorRef);
					}
					ApplyLockpickSkillXpOnUnlock(player, doorRef, lockLevelBeforeUnlock);
				}
				const bool mainHandIsLeft = s_mainHandIsLeft;
				const bool offHandIsLeft = s_offHandIsLeft;
				StopLockpickSessionSound();
				PlayUnlockSuccessSound(doorRef);
				PlayUnlockSuccessHaptics(mainHandIsLeft, offHandIsLeft);
			}

			EndLockpickSession();
		}

		// Single-shot check that runs ON THE GAME THREAD. Never re-queues itself
		// (this SKSE build drains the task queue until empty within one frame,
		// so a self-requeuing task hard-freezes the game). The heartbeat thread
		// below posts a fresh instance periodically while a session is active.
		class SessionCheckTask : public TaskDelegate
		{
		public:
			virtual void Run() override
			{
				if (!s_sessionActive)
					return;

				PlayerCharacter* player = *g_thePlayer;
				if (!player)
				{
					EndLockpickSession();
					return;
				}

				TESForm* doorForm = LookupFormByID(s_targetDoorFormId);
				TESObjectREFR* doorRef = doorForm ? DYNAMIC_CAST(doorForm, TESForm, TESObjectREFR) : nullptr;

				if (!doorRef)
				{
					LOG_INFO("Door lockpick: target door ref gone, ending session");
					EndLockpickSession();
					return;
				}

				if (DistanceBetween(player->pos, doorRef->pos) > GetDoorSessionEndDistance(doorRef))
				{
					LOG_INFO("Door lockpick: player moved away from door, ending session");
					EndLockpickSession();
					return;
				}

				if (s_sessionIsDummy || s_sessionIsHandPush)
				{
					// Push-to-open: contact uses the grabbed dummy's position,
					// or either hand bone when UnlockedDoorSpawnDummy = 0.
					NiPoint3 contactPos;
					bool haveContactPos = false;
					float planeDist = 9999.0f;
					bool atSlab = false;
					bool probeHit = false;
					bool touching = false;

					if (s_sessionIsHandPush)
					{
						haveContactPos = EvaluateHandPushDoorContact(player, doorRef,
							contactPos, planeDist, atSlab, probeHit);
						touching = haveContactPos;
					}
					else
					{
						haveContactPos = TryGetLockpickWorldPos(contactPos);

						const float slabDist = s_touching
							? unlockedDoorTouchDistance + kTouchExitSlack
							: unlockedDoorTouchDistance;
						atSlab = haveContactPos && IsPickAtDoorSlab(doorRef, contactPos, slabDist);

						const bool nearDoor = haveContactPos
							&& IsPickNearDoorBounds(doorRef, contactPos, kDummyNearBoundsMargin);
						const bool collisionFresh = (NowMs() - s_lastDummyCollisionMs.load()) <= kCollisionFreshMs;

						touching = atSlab || (nearDoor && (s_touching || collisionFresh));
					}

					// Push sessions end when the player is no longer in front
					// of the door - but never while actively pressed against it.
					// Pull sessions also stay alive while a pull-away gesture is
					// in progress after the initial touch (contact breaks as the
					// hand moves away from the slab).
					const bool pullGestureInProgress = s_sessionOpenViaPull && s_touching && s_hasTouchAnchor;
					if (!touching && !pullGestureInProgress && !IsPlayerStillInFrontOfDoor(player, doorRef))
					{
						LOG_INFO("Door lockpick: player no longer in front of door, ending %s session",
							s_sessionIsHandPush ? "hand-push" : "dummy");
						EndLockpickSession();
						return;
					}

					if (!touching && !pullGestureInProgress)
					{
						s_touching = false;
						s_hasTouchAnchor = false;
						if (s_sessionIsHandPush)
							s_sessionPushHandActive = false;
						return;
					}

					if (!IsDoorUnlocked(doorRef))
					{
						LOG_INFO("Door lockpick: door became locked, ending %s session",
							s_sessionIsHandPush ? "hand-push" : "dummy");
						EndLockpickSession();
						return;
					}

					NiPoint3 handPos;
					const bool haveHandPos = s_sessionIsHandPush
						? (s_sessionPushHandActive
							&& TryGetGameHandWorldPos(player, s_sessionPushHandIsLeft, handPos))
						: TryGetMainHandWorldPos(player, handPos);

					if (!s_touching)
					{
						s_touching = true;
						s_touchStartTime = std::chrono::steady_clock::now();
						s_hasTouchAnchor = haveHandPos;
						if (haveHandPos)
							s_touchAnchorHandPos = handPos;
						if (s_sessionIsHandPush)
						{
							LOG_INFO("Door lockpick: hand contact with door %08X (%s, planeDist=%.1f%s)",
								doorRef->formID,
								atSlab ? "slab" : (probeHit ? "probe" : "HIGGS collision"),
								planeDist,
								s_sessionOpenViaPull ? ", pull away to open" : "");
						}
						else
						{
							LOG_INFO("Door lockpick: dummy contact with door %08X (%s)",
								doorRef->formID, atSlab ? "slab" : "HIGGS collision");
						}
						return;
					}

					if (!haveHandPos)
						return;

					if (!s_hasTouchAnchor)
					{
						s_touchAnchorHandPos = handPos;
						s_hasTouchAnchor = true;
						return;
					}

					// Open gesture = hand displacement since contact began,
					// projected onto the session-start door-to-player axis.
					// Front mesh face -> push (hand toward door). Back face ->
					// pull (hand toward player).
					NiPoint3 openAxis = s_sessionToPlayerDir;
					if (!s_hasSessionToPlayerDir)
					{
						openAxis = player->pos - GetDoorGeometryAnchor(doorRef);
						if (!NormalizeHorizontalDir(openAxis))
							return;
					}

					const NiPoint3 handMove = handPos - s_touchAnchorHandPos;
					const float towardPlayer = handMove.x * openAxis.x + handMove.y * openAxis.y + handMove.z * openAxis.z;
					const float openGestureDist = s_sessionOpenViaPull ? towardPlayer : -towardPlayer;

					if (openGestureDist < 0.0f)
					{
						s_touchAnchorHandPos = handPos;
						return;
					}

					if (openGestureDist >= unlockedDoorPushDistance)
					{
						if (!IsPlayerFacingDoor(player, doorRef))
							return;

						LOG_INFO("Door lockpick: %s detected (%.1f units, %s face), activating unlocked door %08X",
							s_sessionOpenViaPull ? "pull" : "push",
							openGestureDist,
							s_sessionOpenViaPull ? "back" : "front",
							doorRef->formID);

						PulseDoorOpenGestureHaptic(s_sessionIsHandPush ? s_sessionPushHandIsLeft : s_mainHandIsLeft);

						const UInt32 doorFormId = doorRef->formID;
						const bool handPush = s_sessionIsHandPush;
						const bool loadDoor = IsLoadDoor(doorRef);

						if (loadDoor)
						{
							if (s_cachedHandFormId != 0)
							{
								s_deferredHandRestoreFormId = s_cachedHandFormId;
								s_deferredHandRestoreIsSpell = s_cachedHandIsSpell;
								s_deferredHandRestoreIsLeft = s_mainHandIsLeft;
							}
							s_suppressHandRestore = true;
							ArmLoadDoorTransitionCooldown();
						}

						EndLockpickSession();

						if (loadDoor)
						{
							std::thread([doorFormId]()
							{
								std::this_thread::sleep_for(std::chrono::milliseconds(kLoadDoorActivateDelayMs));
								if (g_task)
									g_task->AddTask(new ActivateDoorTask(doorFormId));
							}).detach();
						}
						else if (handPush)
						{
							if (g_task)
								g_task->AddTask(new ActivateDoorTask(doorFormId));
						}
						else
						{
							// The grabbed dummy must be fully gone from the world
							// before the door is activated, or the activation gets
							// eaten.
							std::thread([doorFormId]()
							{
								std::this_thread::sleep_for(std::chrono::milliseconds(200));
								if (g_task)
									g_task->AddTask(new ActivateDoorTask(doorFormId));
							}).detach();
						}
					}
					return;
				}

				if (s_sessionIsKeyDoor)
				{
					if (!IsDoorLockedAndRequiresKey(doorRef))
					{
						LOG_INFO("[KeyDoor] door %08X unlocked, ending key session", doorRef->formID);
						EndLockpickSession();
						return;
					}

					if (!IsPlayerStillInFrontOfDoor(player, doorRef))
					{
						LOG_INFO("[KeyDoor] player no longer in front of key door, ending session");
						EndLockpickSession();
						return;
					}

					if (!IsKeyDoorSessionReady())
						return;

					NiPoint3 keyPos;
					NiPoint3 handPos;
					const bool haveKeyPos = TryGetLockpickWorldPos(keyPos);
					const bool haveHandPos = TryGetMainHandWorldPos(player, handPos);

					const bool keyTouching = haveKeyPos && IsToolTouchingDoor(doorRef, player, keyPos, haveKeyPos,
						s_pickAtDoor, s_lastPickCollisionMs.load());
					const bool handTouching = haveHandPos && IsToolTouchingDoor(doorRef, player, handPos, haveHandPos,
						s_keyDoorHandAtDoor, s_lastPickCollisionMs.load());
					const bool touching = keyTouching || handTouching;

					if (!touching)
					{
						ResetKeyDoorTurnState();
						return;
					}

					NiMatrix33 handRot;
					if (!TryGetKeyDoorHandRot(player, handRot))
						return;

					const NiPoint3 axisPos = haveHandPos ? handPos : (haveKeyPos ? keyPos : player->pos);

					if (!s_keyDoorTurnBaselineValid)
					{
						BeginKeyDoorWristTwist(handRot, axisPos, doorRef);
						LOG_INFO("[KeyDoor] wrist twist started on door %08X (need %.0f deg either way)",
							doorRef->formID, keyDoorTurnDegrees);
					}

					const float twistDeg = GetWristTwistDegrees(handRot);
					const float absTwist = std::fabs(twistDeg);
					if (absTwist > s_keyDoorTurnPeakDeg)
						s_keyDoorTurnPeakDeg = absTwist;

					if (s_keyDoorTurnPeakDeg >= keyDoorTurnDegrees)
						TriggerKeyDoorUnlock(doorRef);

					return;
				}

				if (!IsDoorLockedAndPickable(doorRef))
				{
					LOG_INFO("Door lockpick: door no longer locked, ending session");
					EndLockpickSession();
					return;
				}

				if (!s_sessionIsLockpick || !s_spawnedShiv)
					return;

				if (s_lockpickRespawnPending)
					return;

				if (!IsLockpickSessionReady())
					return;

				if (!s_spawnedLockpick)
					return;

				TESForm* lockpickForm = LookupFormByID(kLockpickFormId);
				const SInt32 doorLockLevel = GetRefLockLevel(doorRef);

				// Touch-to-unlock: lockpick and shiv both at the door, close
				// together, for s_sessionUnlockHoldMs clears the door lock flag.
				NiPoint3 pickPos;
				NiPoint3 shivPos;
				const bool havePickPos = TryGetLockpickWorldPos(pickPos);
				const bool haveShivPos = TryGetShivWorldPos(shivPos);

				const bool pickTouching = IsToolTouchingDoor(doorRef, player, pickPos, havePickPos,
					s_pickAtDoor, s_lastPickCollisionMs.load());
				const bool shivTouching = IsToolTouchingDoor(doorRef, player, shivPos, haveShivPos,
					s_shivAtDoor, s_lastShivCollisionMs.load());

				s_pickAtDoor = pickTouching;
				s_shivAtDoor = shivTouching;

				const float toolSeparation = (havePickPos && haveShivPos)
					? DistanceBetween(pickPos, shivPos)
					: 9999.0f;
				const bool toolsClose = toolSeparation <= lockpickShivMaxDistance;
				const bool lockpicking = pickTouching && shivTouching && toolsClose;

				if (lockpicking)
				{
					const auto now = std::chrono::steady_clock::now();
					if (!s_touching)
					{
						if (lockpickForm && RollLockpickBreak(player, doorLockLevel))
						{
							HandleLockpickBreak(doorRef, player, lockpickForm);
							return;
						}

						s_touching = true;
						s_touchStartTime = now;
						s_lastBreakRollTime = now;
						s_unlockHapticPulsesSent = 0;
						StartLockpickSessionSound(doorRef);
						LOG_INFO("Door lockpick: pick+shiv at door %08X (sep=%.1f, tier=%s, hold=%d ms)",
							doorRef->formID, toolSeparation,
							GetLockTierName(GetRefLockLevel(doorRef)), s_sessionUnlockHoldMs);
					}
					else
					{
						// Optional mid-hold breaks: re-roll on a fixed cadence
						// during the countdown instead of only at first contact.
						if (lockpickBreakDuringHold != 0 && lockpickForm)
						{
							const auto sinceLastRollMs = std::chrono::duration_cast<std::chrono::milliseconds>(
								now - s_lastBreakRollTime).count();
							if (sinceLastRollMs >= lockpickBreakRollIntervalMs)
							{
								s_lastBreakRollTime = now;
								if (RollLockpickBreak(player, doorLockLevel))
								{
									HandleLockpickBreak(doorRef, player, lockpickForm);
									return;
								}
							}
						}

						const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
							now - s_touchStartTime).count();

						const int elapsedSeconds = static_cast<int>(heldMs / 1000);
						while (s_unlockHapticPulsesSent < elapsedSeconds)
						{
							PulseLockpickSessionHaptics();
							++s_unlockHapticPulsesSent;
						}

						if (heldMs >= s_sessionUnlockHoldMs)
						{
							TriggerDoorUnlock(doorRef);
							return;
						}
					}
				}
				else
				{
					if (s_touching)
					{
						LOG_INFO("Door lockpick: unlock timer reset on door %08X", doorRef->formID);
					}
					StopLockpickSessionSound();
					s_touching = false;
					s_unlockHapticPulsesSent = 0;
					if (!pickTouching)
						s_pickAtDoor = false;
					if (!shivTouching)
						s_shivAtDoor = false;
				}
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		bool IsPlayerWithinSessionStartDistance(TESObjectREFR* door)
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!door || !player)
				return false;

			return DistanceBetween(player->pos, door->pos) <= GetDoorSessionStartDistance(door);
		}

		void TryLogKeyRequiredDoorInFront(TESObjectREFR* doorRef)
		{
			if (keyDoorActions == 0)
				return;

			if (!doorRef || !IsDoorLockedAndRequiresKey(doorRef))
				return;

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
				return;

			if (!IsPlayerWithinSessionStartDistance(doorRef))
			{
				if (s_keyDoorLoggedFormId == doorRef->formID)
					s_keyDoorLoggedFormId = 0;
				return;
			}

			if (!IsPlayerFacingDoor(player, doorRef))
			{
				if (s_keyDoorLoggedFormId == doorRef->formID)
					s_keyDoorLoggedFormId = 0;
				return;
			}

			if (s_keyDoorLoggedFormId == doorRef->formID)
				return;

			s_keyDoorLoggedFormId = doorRef->formID;
			LogKeyRequiredDoor(doorRef, player);

			TESForm* keyForm = GetDoorRequiredKeyForm(doorRef);
			if (!keyForm || !PlayerHasItem(player, keyForm))
			{
				ShowLockpickAlertNotification(
					"This Door can not be lockpicked and requires a key you do not have yet");
			}
		}

		// Logs once when the player is near a load door (crosshair or cell scan).
		void TryLogCaveEntranceInFront(TESObjectREFR* doorRef)
		{
			if (!doorRef || !IsLoadDoor(doorRef))
				return;

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
				return;

			const float dist = DistanceBetween(player->pos, doorRef->pos);
			const float startDist = GetLoadDoorApproachDistance(doorRef);
			const float endDist = GetDoorSessionEndDistance(doorRef);

			if (!IsPlayerInLoadDoorApproachRange(player, doorRef))
			{
				if (s_caveEntranceLoggedFormId == doorRef->formID)
					s_caveEntranceLoggedFormId = 0;
				return;
			}

			if (!IsPlayerInFrontOfLoadDoor(player, doorRef))
			{
				if (s_caveEntranceLoggedFormId == doorRef->formID)
					s_caveEntranceLoggedFormId = 0;
				return;
			}

			if (s_caveEntranceLoggedFormId == doorRef->formID)
				return;

			s_caveEntranceLoggedFormId = doorRef->formID;
			LogLoadDoorState(doorRef, "CaveEntrance", dist, startDist, endDist);
		}

		void StartLockpickSession(TESObjectREFR* door, bool isDummy);

		void CaptureDoorSessionGeometry(TESObjectREFR* door, const NiPoint3& playerPos);
		void StartMonitorThreadOnce();

		void StartKeyDoorSession(TESObjectREFR* door)
		{
			if (keyDoorActions == 0)
				return;

			if (!door || s_sessionActive || doorLockpick == 0)
				return;

			if (!IsDoorLockedAndRequiresKey(door))
				return;

			TESForm* keyForm = GetDoorRequiredKeyForm(door);
			if (!keyForm)
			{
				LOG_INFO("[KeyDoor] door %08X requires a key but none is set on the lock", door->formID);
				return;
			}

			PlayerCharacter* player = *g_thePlayer;
			if (!player || !PlayerHasItem(player, keyForm))
				return;

			if (!IsPlayerWithinSessionStartDistance(door))
				return;

			if (IsRestartCooldownActive())
				return;

			if (!higgsInterface)
			{
				LOG_INFO("[KeyDoor] HIGGS not available, skipping key session");
				return;
			}

			if (!s_higgsCollisionCallbackRegistered)
			{
				s_higgsCollisionCallbackRegistered = true;
				higgsInterface->AddCollisionCallback(OnHiggsCollision);
				LOG_INFO("Door lockpick: HIGGS collision callback registered");
			}

			if (!s_higgsDropCallbacksRegistered)
			{
				s_higgsDropCallbacksRegistered = true;
				higgsInterface->AddDroppedCallback(OnHiggsDropped);
				higgsInterface->AddStashedCallback(OnHiggsStashed);
				higgsInterface->AddConsumedCallback(OnHiggsConsumed);
				LOG_INFO("Door lockpick: HIGGS drop-protection callbacks registered");
			}

			CacheAndClearMainHand(player);
			RemoveItemSilent(player, keyForm, 1);

			TESObjectREFR* itemRef = PlaceAtMe_Native(nullptr, 0, player, keyForm, 1, false, false);
			if (!itemRef)
			{
				LOG_ERR("[KeyDoor] PlaceAtMe failed for key %08X", keyForm->formID);
				return;
			}

			itemRef->pos = player->pos;
			itemRef->pos.z += 20.0f;

			s_sessionActive = true;
			s_targetDoorFormId = door->formID;
			s_spawnedLockpick = itemRef;
			s_spawnedShiv = nullptr;
			s_heldItemBaseFormId = keyForm->formID;
			s_sessionIsDummy = false;
			s_sessionIsHandPush = false;
			s_sessionPushHandIsLeft = false;
			s_sessionPushHandActive = false;
			s_sessionIsLockpick = false;
			s_sessionIsKeyDoor = false;
			s_keyDoorConsumedOnUnlock = false;
			s_keyDoorHandAtDoor = false;
			s_keyDoorGrabReadyTime = {};
			s_sessionIsKeyDoor = true;
			s_keyDoorConsumedOnUnlock = false;
			s_keyDoorHandAtDoor = false;
			s_keyDoorGrabReadyTime = (std::chrono::steady_clock::time_point::max)();
			ResetKeyDoorTurnState();
			s_touching = false;
			s_pickAtDoor = false;
			s_keyDoorLoggedFormId = door->formID;

			CaptureDoorSessionGeometry(door, player->pos);

			const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
			std::thread([itemRef, mainHandIsLeftVR]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (g_task)
					g_task->AddTask(new GrabSessionItemTask(itemRef, mainHandIsLeftVR, itemRef));
			}).detach();

			StartMonitorThreadOnce();

			LOG_INFO("[KeyDoor] key session started for door %08X with key %08X (%s)",
				door->formID, keyForm->formID, GetFormLogName(keyForm));
		}

		void HandleKeyDoorInFront(TESObjectREFR* doorRef)
		{
			if (keyDoorActions == 0)
				return;

			if (!doorRef || !IsDoorLockedAndRequiresKey(doorRef) || s_sessionActive)
				return;

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
				return;

			if (!IsPlayerWithinSessionStartDistance(doorRef))
			{
				if (s_keyDoorLoggedFormId == doorRef->formID)
					s_keyDoorLoggedFormId = 0;
				return;
			}

			if (!IsPlayerFacingDoor(player, doorRef))
			{
				if (s_keyDoorLoggedFormId == doorRef->formID)
					s_keyDoorLoggedFormId = 0;
				return;
			}

			TESForm* keyForm = GetDoorRequiredKeyForm(doorRef);
			if (keyForm && PlayerHasItem(player, keyForm))
			{
				StartKeyDoorSession(doorRef);
				return;
			}

			TryLogKeyRequiredDoorInFront(doorRef);
		}

		void CaptureDoorSessionGeometry(TESObjectREFR* door, const NiPoint3& playerPos)
		{
			const NiPoint3 doorAnchor = GetDoorGeometryAnchor(door);

			s_hasDoorThinAxis = ComputeDoorThinAxis(door, playerPos, s_doorThinAxis);
			s_hasDoorMeshFrontNormal = s_hasDoorThinAxis;
			s_doorMeshFrontNormal = s_doorThinAxis;

			s_hasSessionToPlayerDir = false;
			s_sessionOpenViaPull = false;
			NiPoint3 toPlayerDir = playerPos - doorAnchor;
			if (NormalizeHorizontalDir(toPlayerDir))
			{
				s_sessionToPlayerDir = toPlayerDir;
				s_hasSessionToPlayerDir = true;

				if (s_hasDoorMeshFrontNormal)
				{
					const bool onFrontFace = IsPlayerOnDoorFrontFace(
						playerPos, doorAnchor, s_doorMeshFrontNormal);
					s_sessionOpenViaPull = !onFrontFace;
				}
			}

			// Load doors (cell transitions) always push unless listed in
			// PullLoadDoorRefs in the INI.
			if (IsLoadDoor(door) && !IsPullLoadDoorRef(door))
				s_sessionOpenViaPull = false;

			s_hasDoorPushDir = false;
			NiPoint3 toDoorDir = doorAnchor - playerPos;
			if (NormalizeHorizontalDir(toDoorDir))
			{
				if (s_hasDoorThinAxis)
				{
					const float align = s_doorThinAxis.x * toDoorDir.x + s_doorThinAxis.y * toDoorDir.y;
					s_doorPushDir = s_doorThinAxis;
					if (align < 0.0f)
					{
						s_doorPushDir.x = -s_doorPushDir.x;
						s_doorPushDir.y = -s_doorPushDir.y;
						s_doorPushDir.z = -s_doorPushDir.z;
					}
				}
				else
				{
					s_doorPushDir = toDoorDir;
				}
				s_hasDoorPushDir = true;
			}
		}

		// Single-shot poll: when the crosshair is on a key-required door, log
		// once the player is within session-start range and facing the door.
		class KeyDoorLogPollTask : public TaskDelegate
		{
		public:
			virtual void Run() override
			{
				if (s_crosshairKeyDoorFormId == 0)
					return;

				TESForm* doorForm = LookupFormByID(s_crosshairKeyDoorFormId);
				TESObjectREFR* doorRef = doorForm ? DYNAMIC_CAST(doorForm, TESForm, TESObjectREFR) : nullptr;
				if (!doorRef || !IsDoorLockedAndRequiresKey(doorRef))
				{
					s_keyDoorLoggedFormId = 0;
					return;
				}

				HandleKeyDoorInFront(doorRef);
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		// Diagnostic: log once when the player walks up to a cave load door and
		// faces it (same distance / facing checks as session start).
		class CaveEntranceLogPollTask : public TaskDelegate
		{
		public:
			virtual void Run() override
			{
				TESObjectREFR* doorRef = ResolveLoadDoorForEntranceLogging();
				if (!doorRef)
				{
					s_caveEntranceLoggedFormId = 0;
					return;
				}

				TryLogCaveEntranceInFront(doorRef);
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		// Single-shot poll: crosshair can lock onto a door from far away but
		// SKSE only fires the crosshair event on change, so we re-check every
		// tick until the player walks within DoorSessionStartDistance.
		class SessionStartPollTask : public TaskDelegate
		{
		public:
			virtual void Run() override
			{
				if (s_sessionActive || doorLockpick == 0 || s_crosshairDoorFormId == 0)
					return;

				if (IsLoadDoorTransitionCooldownActive())
					return;

				if (IsRestartCooldownActive())
					return;

				TESForm* doorForm = LookupFormByID(s_crosshairDoorFormId);
				TESObjectREFR* doorRef = doorForm ? DYNAMIC_CAST(doorForm, TESForm, TESObjectREFR) : nullptr;
				if (!doorRef || !IsDoor(doorRef))
					return;

				if (IsVanillaDoorOnly(doorRef))
					return;

				SyncDoorAwaitingVanillaCloseState(doorRef);

				if (!IsPlayerWithinSessionStartDistance(doorRef))
					return;

				if (IsDoorLockedAndRequiresKey(doorRef))
				{
					if (keyDoorActions != 0)
						HandleKeyDoorInFront(doorRef);
					return;
				}

				if (IsDoorLockedAndPickable(doorRef))
					StartLockpickSession(doorRef, false);
				else if (IsUnlockedDoorSessionEligible(doorRef))
					StartLockpickSession(doorRef, true);
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		// Background heartbeat: posts one game-thread check per tick while a
		// session is active, or polls for session start while the crosshair
		// is on a door but the player is still walking into range.
		void StartMonitorThreadOnce()
		{
			if (s_monitorThreadStarted)
				return;

			s_monitorThreadStarted = true;

			std::thread([]()
			{
				while (true)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(kMonitorIntervalMs));

					if (!g_task)
						continue;

					if (s_sessionActive.load())
						g_task->AddTask(new SessionCheckTask());
					else
					{
						if (doorLockpick != 0 && s_crosshairDoorFormId != 0)
							g_task->AddTask(new SessionStartPollTask());
						if (keyDoorActions != 0 && s_crosshairKeyDoorFormId != 0)
							g_task->AddTask(new KeyDoorLogPollTask());
						g_task->AddTask(new CaveEntranceLogPollTask());
					}
				}
			}).detach();
		}

		// Starts a session: locked door -> real lockpick, unlocked door ->
		// invisible dummy (optional) or main-hand push geometry.
		void StartLockpickSession(TESObjectREFR* door, bool isDummy)
		{
			if (!door || s_sessionActive || doorLockpick == 0)
				return;

			if (IsLoadDoorTransitionCooldownActive())
				return;

			if (IsVanillaDoorOnly(door))
				return;

			if (!IsPlayerWithinSessionStartDistance(door))
				return;

			if (IsRestartCooldownActive())
				return;

			if (!higgsInterface)
			{
				LOG_INFO("Door lockpick: HIGGS not available, skipping");
				return;
			}

			if (!s_higgsCollisionCallbackRegistered)
			{
				s_higgsCollisionCallbackRegistered = true;
				higgsInterface->AddCollisionCallback(OnHiggsCollision);
				LOG_INFO("Door lockpick: HIGGS collision callback registered");
			}

			if (!s_higgsDropCallbacksRegistered)
			{
				s_higgsDropCallbacksRegistered = true;
				higgsInterface->AddDroppedCallback(OnHiggsDropped);
				higgsInterface->AddStashedCallback(OnHiggsStashed);
				higgsInterface->AddConsumedCallback(OnHiggsConsumed);
				LOG_INFO("Door lockpick: HIGGS drop-protection callbacks registered");
			}

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
				return;

			if (isDummy && unlockedDoorSpawnDummy == 0)
			{
				s_sessionActive = true;
				s_targetDoorFormId = door->formID;
				s_spawnedLockpick = nullptr;
				s_heldItemBaseFormId = 0;
				s_sessionIsDummy = false;
				s_sessionIsHandPush = true;
				s_sessionPushHandIsLeft = false;
				s_sessionPushHandActive = false;
				s_sessionIsLockpick = false;
				s_touching = false;
				CaptureDoorSessionGeometry(door, player->pos);
				StartMonitorThreadOnce();
				LOG_INFO("Door lockpick: hand-push session started for door %08X (%s face, %s to open, load=%d cave=%d base=%08X)",
					s_targetDoorFormId,
					s_sessionOpenViaPull ? "back" : "front",
					s_sessionOpenViaPull ? "pull" : "push",
					IsLoadDoor(door) ? 1 : 0,
					IsCaveLoadDoor(door) ? 1 : 0,
					door->baseForm ? door->baseForm->formID : 0);
				return;
			}

			TESForm* itemForm = nullptr;
			if (isDummy)
			{
				itemForm = LookupDummyForm();
				if (!itemForm)
				{
					LOG_ERR("Door lockpick: dummy item %06X from %s not found", kDummyLocalFormId, MOD_ESP_NAME);
					return;
				}
			}
			else
			{
				itemForm = LookupFormByID(kLockpickFormId);
				if (!itemForm)
				{
					LOG_ERR("Door lockpick: lockpick form %08X not found", kLockpickFormId);
					return;
				}

				if (!PlayerHasLockpick(player, itemForm))
				{
					ShowLockpickAlertNotification("You have no lockpicks in your inventory");
					LOG_INFO("Door lockpick: player has no lockpicks, skipping session");
					return;
				}
			}

			TESForm* shivForm = nullptr;
			if (!isDummy)
			{
				shivForm = LookupFormByID(kShivFormId);
				if (!shivForm)
				{
					LOG_ERR("Door lockpick: shiv form %08X not found", kShivFormId);
					return;
				}
			}

			CacheAndClearMainHand(player);
			if (!isDummy)
				CacheAndClearOffHand(player);

			// If the player already had lockpicks, remove one so activating the spawned copy
			// does not increase their count. (Not needed for the dummy: it never
			// stays in inventory.)
			if (!isDummy)
			{
				ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
					player->extraData.GetByType(kExtraData_ContainerChanges));
				if (containerChanges && containerChanges->data)
				{
					InventoryEntryData* entry = containerChanges->data->FindItemEntry(itemForm);
					if (entry && entry->countDelta > 0)
						RemoveItemSilent(player, itemForm, 1);
				}
			}

			TESObjectREFR* itemRef = PlaceAtMe_Native(nullptr, 0, player, itemForm, 1, false, false);
			if (!itemRef)
			{
				LOG_ERR("Door lockpick: PlaceAtMe failed");
				return;
			}

			itemRef->pos = player->pos;
			itemRef->pos.z += 20.0f;

			TESObjectREFR* shivRef = nullptr;
			if (!isDummy && shivForm)
			{
				shivRef = PlaceAtMe_Native(nullptr, 0, player, shivForm, 1, false, false);
				if (!shivRef)
				{
					LOG_ERR("Door lockpick: PlaceAtMe failed for shiv");
					DeleteWorldObject(itemRef);
					return;
				}

				shivRef->pos = player->pos;
				shivRef->pos.z += 20.0f;
			}

			s_sessionActive = true;
			s_targetDoorFormId = door->formID;
			s_spawnedLockpick = itemRef;
			s_spawnedShiv = shivRef;
			s_heldItemBaseFormId = itemForm->formID;
			s_sessionIsDummy = isDummy;
			s_sessionIsHandPush = false;
			s_sessionIsLockpick = !isDummy;
			s_touching = false;
			s_pickAtDoor = false;
			s_shivAtDoor = false;
			s_unlockHapticPulsesSent = 0;
			if (!isDummy)
			{
				ResetLockpickSessionReadyDelay();
				const SInt32 lockLevel = GetRefLockLevel(door);
				s_sessionUnlockHoldMs = GetUnlockHoldMsForLockLevel(lockLevel);
			}

			CaptureDoorSessionGeometry(door, player->pos);

			const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
			const bool offHandIsLeftVR = GameHandToVRController(s_offHandIsLeft);
			if (isDummy)
			{
				std::thread([itemRef, mainHandIsLeftVR]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (g_task)
						g_task->AddTask(new GrabSessionItemTask(itemRef, mainHandIsLeftVR, itemRef));
				}).detach();
			}
			else
			{
				std::thread([itemRef, shivRef, mainHandIsLeftVR, offHandIsLeftVR]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (g_task)
						g_task->AddTask(new GrabLockpickSessionTask(itemRef, shivRef,
							mainHandIsLeftVR, offHandIsLeftVR, true));
				}).detach();
			}

			StartMonitorThreadOnce();

			LOG_INFO("Door lockpick: %s session started for door %08X (%s face, %s to open)",
				isDummy ? "dummy" : "lockpick+shiv",
				s_targetDoorFormId,
				s_sessionOpenViaPull ? "back" : "front",
				s_sessionOpenViaPull ? "pull" : "push");
		}

		void ClearLockpickSessionState()
		{
			s_sessionActive = false;
			s_targetDoorFormId = 0;
			s_crosshairDoorFormId = 0;
			s_spawnedLockpick = nullptr;
			s_spawnedShiv = nullptr;
			s_heldItemBaseFormId = 0;
			s_sessionIsDummy = false;
			s_sessionIsHandPush = false;
			s_sessionPushHandIsLeft = false;
			s_sessionPushHandActive = false;
			s_sessionIsLockpick = false;
			s_sessionIsKeyDoor = false;
			s_keyDoorConsumedOnUnlock = false;
			s_keyDoorHandAtDoor = false;
			s_keyDoorGrabReadyTime = {};
			ResetKeyDoorTurnState();
			s_cachedHandFormId = 0;
			s_cachedHandIsSpell = false;
			s_cachedOffHandFormId = 0;
			s_cachedOffHandIsSpell = false;
			s_touching = false;
			s_pickAtDoor = false;
			s_shivAtDoor = false;
			s_unlockHapticPulsesSent = 0;
			s_lockpickRespawnPending = false;
			ResetLockpickSessionReadyDelay();
			s_sessionUnlockHoldMs = 3000;
			s_hasTouchAnchor = false;
			s_hasDoorThinAxis = false;
			s_hasDoorPushDir = false;
			s_hasDoorMeshFrontNormal = false;
			s_hasSessionToPlayerDir = false;
			s_sessionOpenViaPull = false;
			s_loadDoorTransitionCooldownUntil = {};
			s_suppressHandRestore = false;
			s_deferredHandRestoreFormId = 0;
			s_deferredHandRestoreIsSpell = false;
			s_deferredHandRestoreIsLeft = false;
		}

		void AbortLockpickSessionForSaveLoad()
		{
			if (!s_sessionActive)
				return;

			LOG_INFO("Door lockpick: aborting active session for save load");

			TESObjectREFR* heldItem = s_spawnedLockpick;
			TESObjectREFR* heldShiv = s_spawnedShiv;
			const bool mainHandIsLeftVR = GameHandToVRController(s_mainHandIsLeft);
			const bool offHandIsLeftVR = GameHandToVRController(s_offHandIsLeft);

			// Drop session state first so HIGGS callbacks and the monitor task
			// ignore anything we release/delete during teardown.
			s_sessionActive = false;
			s_spawnedLockpick = nullptr;
			s_spawnedShiv = nullptr;
			s_lockpickRespawnPending = false;

			if (heldItem)
				DeleteSessionRefFromWorld(heldItem, mainHandIsLeftVR);

			if (heldShiv)
				DeleteSessionRefFromWorld(heldShiv, offHandIsLeftVR);

			if (higgsInterface)
			{
				higgsInterface->EnableHand(mainHandIsLeftVR);
				higgsInterface->EnableHand(offHandIsLeftVR);
			}

			StopLockpickSessionSound();
			ClearLockpickSessionState();
		}
	}

	bool IsMainHandLeftGameHand()
	{
		return leftHandedMode != 0;
	}

	bool GameHandToVRController(bool isLeftGameHand)
	{
		if (IsMainHandLeftGameHand())
			return !isLeftGameHand;

		return isLeftGameHand;
	}

	void EquipSpellToGameHand(Actor* actor, SpellItem* spell, bool isLeftGameHand)
	{
		if (!actor || !spell)
			return;

		EquipManager* equipMan = EquipManager::GetSingleton();
		if (!equipMan)
			return;

		BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
		CALL_MEMBER_FN(equipMan, EquipSpell)(actor, spell, slot);
	}

	void UnequipSpellFromGameHand(Actor* actor, bool isLeftGameHand)
	{
		if (!actor)
			return;

		SpellItem* spell = isLeftGameHand ? actor->leftHandSpell : actor->rightHandSpell;
		if (!spell)
			return;

		EquipManager* equipMan = EquipManager::GetSingleton();
		if (!equipMan)
			return;

		BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
		CALL_MEMBER_FN(equipMan, UnequipItem)(actor, spell, nullptr, 1, slot, true, true, true, false, nullptr);
	}

	bool IsDoorLeftVanilla(TESObjectREFR* ref)
	{
		return IsDoor(ref) && IsVanillaDoorOnly(ref);
	}

	bool IsDoorActivateTextSuppressed(TESObjectREFR* ref)
	{
		if (!IsDoor(ref) || doorLockpick == 0)
			return false;

		if (IsVanillaDoorOnly(ref) || IsExcludedFromDoorPushBase(ref))
			return false;

		if (IsDoorUnlocked(ref))
			return IsUnlockedDoorSessionEligible(ref);

		if (IsDoorLockedAndRequiresKey(ref))
			return keyDoorActions != 0;

		if (IsDoorLockedAndPickable(ref))
			return excludeLockedDoors == 0;

		return false;
	}

	void OnLockpickCrosshairRefChanged(TESObjectREFR* crosshairRef)
	{
		if (keyDoorActions != 0 && crosshairRef && IsDoor(crosshairRef) && IsDoorLockedAndRequiresKey(crosshairRef))
			s_crosshairKeyDoorFormId = crosshairRef->formID;
		else if (crosshairRef)
			s_crosshairKeyDoorFormId = 0;

		// Load-door diagnostic: track cave mouths and any teleport door on crosshair.
		if (crosshairRef && IsDoor(crosshairRef) && IsLoadDoor(crosshairRef)
			&& !IsLoadDoorTransitionCooldownActive())
			s_crosshairCaveDoorFormId = crosshairRef->formID;
		else if (crosshairRef)
			s_crosshairCaveDoorFormId = 0;

		StartMonitorThreadOnce();

		if (doorLockpick == 0)
			return;

		// Lazy hook install on the game thread, safely after every other
		// plugin's DataLoaded-time detour of the same function is in place.
		EnsureActivateHookInstalled();

		// Track the door under the crosshair for distance polling. VR fires
		// per-hand; a null ref from one device must not clear a valid door
		// target set by another (same rule as the activate-text tracker).
		if (crosshairRef && IsDoor(crosshairRef) && !IsVanillaDoorOnly(crosshairRef)
			&& !IsLoadDoorTransitionCooldownActive())
		{
			s_crosshairDoorFormId = crosshairRef->formID;
			SyncDoorAwaitingVanillaCloseState(crosshairRef);
		}
		else if (crosshairRef)
			s_crosshairDoorFormId = 0;

		if (crosshairRef && IsVanillaDoorOnly(crosshairRef))
			return;

		if (s_sessionActive)
		{
			// The monitor task owns the session lifetime (distance / unlock
			// checks). Crosshair only ends it early on a switch to a DIFFERENT
			// locked door, so the pick can follow the player's new target.
			if (crosshairRef && crosshairRef->baseForm && crosshairRef->baseForm->formType == kFormType_Door
				&& crosshairRef->formID != s_targetDoorFormId)
			{
				LOG_INFO("Door lockpick: switched to a different door, ending session");
				EndLockpickSession();
			}
			return;
		}

		if (IsDoorLockedAndPickable(crosshairRef))
		{
			StartLockpickSession(crosshairRef, false);
			return;
		}

		// Unlocked door: dummy grab or hand-push (UnlockedDoorSpawnDummy).
		// Load doors (any cell), outdoor non-load doors, and interior non-load
		// doors not on ExcludeInteriorDoorBases.
		if (IsUnlockedDoorSessionEligible(crosshairRef))
		{
			StartLockpickSession(crosshairRef, true);
			return;
		}

		// One log per door ref when pointing at a locked door that is neither
		// pickable, key-required, nor unlocked.
		if (IsDoor(crosshairRef) && crosshairRef->formID != s_lastDoorDebugFormId
			&& !IsDoorUnlocked(crosshairRef) && !IsDoorLockedAndPickable(crosshairRef)
			&& !IsDoorLockedAndRequiresKey(crosshairRef))
		{
			s_lastDoorDebugFormId = crosshairRef->formID;
			const SInt32 level = GetRefLockLevel(crosshairRef);
			LOG_INFO("Door lockpick: door %08X lockLevel=%d locked=%d pickable=%d",
				crosshairRef->formID, level, (int)IsRefLocked(crosshairRef), (int)IsPickableLockLevel(level));
		}
	}

	namespace
	{
		constexpr UInt32 kDoorVanillaCloseRecordType = 'DRVC';
		constexpr UInt32 kDoorVanillaCloseRecordVersion = 1;

		void DoorVanillaCloseSaveCallback(SKSESerializationInterface* intfc)
		{
			if (!intfc || !intfc->OpenRecord(kDoorVanillaCloseRecordType, kDoorVanillaCloseRecordVersion))
				return;

			const UInt32 count = static_cast<UInt32>(s_interiorDoorsAwaitingVanillaClose.size());
			intfc->WriteRecordData(&count, sizeof(count));

			for (UInt32 formId : s_interiorDoorsAwaitingVanillaClose)
				intfc->WriteRecordData(&formId, sizeof(formId));

			if (count > 0)
			{
				LOG_INFO("Door lockpick: [save] persisted %u interior door(s) awaiting vanilla close",
					count);
			}

			SaveLockpickSkillXp(intfc);
		}

		void DoorVanillaCloseLoadCallback(SKSESerializationInterface* intfc)
		{
			s_interiorDoorsAwaitingVanillaClose.clear();
			RevertLockpickSkillXp();

			if (!intfc)
				return;

			UInt32 type = 0;
			UInt32 version = 0;
			UInt32 length = 0;

			while (intfc->GetNextRecordInfo(&type, &version, &length))
			{
				if (type == kDoorVanillaCloseRecordType && length >= sizeof(UInt32))
				{
					UInt32 count = 0;
					if (intfc->ReadRecordData(&count, sizeof(count)) != sizeof(count))
						continue;

					const UInt32 maxCount = (length - sizeof(UInt32)) / sizeof(UInt32);
					if (count > maxCount)
						count = maxCount;

					for (UInt32 i = 0; i < count; ++i)
					{
						UInt32 savedId = 0;
						if (intfc->ReadRecordData(&savedId, sizeof(savedId)) != sizeof(savedId))
							break;

						UInt32 resolvedId = 0;
						if (savedId != 0 && intfc->ResolveFormId(savedId, &resolvedId) && resolvedId != 0)
							s_interiorDoorsAwaitingVanillaClose.insert(resolvedId);
					}

					LOG_INFO("Door lockpick: [load] recovered %u interior door(s) awaiting vanilla close",
						static_cast<UInt32>(s_interiorDoorsAwaitingVanillaClose.size()));
				}
				else if (type == 'LKXP')
				{
					LoadLockpickSkillXpRecord(intfc, version, length);
				}
			}
		}

		void DoorVanillaCloseRevertCallback(SKSESerializationInterface* /*intfc*/)
		{
			s_interiorDoorsAwaitingVanillaClose.clear();
			RevertLockpickSkillXp();
		}

		void PruneInvalidVanillaCloseDoors()
		{
			for (auto it = s_interiorDoorsAwaitingVanillaClose.begin();
				it != s_interiorDoorsAwaitingVanillaClose.end();)
			{
				TESForm* form = LookupFormByID(*it);
				TESObjectREFR* doorRef = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
				if (!IsDoor(doorRef))
				{
					it = s_interiorDoorsAwaitingVanillaClose.erase(it);
					continue;
				}

				// Saved awaiting state but the door loaded closed: restore push
				// for this ref only (other refs in the set are unaffected).
				if (IsDoorPhysicallyClosed(doorRef))
				{
					LOG_INFO("Door lockpick: [load] door %08X is closed, clearing stale awaiting state",
						*it);
					it = s_interiorDoorsAwaitingVanillaClose.erase(it);
				}
				else
					++it;
			}
		}
	}

	void RegisterDoorMechanicsSerialization(SKSESerializationInterface* serialization, PluginHandle pluginHandle)
	{
		if (!serialization)
		{
			LOG_INFO("Door lockpick: no SKSE serialization interface - interior door state will not persist across loads.");
			return;
		}

		serialization->SetUniqueID(pluginHandle, 'BBVR');
		serialization->SetRevertCallback(pluginHandle, DoorVanillaCloseRevertCallback);
		serialization->SetSaveCallback(pluginHandle, DoorVanillaCloseSaveCallback);
		serialization->SetLoadCallback(pluginHandle, DoorVanillaCloseLoadCallback);
		LOG_INFO("Door lockpick: registered co-save serialization for interior door vanilla-close state.");
	}

	void OnDoorMechanicsPreLoadGame()
	{
		AbortLockpickSessionForSaveLoad();
	}

	void OnDoorMechanicsPostLoadGame()
	{
		StopLockpickSessionSound();
		ClearLockpickSessionState();
		PruneInvalidVanillaCloseDoors();
		LOG_INFO("Door lockpick: session state cleared after save load");
	}
}
