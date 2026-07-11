#pragma once

#include "skse64/GameReferences.h"
#include "skse64/NiTypes.h"

namespace InteractiveLockpickingVR
{
	// Havok physics raycast for hand-on-door contact.
	// Must run on the main game thread.

	struct DoorHandRaycastHit
	{
		bool hit = false;
		float distance = 0.0f;
		NiPoint3 hitPoint;
		NiPoint3 hitNormal;
		TESObjectREFR* hitRef = nullptr;
	};

	bool IsDoorHandRaycastEnabled();

	bool CastDoorHandRay(const NiPoint3& origin, const NiPoint3& direction, float maxDistance,
		DoorHandRaycastHit& outHit);

	bool IsHandTouchingDoorViaRaycast(TESObjectREFR* doorRef, const NiPoint3& handPos,
		float rayLength, float& outPlaneDist, NiPoint3& outHitNormal);
}
