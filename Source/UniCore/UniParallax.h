#pragma once

// UniParallax.h
// Placing scene actors consistently with the compressed backdrop (M3, D-146). Engine-agnostic.
//
// Sprites are drawn at p = dir * f(r) with f the radial compression (identity inside r0,
// logarithmic beyond, UniPhotometry.ush). Scaling about the eye preserves every direction and
// every angular size, so an actor can follow exactly the same rule:
//
//     render offset from the eye = (object - camera) * s,   actor scale = s,   s = f(d) / d
//
// with d the true distance to the actor's centre. Its silhouette, angular size and parallax are
// then identical to an uncompressed actor's, at any distance, and identical to the backdrop
// sprites around it.
//
// Real space: while the actor's surface is inside r0 (d - R < r0) the placement is unit scale
// (s = 1), so a body you are close to is at its true size and distance for physics and gameplay.
// The switch between the two placements changes nothing on screen (both preserve projection);
// it has 1.25x hysteresis on r0 so a body hovering at the edge does not toggle its scale.
//
// Speed: the placement is recomputed from the universal camera every frame. Nothing moves by its
// own physics while placed this way, so actors move exactly in step with the backdrop at any
// camera speed.

#include "UniCoord.h"
#include "UniShaderCores.h"

#include <cmath>

namespace UniCore
{
	constexpr double ParallaxRealSpaceHysteresis = 1.25;
	/** Real space only while the centre is closer than this (5 million km): a supergiant's
	 *  surface can be inside r0 with its centre beyond the engine's world bounds, so it stays
	 *  compressed (identical on screen) and its centre stays inside them. */
	constexpr double ParallaxMaxRealSpaceCm = 5.0e11;

	/** f(r) in double: the same function as Uni_Compress (UniPhotometry.ush). */
	inline double CompressD(double R)
	{
		const double R0 = double(UniHlsl::UNI_R0_CM);
		return R <= R0 ? R : R0 * (1.0 + std::log(R / R0));
	}

	struct FParallaxPlacement
	{
		FVec3d Offset;            // render-space offset from the eye, cm
		double Scale = 1.0;       // uniform actor scale
		double Distance = 0.0;    // true distance to the centre, cm
		bool bRealSpace = true;   // unit scale (surface inside r0)
	};

	/** Where to put an actor whose centre is at Obj, of radius RadiusCm, seen from the camera at
	 *  Cam (+ CamFrac, its sub-cm part). bWasRealSpace: last frame's state, for hysteresis. */
	inline FParallaxPlacement ParallaxPlace(const FCoord& Obj, double RadiusCm, const FCoord& Cam, const FVec3f& CamFrac, bool bWasRealSpace)
	{
		FParallaxPlacement P;
		FVec3d D = ToDouble(Sub(Obj, Cam));
		D.X -= double(CamFrac.X); D.Y -= double(CamFrac.Y); D.Z -= double(CamFrac.Z);
		P.Distance = Length(D);
		const double Limit = double(UniHlsl::UNI_R0_CM) * (bWasRealSpace ? ParallaxRealSpaceHysteresis : 1.0);
		P.bRealSpace = (P.Distance - RadiusCm) < Limit && P.Distance < ParallaxMaxRealSpaceCm;
		P.Scale = (P.bRealSpace || P.Distance <= 0.0) ? 1.0 : CompressD(P.Distance) / P.Distance;
		P.Offset = FVec3d{ D.X * P.Scale, D.Y * P.Scale, D.Z * P.Scale };
		return P;
	}
}
