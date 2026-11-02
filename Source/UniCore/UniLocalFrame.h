#pragma once

// UniLocalFrame.h
// The real-space bubble's frame (M3, D-149). Engine-agnostic.
//
// In Local mode the UE world is a window onto the universe: world position p (cm, double)
// is the universal coordinate Anchor + p. The pawn and physics move in it normally. When the pawn
// gets far from the anchor (precision and physics prefer small coordinates), the frame is
// re-centred: the anchor moves by an INTEGER-cm shift S, and everything in the world moves by -S.
// Because S is whole centimetres, Anchor + p is unchanged to the bit: the universal camera, every
// body and every backdrop sprite stay exactly where they were, and nothing can be seen to move.

#include "UniCoord.h"

#include <cmath>

namespace UniCore
{
	/** Universal coordinate (and its sub-cm fraction) of a local-world position. */
	inline FCoord LocalToUniversal(const FCoord& Anchor, const FVec3d& Local, FVec3f& OutFraction)
	{
		FCoord C = Anchor;
		OutFraction = FVec3f{};
		AddDisplacement(C, OutFraction, Local);
		return C;
	}

	/** The re-centring shift for a local position: its whole-centimetre part. */
	inline FVec3d RecentreShift(const FVec3d& Local)
	{
		return FVec3d{ std::round(Local.X), std::round(Local.Y), std::round(Local.Z) };
	}

	/** Move the anchor by an integer-cm shift (exact: no fraction is produced). */
	inline void ShiftAnchor(FCoord& Anchor, const FVec3d& IntegerShift)
	{
		FVec3f Discard{};
		AddDisplacement(Anchor, Discard, IntegerShift);
	}
}
