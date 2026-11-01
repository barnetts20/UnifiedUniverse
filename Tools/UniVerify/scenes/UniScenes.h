#pragma once

// UniScenes.h
// Test scenes for univerify: archetype sets and camera placements. Every number here is a
// PLACEHOLDER chosen to be physically plausible (D-012, D-014), not tuned art direction.
//
//   web_to_galaxy   root cosmic-web emitter places galaxies; camera in intergalactic space
//   galaxy_to_star  one anchored Milky-Way-like galaxy; camera in its disc 8 kpc out
//   flythrough      both, plus a suppressor clearing generated galaxies around the anchor

#include "UniGenRef.h"
#include "UniScaleLadder.h"

#include <cmath>
#include <string>

namespace UniScenes
{
	using namespace UniCore;
	namespace H = UniHlsl;

	constexpr u32 kStar = 1, kGalaxy = 2;

	inline FArchetype MakeStar()
	{
		FArchetype A;
		A.Id = kStar;
		A.Name = "Star";
		A.Visibility = EVisibility::Point;
		// Placeholder V-band stellar luminosity function, stars / pc^3 / mag (log10), shaped
		// after the solar-neighbourhood function: rising to M ~ +12, then falling (D-014).
		A.AbsMag = FMagnitudeLut::FromLogControlPoints({
			{ -10.0, -9.5 }, { -8.0, -8.5 }, { -6.0, -7.3 }, { -4.0, -6.0 }, { -2.0, -4.9 }, { 0.0, -3.7 },
			{ 2.0, -3.0 }, { 4.0, -2.6 }, { 6.0, -2.3 }, { 8.0, -2.1 }, { 10.0, -1.9 }, { 12.0, -1.8 },
			{ 14.0, -1.9 }, { 16.0, -2.2 } });
		// log10 R_cm = 10.84 - 0.15 (M - 4.83) +- 0.15: the Sun at M = 4.83, supergiants ~1000 R_sun.
		A.RadiusModel[0] = 10.84f; A.RadiusModel[1] = -0.15f; A.RadiusModel[2] = 4.83f; A.RadiusModel[3] = 0.15f;
		// log10 T = 3.76 - 0.05 (M - 4.83) +- 0.05.
		A.ColourModel[0] = 3.76f; A.ColourModel[1] = -0.05f; A.ColourModel[2] = 4.83f; A.ColourModel[3] = 0.05f;
		return A;
	}

	/** Galaxy field exemplar: units of the influence radius (1.6 x galaxy radius). */
	inline FFieldParams GalaxyExemplar()
	{
		FFieldParams P;
		P.V[0] = 0.13f;    // disc scale length (~3 kpc for a 15 kpc galaxy)
		P.V[1] = 0.0125f;  // disc scale height (~0.3 kpc)
		P.V[2] = 1.0f;     // disc weight
		P.V[3] = 0.04f;    // bulge effective radius
		P.V[4] = 0.6f;     // bulge flattening
		P.V[5] = 0.6f;     // bulge weight
		P.V[6] = 2.0f;     // arm count
		P.V[7] = 0.25f;    // tan(pitch) ~ 14 degrees
		P.V[8] = 0.6f;     // arm amplitude
		P.V[9] = 3.0f;     // arm sharpness
		P.V[10] = 0.0f;    // arm phase
		P.V[12] = 1.0f;    // cut radius (the influence sphere)
		return P;
	}

	inline FArchetype MakeGalaxy()
	{
		FArchetype A;
		A.Id = kGalaxy;
		A.Name = "Galaxy";
		A.Visibility = EVisibility::Extended;
		// Placeholder Schechter-shaped function, M* = -21, alpha = -1.1, over -23.5..-16 (D-014).
		A.AbsMag = FMagnitudeLut::Schechter(-21.0, -1.1, -23.5, -16.0);
		// log10 R_cm = 22.67 - 0.2 (M + 21) +- 0.1: 15 kpc at M*, R ~ L^0.5.
		A.RadiusModel[0] = 22.67f; A.RadiusModel[1] = -0.2f; A.RadiusModel[2] = -21.0f; A.RadiusModel[3] = 0.1f;
		A.ColourModel[0] = 3.75f; A.ColourModel[1] = 0.02f; A.ColourModel[2] = -21.0f; A.ColourModel[3] = 0.08f;

		FEmitter E;
		E.ChildArchetypeId = kStar;
		E.FieldId = H::UNI_FIELD_GALAXY;
		E.Exemplar = GalaxyExemplar();
		E.NTotal = 2.0e11;
		E.NTotalRefMag = -21.0;
		E.InfluenceMul = 1.6;
		E.Ranges = {
			{ "Disc.ScaleLength", 0, 0.10f, 0.18f, 1.0f, 0.0f },
			{ "Bulge.Weight", 5, 0.2f, 1.5f, 1.5f, 0.0f },
			{ "Arms.Count", 6, 2.0f, 4.0f, 1.5f, 1.0f },
			{ "Arms.TanPitch", 7, 0.15f, 0.45f, 1.0f, 0.0f },
			{ "Arms.Amplitude", 8, 0.3f, 0.8f, 1.0f, 0.0f },
			{ "Arms.Phase", 10, 0.0f, 6.2831853f, 1.0f, 0.0f },
		};
		A.Emitters.push_back(E);
		return A;
	}

	inline FEmitter WebEmitter()
	{
		FEmitter E;
		E.ChildArchetypeId = kGalaxy;
		E.FieldId = H::UNI_FIELD_WEB;
		E.bDensityMode = true;
		E.PeriodBits = 88;                    // 2^88 cm = 100 Mpc period
		E.Exemplar.V[0] = 5.0f;               // G: 20 Mpc lattice cells
		E.Exemplar.V[1] = 0.12f;              // width
		E.Exemplar.V[2] = 0.05f;              // void floor
		E.Exemplar.V[3] = 1.0f;               // wall weight
		E.Exemplar.V[4] = 3.0f;               // filament weight
		E.Exemplar.V[5] = 7.0f;               // G2 (coprime with 5, lesson #6)
		E.Exemplar.V[6] = 0.3f;               // second-lattice weight
		E.Exemplar.SetUint(7, 0x5EED1234u);
		E.NTotal = 0.02 / std::pow(Units::Mpc, 3.0);   // galaxies per cm^3 over the LF range
		return E;
	}

	inline FUniverseDef Base()
	{
		FUniverseDef D;
		D.Seed = 69;   // UltraLargeScale's universe root seed
		D.Archetypes.resize(3);
		D.Archetypes[0].Id = 0; D.Archetypes[0].Name = "Root";
		D.Archetypes[kStar] = MakeStar();
		D.Archetypes[kGalaxy] = MakeGalaxy();
		return D;
	}

	/** The anchored Milky-Way-like galaxy at the nominal origin, disc in the xy plane. */
	inline FAnchor MilkyWayAnchor()
	{
		FAnchor A;
		A.Coord = FCoord::Origin();
		A.ArchetypeId = kGalaxy;
		A.AbsMag = -21.0f;
		A.RadiusLog2 = float(std::log2(15.0 * Units::Kpc));
		A.EmitterParams = { GalaxyExemplar() };
		A.bFixedOrientation = true;
		A.Seed = 0xA2C40001u;
		return A;
	}

	inline FUniverseDef WebToGalaxy()
	{
		FUniverseDef D = Base();
		D.RootEmitters.push_back(WebEmitter());
		return D;
	}

	inline FUniverseDef GalaxyToStar()
	{
		FUniverseDef D = Base();
		D.Anchors.push_back(MilkyWayAnchor());
		return D;
	}

	inline FUniverseDef Flythrough()
	{
		FUniverseDef D = Base();
		D.RootEmitters.push_back(WebEmitter());
		D.Anchors.push_back(MilkyWayAnchor());
		FSuppressor S;
		S.Centre = FCoord::Origin();
		S.RadiusCm = 300.0 * Units::Kpc;
		S.ArchetypeId = kGalaxy;
		D.Suppressors.push_back(S);
		return D;
	}

	/** Camera 8 kpc from the anchor's centre in its disc, 20 pc above the plane. */
	inline FCoord SolarCamera()
	{
		FCoord C = FCoord::Origin();
		FVec3f F;
		AddDisplacement(C, F, FVec3d{ 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc });
		return C;
	}

	/** Camera in intergalactic space: 37 Mpc from the origin, off any lattice symmetry. */
	inline FCoord VoidCamera()
	{
		FCoord C = FCoord::Origin();
		FVec3f F;
		AddDisplacement(C, F, FVec3d{ 37.0 * Units::Mpc, -11.0 * Units::Mpc, 5.3 * Units::Mpc });
		return C;
	}
}
