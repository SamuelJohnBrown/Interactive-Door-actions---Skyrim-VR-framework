#include "DoorHandRaycast.h"
#include "config.h"

#include "skse64/GameAPI.h"
#include "skse64/GameCamera.h"
#include "skse64/GameForms.h"
#include "skse64_common/Relocation.h"

#include <cmath>
#include <cstring>

namespace InteractiveLockpickingVR
{
	namespace
	{
		class bhkWorld;

		struct alignas(16) HavokVector4
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			float w = 0.0f;
		};

		struct HavokWorldRayCastInput
		{
			HavokVector4 from;
			HavokVector4 to;
			bool enableShapeCollectionFilter = false;
			UInt8 pad21[3] = {};
			UInt32 filterInfo = 0;
			UInt8 pad28[8] = {};
		};
		static_assert(sizeof(HavokWorldRayCastInput) == 0x30, "HavokWorldRayCastInput layout mismatch");

		struct HavokShapeRayCastOutput
		{
			HavokVector4 normal;
			float hitFraction = 1.0f;
			SInt32 extraInfo = -1;
			UInt32 shapeKey = 0xFFFFFFFF;
			UInt32 pad1C = 0;
			UInt32 shapeKeys[8] = {
				0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
				0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
			SInt32 shapeKeyIndex = 0;
			UInt32 pad44 = 0;
			UInt64 pad48 = 0;
		};
		static_assert(sizeof(HavokShapeRayCastOutput) == 0x50, "HavokShapeRayCastOutput layout mismatch");

		struct HavokWorldRayCastOutput : HavokShapeRayCastOutput
		{
			const void* rootCollidable = nullptr;
			UInt64 pad58 = 0;
		};
		static_assert(sizeof(HavokWorldRayCastOutput) == 0x60, "HavokWorldRayCastOutput layout mismatch");

		struct HavokPickData
		{
			HavokWorldRayCastInput rayInput;
			HavokWorldRayCastOutput rayOutput;
			HavokVector4 ray;
			void* rayHitCollectorA0 = nullptr;
			void* rayHitCollectorA8 = nullptr;
			void* rayHitCollectorB0 = nullptr;
			void* rayHitCollectorB8 = nullptr;
			bool unkC0 = false;
			UInt8 padC1 = 0;
			UInt16 padC2 = 0;
			UInt32 padC4 = 0;
			UInt32 padC8 = 0;
			UInt32 padCC = 0;
		};
		static_assert(sizeof(HavokPickData) == 0xD0, "HavokPickData layout mismatch");

		typedef bhkWorld* (*GetCellBhkWorldFn)(const TESObjectCELL* cell);
		typedef TESObjectREFR* (*FindCollidableRefFn)(const void* collidable);
		typedef bool (*BhkWorldPickObjectFn)(bhkWorld* world, HavokPickData* pickData);

		static RelocAddr<GetCellBhkWorldFn> GetCellBhkWorld(0x00276A90);
		static RelocAddr<FindCollidableRefFn> FindCollidableRef(0x003B4940);
		static RelocPtr<float> g_havokWorldScale(0x015B78F4);

		bool HasRayHit(const HavokWorldRayCastOutput& output)
		{
			return output.rootCollidable != nullptr;
		}

		float GetHavokWorldScale()
		{
			const float scale = g_havokWorldScale ? *g_havokWorldScale : 0.0f;
			return scale > 0.0f ? scale : 0.0142875f;
		}

		HavokVector4 ScalePoint(const NiPoint3& point, float scale)
		{
			HavokVector4 out;
			out.x = point.x * scale;
			out.y = point.y * scale;
			out.z = point.z * scale;
			out.w = 0.0f;
			return out;
		}

		NiPoint3 NormalizeDirection(const NiPoint3& direction)
		{
			const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
			if (len < 0.0001f)
				return NiPoint3();

			return NiPoint3(direction.x / len, direction.y / len, direction.z / len);
		}

		UInt32 GetCollidableLayer(const void* collidable)
		{
			if (!collidable)
				return 0;

			// hkpCollidable::broadPhaseHandle.collisionFilterInfo.filter (lower 7 bits).
			const UInt32* filterPtr = reinterpret_cast<const UInt32*>(reinterpret_cast<uintptr_t>(collidable) + 0x2C);
			return *filterPtr & 0x7F;
		}

		bool IsDoorRaycastLayer(UInt32 layer)
		{
			switch (layer)
			{
			case 1:  // kStatic
			case 2:  // kAnimStatic
			case 10: // kProps
				return true;
			default:
				return false;
			}
		}

		bool PickObject(bhkWorld* world, HavokPickData& pickData)
		{
			if (!world)
				return false;

			void** vtable = *reinterpret_cast<void***>(world);
			if (!vtable)
				return false;

			BhkWorldPickObjectFn pick = reinterpret_cast<BhkWorldPickObjectFn>(vtable[33]);
			return pick && pick(world, &pickData);
		}

		bhkWorld* GetPlayerCellBhkWorld()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !player->parentCell)
				return nullptr;

			return GetCellBhkWorld(player->parentCell);
		}

		bool TryGetCameraViewPos(NiPoint3& outPos)
		{
			PlayerCamera* camera = PlayerCamera::GetSingleton();
			if (!camera)
				return false;

			outPos = camera->pos;
			return true;
		}

		bool FillRaycastHit(const NiPoint3& origin, const NiPoint3& direction, float maxDistance,
			const HavokPickData& pickData, DoorHandRaycastHit& outHit)
		{
			if (!HasRayHit(pickData.rayOutput))
				return false;

			const UInt32 layer = GetCollidableLayer(pickData.rayOutput.rootCollidable);
			if (!IsDoorRaycastLayer(layer))
				return false;

			TESObjectREFR* hitRef = FindCollidableRef(pickData.rayOutput.rootCollidable);
			if (!hitRef)
				return false;

			outHit.hit = true;
			outHit.distance = maxDistance * pickData.rayOutput.hitFraction;
			outHit.hitPoint = NiPoint3(
				origin.x + direction.x * outHit.distance,
				origin.y + direction.y * outHit.distance,
				origin.z + direction.z * outHit.distance);
			outHit.hitNormal = NiPoint3(
				pickData.rayOutput.normal.x,
				pickData.rayOutput.normal.y,
				pickData.rayOutput.normal.z);
			outHit.hitRef = hitRef;
			return true;
		}
	}

	bool IsDoorHandRaycastEnabled()
	{
		return unlockedDoorUsePhysicsRaycast != 0;
	}

	bool CastDoorHandRay(const NiPoint3& origin, const NiPoint3& direction, float maxDistance,
		DoorHandRaycastHit& outHit)
	{
		outHit = DoorHandRaycastHit();

		if (maxDistance <= 0.0f)
			return false;

		bhkWorld* world = GetPlayerCellBhkWorld();
		if (!world)
			return false;

		const NiPoint3 dir = NormalizeDirection(direction);
		if (dir.x == 0.0f && dir.y == 0.0f && dir.z == 0.0f)
			return false;

		const float scale = GetHavokWorldScale();
		const NiPoint3 rayEnd = NiPoint3(
			origin.x + dir.x * maxDistance,
			origin.y + dir.y * maxDistance,
			origin.z + dir.z * maxDistance);

		HavokPickData pickData;
		std::memset(&pickData, 0, sizeof(pickData));
		pickData.rayInput.from = ScalePoint(origin, scale);
		pickData.rayInput.to = ScalePoint(rayEnd, scale);

		if (!PickObject(world, pickData))
			return false;

		return FillRaycastHit(origin, dir, maxDistance, pickData, outHit);
	}

	namespace
	{
		bool RayHitsTargetDoor(const NiPoint3& origin, const NiPoint3& direction, float maxDistance,
			TESObjectREFR* doorRef, DoorHandRaycastHit& outHit)
		{
			if (!doorRef || !CastDoorHandRay(origin, direction, maxDistance, outHit))
				return false;

			return outHit.hitRef == doorRef;
		}

		// Six short rays from the hand through nearby static geometry.
		bool CastMultiDirectionalDoorRays(const NiPoint3& handPos, float rayLength,
			TESObjectREFR* doorRef, DoorHandRaycastHit& outHit)
		{
			static const NiPoint3 kDirections[] = {
				{ 1.0f, 0.0f, 0.0f },
				{ -1.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f },
				{ 0.0f, -1.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f },
				{ 0.0f, 0.0f, -1.0f },
			};

			for (const NiPoint3& dir : kDirections)
			{
				if (RayHitsTargetDoor(handPos, dir, rayLength, doorRef, outHit))
					return true;
			}

			return false;
		}

		// Cast from the camera toward the hand to catch hands already inside door colliders.
		bool CastRayTowardCamera(const NiPoint3& handPos, TESObjectREFR* doorRef, DoorHandRaycastHit& outHit)
		{
			NiPoint3 cameraPos;
			if (!TryGetCameraViewPos(cameraPos))
				return false;

			NiPoint3 toHand(handPos.x - cameraPos.x, handPos.y - cameraPos.y, handPos.z - cameraPos.z);
			const float distance = std::sqrt(toHand.x * toHand.x + toHand.y * toHand.y + toHand.z * toHand.z);
			if (distance < 0.001f)
				return false;

			const NiPoint3 direction(toHand.x / distance, toHand.y / distance, toHand.z / distance);
			return RayHitsTargetDoor(cameraPos, direction, distance, doorRef, outHit);
		}

		// Push direction: hand toward the door anchor catches flat door faces reliably.
		bool CastRayTowardDoor(const NiPoint3& handPos, TESObjectREFR* doorRef, float rayLength,
			DoorHandRaycastHit& outHit)
		{
			if (!doorRef)
				return false;

			NiPoint3 toDoor(doorRef->pos.x - handPos.x, doorRef->pos.y - handPos.y, doorRef->pos.z - handPos.z);
			toDoor.z = 0.0f;
			const float len = std::sqrt(toDoor.x * toDoor.x + toDoor.y * toDoor.y);
			if (len < 1.0f)
				return false;

			toDoor.x /= len;
			toDoor.y /= len;
			toDoor.z = 0.0f;
			return RayHitsTargetDoor(handPos, toDoor, rayLength, doorRef, outHit);
		}
	}

	bool IsHandTouchingDoorViaRaycast(TESObjectREFR* doorRef, const NiPoint3& handPos,
		float rayLength, float& outPlaneDist, NiPoint3& outHitNormal)
	{
		outPlaneDist = 9999.0f;
		outHitNormal = NiPoint3();

		if (!doorRef || !IsDoorHandRaycastEnabled() || rayLength <= 0.0f)
			return false;

		DoorHandRaycastHit hit;
		if (CastMultiDirectionalDoorRays(handPos, rayLength, doorRef, hit)
			|| CastRayTowardDoor(handPos, doorRef, rayLength, hit)
			|| CastRayTowardCamera(handPos, doorRef, hit))
		{
			outHitNormal = hit.hitNormal;
			const NiPoint3 delta(
				hit.hitPoint.x - doorRef->pos.x,
				hit.hitPoint.y - doorRef->pos.y,
				hit.hitPoint.z - doorRef->pos.z);
			outPlaneDist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			return true;
		}

		return false;
	}
}
