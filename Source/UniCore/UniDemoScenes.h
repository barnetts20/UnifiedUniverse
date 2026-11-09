#pragma once

// UniDemoScenes.h
// Demo and test scenes: archetype sets and camera placements, shared by univerify and by
// UUniSubsystem (uu.Flythrough, uu.Scene). Every number here is a
// PLACEHOLDER chosen to be physically plausible (D-012, D-014), not tuned art direction.
//
//   web_to_galaxy   root cosmic-web emitter places galaxies; camera in intergalactic space
//   galaxy_to_star  one anchored Milky-Way-like galaxy; camera in its disc 8 kpc out
//   flythrough      both, plus a suppressor clearing generated galaxies around the anchor
//
// D-151: the web scenes also place galaxy groups and clusters (FClusterTuning, realistic by
// default: 15% of all galaxies in clusters, 8.6% in rich ones; groups stay field galaxies).

#include "UniGenRef.h"
#include "UniScaleLadder.h"
#include "UniVolume.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace UniScenes
{
	using namespace UniCore;
	namespace H = UniHlsl;

	constexpr u32 kStar = 1, kGalaxy = 2, kCluster = 3, kDwarf = 4, kGroup = 5, kMember = 6;   // D-174: dwarfs and groups; D-177: clusters' fainter members

	/** D-151: the tuning handles for galaxy groups and clusters. Defaults are the realistic
	 *  distribution; every value here changes generated content (it is a content definition), so
	 *  tune before authoring. UE: the uu.Cluster.* console variables, applied on uu.Enable. */
	struct FClusterTuning
	{
		bool bEnabled = true;
		/** Fraction of all galaxies that are members of clusters. The rest are field galaxies
		 *  placed by the web directly (their number scales by 1 - Fraction, so the total galaxy
		 *  density is unchanged). Realistic: 10-15% (5-10% in rich clusters). */
		double Fraction = 0.15;
		/** Cluster luminosity function (total light of a cluster, V band): Schechter, number per
		 *  luminosity ~ L^Alpha exp(-L / L*). With these defaults 57% of member galaxies (8.6% of
		 *  all) are in rich clusters, M < -25 (300+ galaxies each). */
		double MStar = -25.3;
		double Alpha = -2.0;
		double BrightMag = -27.5;   // the richest clusters (Coma-like, ~2000 galaxies)
		double FaintMag = -24.5;    // the poorest clusters (~200 galaxies with dwarfs); groups are field galaxies (D-151)
		/** Placement on the web: clusters sit on the same lattice as the field galaxies, with this
		 *  power on the web density (higher: more concentrated on filaments) and this weight on the
		 *  web's nodes (where filaments meet). */
		double BiasPower = 2.0;
		double NodeWeight = 6.0;
		/** Size: multiplies cluster radii (and so their influence and surface brightness). */
		double RadiusScale = 1.0;
		/** Shape: core radius range (fraction of the influence radius), and sub-clumps. */
		double CoreMin = 0.06, CoreMax = 0.14;
		int MaxClumps = 3;
		double ClumpWeight = 0.6;
		/** D-172, morphology-density: the share of members that are early types (elliptical or
		 *  lenticular: no arms, no dust, redder) is EarlyOuter + (EarlyCore - EarlyOuter)
		 *  exp(-r / EarlyScale), r in units of the influence radius (~4 cluster radii; the
		 *  virial radius is ~0.25). Observed: ~80% in cores, ~40-50% near the virial radius,
		 *  ~20% in the field. EarlyCore 0: off (members as before D-172). */
		double EarlyCore = 0.8, EarlyOuter = 0.2, EarlyScale = 0.25;
		double EarlyColourShift = -0.07;   // log10 K: ~5600 K -> ~4800 K at the reference magnitude
		/** D-174, galaxy groups from the web's node list (owner's option 2): the nodes after the
		 *  clusters become group objects. A group places only dwarfs (fainter than the galaxy
		 *  function's -16, so nothing is counted twice); its bright galaxies are the web's field
		 *  galaxies, as before. GroupDwarfs: the dwarfs (M -16..-12) of the median-mass group;
		 *  each group's count goes with its node's mass. */
		bool bGroups = true;
		double GroupDwarfs = 20.0;
		/** D-191: rich catalogue clusters (M < BcgMaxMag) carry a brightest cluster galaxy
		 *  (BcgShare of their light at M -26.5, rising as L^-0.5 toward poorer ones: observed
		 *  ~5-10% in the richest clusters) and intracluster light (IclShare; observed ~10-20%),
		 *  taken from their members' budget. 0: none. */
		double BcgShare = 0.06;
		double IclShare = 0.12;
		double BcgMaxMag = -25.0;
		/** D-197 (owner: ~30% fewer entities): the share of cluster members and group dwarfs, and
		 *  of every galaxy's stars, that are generated and drawn (FUniEmitter::SpriteKeep). The
		 *  rest of their light is glow (their parent's carrier), so nothing dims. Applied by
		 *  UseSpriteKeep (the engine does at uu.Enable); 1: every child, as before D-197. */
		double MemberKeep = 0.5;   // D-198: 0.7 -> 0.5 (owner: less pop-in of nearby stars, less to load in clusters)
		double StarKeep = 0.35;    // D-205: 0.5 -> 0.35 (owner: speed inside galaxy cores; ~28% less generation there)
		/** D-198 (owner: darker dust lanes): galaxies' dust optical depth x DustScale, and their
		 *  textured lanes' contrast (FVolumeProfile::DustLanes). Applied by UseDustTuning (the
		 *  engine does at uu.Enable); 1 and 2.2: the D-132 / D-180 dust. */
		double DustScale = 2.0;
		double DustLanes = 3.0;
	};

	/** The galaxy number density over the galaxy luminosity function (the D-012 placeholder). */
	constexpr double kGalaxyDensityPerMpc3 = 0.02;

	inline FArchetype MakeStar()
	{
		FArchetype A;
		A.Id = kStar;
		A.Name = "Star";
		A.Visibility = EUniVisibility::Point;
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
		A.bSystems = true;   // B-002: every star has a system (possibly empty)
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
		P.V[11] = 0.25f;   // D-210: arms' inner radius (disc scale lengths): arms fade to their mean inside it, the disc warp shrinks to 0 at the centre
		P.V[12] = 1.0f;    // cut radius (the influence sphere)
		return P;
	}

	inline FArchetype MakeGalaxy()
	{
		FArchetype A;
		A.Id = kGalaxy;
		A.Name = "Galaxy";
		A.Visibility = EUniVisibility::Extended;
		// Placeholder Schechter-shaped function, M* = -21, alpha = -1.1, over -23.5..-16 (D-014).
		A.AbsMag = FMagnitudeLut::Schechter(-21.0, -1.1, -23.5, -16.0);
		// log10 R_cm = 22.67 - 0.2 (M + 21) +- 0.1: 15 kpc at M*, R ~ L^0.5.
		A.RadiusModel[0] = 22.67f; A.RadiusModel[1] = -0.2f; A.RadiusModel[2] = -21.0f; A.RadiusModel[3] = 0.1f;
		A.ColourModel[0] = 3.75f; A.ColourModel[1] = 0.02f; A.ColourModel[2] = -21.0f; A.ColourModel[3] = 0.08f;

		FUniEmitter E;
		E.ChildArchetypeId = kStar;
		E.FieldId = H::UNI_FIELD_GALAXY;
		E.Exemplar = GalaxyExemplar();
		// D-133 (owner, option B): 2.69e10 stars at M = -21, so the stars add up to the galaxy's own
		// magnitude (the placeholder stellar function's budget; 2e11 summed to -23.18). Re-derive from
		// LuminosityBudget whenever the stellar function changes (D-014).
		E.NTotal = 2.69e10;
		E.NTotalRefMag = -21.0;
		E.InfluenceMul = 1.6;
		E.Ranges = {
			{ "Disc.ScaleLength", 0, 0.10f, 0.18f, 1.0f, 0.0f },
			{ "Bulge.Weight", 5, 0.2f, 1.5f, 1.5f, 0.0f },
			{ "Arms.Count", 6, 2.0f, 4.0f, 1.5f, 1.0f },
			{ "Arms.TanPitch", 7, 0.15f, 0.45f, 1.0f, 0.0f },
			{ "Arms.Amplitude", 8, 0.3f, 0.8f, 1.0f, 0.0f },
			{ "Arms.Phase", 10, 0.0f, 6.2831853f, 1.0f, 0.0f },
			// D-178: disc-level modes and per-arm asymmetry (UltraLargeScale's ideas), so no two
			// galaxies share a shape. Warp at the visible edge (r ~ 0.6 of the influence radius)
			// ~0.36 x its amplitude: up to ~2 kpc for a 15 kpc galaxy (the Milky Way's ~1 kpc;
			// owner, after the first engine pass: a slightly wider range).
			{ "Disc.WarpAmplitude", 16, 0.0f, 0.22f, 1.4f, 0.0f },
			{ "Disc.WarpTwist", 17, -1.2f, 1.2f, 1.0f, 0.0f },
			{ "Disc.Lopsided", 18, 0.0f, 0.35f, 1.6f, 0.0f },
			{ "Disc.Flare", 19, 0.0f, 1.2f, 1.0f, 0.0f },
			{ "Arms.Asymmetry", 20, 0.1f, 0.7f, 1.0f, 0.0f },
			{ "Arms.Tightening", 21, 0.0f, 0.5f, 1.3f, 0.0f },
			{ "Disc.ShapeSeed", 22, 0.0f, 16777215.0f, 1.0f, 1.0f },
		};
		// D-172: early types (cluster members drawn as such): a dominant, rounder, larger
		// spheroid, little or no disc (elliptical to lenticular), no arms. Their stars follow it.
		E.bEarlyShape = true;
		E.EarlyExemplar = E.Exemplar;
		E.EarlyExemplar.V[0] = 0.12f;   // disc scale length (lenticulars)
		E.EarlyExemplar.V[1] = 0.03f;   // thicker disc
		E.EarlyExemplar.V[5] = 1.5f;    // spheroid weight
		E.EarlyExemplar.V[6] = 0.0f;    // no arms
		E.EarlyExemplar.V[8] = 0.0f;
		E.EarlyRanges = {
			{ "Early.DiscWeight", 2, 0.0f, 0.6f, 2.0f, 0.0f },   // 0 elliptical .. 0.6 lenticular, skewed to ellipticals
			{ "Early.BulgeRadius", 3, 0.06f, 0.12f, 1.0f, 0.0f },   // r_e ~3 kpc at M*; larger would cut 10-20% of an n = 2 profile at the influence radius
			{ "Early.BulgeAxis", 4, 0.55f, 0.95f, 1.0f, 0.0f },
		};
		A.Emitters.push_back(E);
		// Render-only dust (D-132): placeholder, about the Milky Way's face-on A_V.
		A.Volume.bDust = true;
		A.Volume.DustTau = 0.6f;
		return A;
	}

	/** Mean V-band flux per galaxy as the generator draws it (10^(-0.4 M) units): the luminosity
	 *  budget per galaxy (D-133). Built once from a model of the galaxy archetype alone. */
	inline double GalaxyMeanFluxV()
	{
		static const double Value = []()
		{
			FUniverseDef D;
			D.Seed = 69;
			D.Archetypes.resize(3);
			D.Archetypes[0].Id = 0; D.Archetypes[0].Name = "Root";
			D.Archetypes[kStar] = MakeStar();
			D.Archetypes[kGalaxy] = MakeGalaxy();
			const FUniverseModel M(D);
			return FUnresolvedTable::Build(M, kGalaxy).MeanFluxV();
		}();
		return Value;
	}

	/** D-174: dwarf galaxies, M -16..-12 (below the galaxy function's faint end, so new content,
	 *  never a second copy of a galaxy). Placed only by groups. A steeper faint end (alpha -1.3),
	 *  4 kpc at M = -16 to ~1.3 kpc at -12 (R ~ L^0.3), bluer and more diffuse than spirals:
	 *  thick discs, weak arms, small bulges, no dust. Their stars follow them, as galaxies'. */
	inline FArchetype MakeDwarf()
	{
		FArchetype A = MakeGalaxy();
		A.Id = kDwarf;
		A.Name = "Dwarf";
		A.AbsMag = FMagnitudeLut::Schechter(-21.0, -1.3, -16.0, -12.0);
		A.RadiusModel[0] = 22.09f; A.RadiusModel[1] = -0.122f; A.RadiusModel[2] = -16.0f; A.RadiusModel[3] = 0.12f;
		A.ColourModel[0] = 3.78f; A.ColourModel[1] = 0.01f; A.ColourModel[2] = -16.0f; A.ColourModel[3] = 0.10f;
		FUniEmitter& E = A.Emitters[0];
		E.Exemplar.V[1] = 0.04f;    // thick disc
		E.Exemplar.V[3] = 0.06f;    // small bulge
		E.bEarlyShape = false;
		E.EarlyRanges.clear();
		E.Ranges = {
			{ "Dwarf.ScaleLength", 0, 0.14f, 0.26f, 1.0f, 0.0f },
			{ "Dwarf.BulgeWeight", 5, 0.0f, 0.3f, 1.5f, 0.0f },
			{ "Dwarf.Arms", 6, 0.0f, 2.0f, 1.0f, 1.0f },
			{ "Dwarf.ArmAmplitude", 8, 0.0f, 0.3f, 1.0f, 0.0f },
			{ "Dwarf.ArmPhase", 10, 0.0f, 6.2831853f, 1.0f, 0.0f },
			// D-178: dwarfs are the most irregular: strongly lopsided, ragged arms when they have any.
			{ "Dwarf.WarpAmplitude", 16, 0.0f, 0.10f, 1.3f, 0.0f },
			{ "Dwarf.WarpTwist", 17, -1.5f, 1.5f, 1.0f, 0.0f },
			{ "Dwarf.Lopsided", 18, 0.1f, 0.6f, 1.0f, 0.0f },
			{ "Dwarf.Flare", 19, 0.0f, 0.6f, 1.0f, 0.0f },
			{ "Dwarf.ArmAsymmetry", 20, 0.3f, 0.9f, 1.0f, 0.0f },
			{ "Dwarf.ShapeSeed", 22, 0.0f, 16777215.0f, 1.0f, 1.0f },
		};
		A.Volume.bDust = false;
		A.Volume.DustTau = 0.0f;
		return A;
	}

	/** D-174: mean V-band flux per dwarf as the generator draws it (as GalaxyMeanFluxV). */
	inline double DwarfMeanFluxV()
	{
		static const double Value = []()
		{
			FUniverseDef D;
			D.Seed = 69;
			D.Archetypes.resize(5);
			D.Archetypes[0].Id = 0; D.Archetypes[0].Name = "Root";
			D.Archetypes[kStar] = MakeStar();
			D.Archetypes[kGalaxy] = MakeGalaxy();
			D.Archetypes[3].Id = 3; D.Archetypes[3].Name = "Unused";
			D.Archetypes[kDwarf] = MakeDwarf();
			const FUniverseModel M(D);
			return FUnresolvedTable::Build(M, kDwarf).MeanFluxV();
		}();
		return Value;
	}

	/** D-177: the faintest magnitude of the galaxy function's coarse, bright levels (>= the
	 *  level of an M* galaxy's next brighter slice): the cut between a catalogue cluster's
	 *  authored bright members and those its emitter places. Read from the galaxy slices, so it
	 *  follows the function: the faint edge of the second-coarsest galaxy level (L86: -20.82). */
	inline int BrightMemberMinLevel() { return 86; }
	inline double BrightMemberCut()
	{
		static const double Value = []()
		{
			FUniverseDef D;
			D.Seed = 69;
			D.Archetypes.resize(3);
			D.Archetypes[0].Id = 0; D.Archetypes[0].Name = "Root";
			D.Archetypes[kStar] = MakeStar();
			D.Archetypes[kGalaxy] = MakeGalaxy();
			const FUniverseModel M(D);
			double Cut = -20.82;
			for (const FSlice& S : M.SlicesOf(kGalaxy).Slices) { if (S.Level == BrightMemberMinLevel()) { Cut = S.FaintM; } }
			return Cut;
		}();
		return Value;
	}

	/** D-177: a catalogue cluster's fainter members (M fainter than BrightMemberCut): the galaxy
	 *  archetype with its function cut there, so they live only at the finer levels and their
	 *  cluster's reach is ~200 Mpc instead of ~800. Same look, morphology rule and stars. */
	inline FArchetype MakeMember()
	{
		FArchetype A = MakeGalaxy();
		A.Id = kMember;
		A.Name = "Member";
		A.AbsMag = FMagnitudeLut::Schechter(-21.0, -1.1, BrightMemberCut(), -16.0);
		return A;
	}

	/** Group / cluster field exemplar (units of the influence radius, 4 x the cluster radius). */
	inline FFieldParams ClusterExemplar()
	{
		FFieldParams P;
		P.V[0] = 0.1f;     // core radius
		P.V[1] = 0.85f;    // axis ratio b/a
		P.V[2] = 0.7f;     // axis ratio c/a
		P.V[3] = 1.0f;     // cut radius (the influence sphere)
		P.V[4] = 0.0f;     // clump count
		P.V[5] = 0.0f;     // clump weight
		P.V[6] = 0.04f;    // clump radius
		P.V[7] = 0.0f;     // clump seed
		P.V[8] = 0.8f;     // D-172: early-type share at the centre
		P.V[9] = 0.2f;     //        ... far out
		P.V[10] = 0.25f;   //        ... e-folding radius
		P.V[11] = -0.07f;  //        their colour shift (log10 K)
		return P;
	}

	/** D-151: galaxy groups and clusters. Extended (their light hands over to the volume as they
	 *  grow, D-129, like galaxies), and an emitter placing their member galaxies with a budget-
	 *  consistent count: a cluster's galaxies add up to its own magnitude (D-133). */
	inline FArchetype MakeCluster(const FClusterTuning& T = FClusterTuning())
	{
		FArchetype A;
		A.Id = kCluster;
		A.Name = "Cluster";
		A.Visibility = EUniVisibility::Extended;
		A.AbsMag = FMagnitudeLut::Schechter(T.MStar, T.Alpha, T.BrightMag, T.FaintMag);
		// The radius is where its light concentrates (about the half-light radius): 0.5 Mpc at
		// M = -26, R ~ L^0.5, so the mean surface brightness inside it is the same for every
		// cluster, ~25.4 mag/arcsec^2: at the naked-eye limit (25), visible with a little added
		// sensitivity. Members reach 4 radii (the influence sphere, ~ the virial radius).
		A.RadiusModel[0] = float(std::log10(0.5 * Units::Mpc * std::max(T.RadiusScale, 1.0e-3)));
		A.RadiusModel[1] = -0.2f; A.RadiusModel[2] = -26.0f; A.RadiusModel[3] = 0.1f;
		// D-151: below ~6 px a cluster counts as unresolved for residency (its relevance is then
		// set by its members, ~330 Mpc). Its light there is the volume's: the volume models every
		// child by its point flux, so this is consistent with it; at 1 px (the default) clusters
		// would be resident out to 1.6 Gpc, about 300,000 of them.
		A.ThetaMin = 5.0e-3;
		// Old stellar populations: redder than the field.
		A.ColourModel[0] = 3.70f; A.ColourModel[1] = 0.0f; A.ColourModel[2] = -26.0f; A.ColourModel[3] = 0.04f;

		FUniEmitter E;
		E.ChildArchetypeId = kGalaxy;
		E.FieldId = H::UNI_FIELD_CLUSTER;
		E.Exemplar = ClusterExemplar();
		E.Exemplar.V[8] = float(std::clamp(T.EarlyCore, 0.0, 1.0));
		E.Exemplar.V[9] = float(std::clamp(T.EarlyOuter, 0.0, 1.0));
		E.Exemplar.V[10] = float(std::max(T.EarlyScale, 1.0e-3));
		E.Exemplar.V[11] = float(T.EarlyColourShift);
		// Budget-consistent membership: N(M) galaxies of mean flux F add up to 10^(-0.4 M).
		E.NTotalRefMag = -26.0;
		E.NTotal = std::pow(10.0, -0.4 * E.NTotalRefMag) / GalaxyMeanFluxV();
		E.InfluenceMul = 4.0;
		E.bSampleBox = true;   // D-153: 2-5 Mpc clusters inside galaxy cells up to 50 Mpc
		E.Ranges = {
			{ "Cluster.Core", 0, float(T.CoreMin), float(T.CoreMax), 1.0f, 0.0f },
			{ "Cluster.AxisB", 1, 0.6f, 1.0f, 1.0f, 0.0f },
			{ "Cluster.AxisC", 2, 0.45f, 0.9f, 1.0f, 0.0f },
			{ "Cluster.Clumps", 4, 0.0f, float(std::max(T.MaxClumps, 0)), 1.0f, 1.0f },
			{ "Cluster.ClumpWeight", 5, 0.0f, float(std::max(T.ClumpWeight, 0.0)), 1.0f, 0.0f },
			{ "Cluster.ClumpRadius", 6, 0.025f, 0.06f, 1.0f, 0.0f },
			{ "Cluster.ClumpSeed", 7, 0.0f, 16777215.0f, 1.0f, 1.0f },
		};
		A.Emitters.push_back(E);
		return A;
	}

	/** Mean members per cluster over the cluster luminosity function. */
	inline double MeanClusterMembers(const FArchetype& Cluster)
	{
		const FUniEmitter& E = Cluster.Emitters[0];
		const FMagnitudeLut& L = Cluster.AbsMag;
		double Sum = 0.0, Norm = 0.0;
		for (size_t I = 0; I < L.Pdf.size(); ++I)
		{
			const double M = L.MAt(I);
			Sum += L.Pdf[I] * E.NTotal * std::pow(10.0, -0.4 * (M - E.NTotalRefMag));
			Norm += L.Pdf[I];
		}
		return Norm > 0.0 ? Sum / Norm : 0.0;
	}

	/** D-174: a galaxy group: its own light is its dwarfs' (so its magnitude sets their number,
	 *  budget-consistent as clusters' members are); 0.25 Mpc at M = -17, R ~ L^(1/3); its
	 *  dwarfs reach 3 radii. Its glow is far below any surface-brightness limit, so in practice
	 *  it is seen only through its galaxies; ThetaMin 0.02 keeps it resident only within ~20 Mpc
	 *  (its dwarfs are visible to a few Mpc). Placed only from the node list (UseDeliveredWeb). */
	inline FArchetype MakeGroup()
	{
		FArchetype A;
		A.Id = kGroup;
		A.Name = "Group";
		A.Visibility = EUniVisibility::Extended;
		A.AbsMag = FMagnitudeLut::Schechter(-17.0, -1.0, -20.0, -13.0);   // unused by the catalogue (entries carry magnitudes); keeps the model valid
		A.RadiusModel[0] = float(std::log10(0.25 * Units::Mpc)); A.RadiusModel[1] = -0.1333f; A.RadiusModel[2] = -17.0f; A.RadiusModel[3] = 0.08f;
		A.ColourModel[0] = 3.76f; A.ColourModel[1] = 0.0f; A.ColourModel[2] = -17.0f; A.ColourModel[3] = 0.04f;
		A.ThetaMin = 0.02;
		FUniEmitter E;
		E.ChildArchetypeId = kDwarf;
		E.FieldId = H::UNI_FIELD_CLUSTER;
		E.Exemplar = ClusterExemplar();
		E.Exemplar.V[0] = 0.15f;   // core
		E.Exemplar.V[8] = 0.0f;    // no morphology rule (D-172 is for cluster members)
		E.NTotalRefMag = -17.0;
		E.NTotal = std::pow(10.0, -0.4 * E.NTotalRefMag) / DwarfMeanFluxV();
		E.InfluenceMul = 3.0;
		E.bSampleBox = true;   // D-153: a ~1 Mpc group inside dwarf cells of a few Mpc
		E.Ranges = {
			{ "Group.Core", 0, 0.10f, 0.25f, 1.0f, 0.0f },
			{ "Group.AxisB", 1, 0.6f, 1.0f, 1.0f, 0.0f },
			{ "Group.AxisC", 2, 0.45f, 0.9f, 1.0f, 0.0f },
		};
		A.Emitters.push_back(E);
		return A;
	}

	inline FUniEmitter WebEmitter()
	{
		FUniEmitter E;
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
		E.NTotal = kGalaxyDensityPerMpc3 / std::pow(Units::Mpc, 3.0);   // galaxies per cm^3 over the LF range
		return E;
	}

	/** D-151: the root emitter for clusters: the galaxies' own web lattice (same G, widths and
	 *  seed), sharpened (BiasPower) and with nodes added, so clusters sit where the web is
	 *  densest. Its density makes the members Fraction of all galaxies. */
	inline FUniEmitter ClusterWebEmitter(const FClusterTuning& T)
	{
		FUniEmitter E = WebEmitter();
		E.ChildArchetypeId = kCluster;
		E.Exemplar.V[8] = float(T.BiasPower);
		E.Exemplar.V[9] = float(T.NodeWeight);
		const double Members = MeanClusterMembers(MakeCluster(T));
		E.NTotal = Members > 0.0 ? T.Fraction * kGalaxyDensityPerMpc3 / Members / std::pow(Units::Mpc, 3.0) : 0.0;
		return E;
	}

	/** The root emitters of a web scene: field galaxies, and (if enabled) clusters. */
	inline void AddWeb(FUniverseDef& D, const FClusterTuning& T)
	{
		const bool bClusters = T.bEnabled && T.Fraction > 0.0;
		D.Archetypes[kCluster] = MakeCluster(T);
		FUniEmitter Field = WebEmitter();
		if (bClusters) { Field.NTotal *= std::clamp(1.0 - T.Fraction, 0.0, 1.0); }
		D.RootEmitters.push_back(Field);
		if (bClusters) { D.RootEmitters.push_back(ClusterWebEmitter(T)); }
	}

	/** D-182 (owner): every galaxy (field, cluster member, dwarf) its own colour palette, which
	 *  its stars and its glow take (render chroma only: luminance kept). Returns the archetypes. */
	inline int UseGalaxyPalettes(FUniverseDef& D)
	{
		int N = 0;
		for (FArchetype& A : D.Archetypes) { if (A.Id == kGalaxy || A.Id == kMember || A.Id == kDwarf) { A.bPalette = true; ++N; } }
		return N;
	}

	/** D-179 (SPRITE_PASS_PLAN §3 A3): galaxies read the noise project's galaxy textures:
	 *  warp_fine displaces discs and arms, warp_large bulges, galaxy_detail modulates discs (fBm)
	 *  and arms (star-forming knots). Every galaxy rolls its amounts and frequencies by name;
	 *  dwarfs are more irregular; early types stay smooth; the Milky Way anchor has one fixed set.
	 *  TexSize: texels per texture period (all three textures the same size). Call only with the
	 *  textures bound (UniCore::BindGalaxyTexture): unbound, the terms read NaN and the galaxies
	 *  place nothing (fail closed). Returns the number of emitters and anchors switched. */
	inline int UseGalaxyTextures(FUniverseDef& D, uint32_t TexSize)
	{
		int N = 0;
		auto Add = [&](FUniEmitter& E, bool bDwarf)
		{
			if (E.FieldId != H::UNI_FIELD_GALAXY) { return; }
			E.Exemplar.V[31] = float(TexSize);
			const std::string P = bDwarf ? "DwarfTex." : "Tex.";
			// The warp moves the disc by about a tenth of its own wavelength (1 / (4 x freq): warp_fine
			// has 4 curl periods a tile), so the displacement bends structure without folding it
			// (folding would make the warp compress or stretch the disc, and its light with it).
			E.Ranges.push_back({ P + "DiscWarp", 24, bDwarf ? 0.012f : 0.008f, bDwarf ? 0.03f : 0.022f, 1.0f, 0.0f });
			E.Ranges.push_back({ P + "BulgeWarp", 25, 0.0f, 0.012f, 1.0f, 0.0f });
			E.Ranges.push_back({ P + "WarpFreq", 26, 0.8f, 1.6f, 1.0f, 0.0f });
			E.Ranges.push_back({ P + "DetailFreq", 27, 2.0f, 4.0f, 1.0f, 0.0f });
			E.Ranges.push_back({ P + "DiscNoise", 28, bDwarf ? 0.4f : 0.2f, bDwarf ? 0.8f : 0.6f, 1.0f, 0.0f });
			E.Ranges.push_back({ P + "Knots", 29, bDwarf ? 0.5f : 0.3f, bDwarf ? 1.0f : 0.9f, 1.0f, 0.0f });
			E.Ranges.push_back({ P + "Twist", 30, 0.5f, 1.0f, 1.0f, 0.0f });
			++N;
		};
		for (FArchetype& A : D.Archetypes)
		{
			if (A.Id != kGalaxy && A.Id != kMember && A.Id != kDwarf) { continue; }
			for (FUniEmitter& E : A.Emitters) { Add(E, A.Id == kDwarf); }
		}
		for (FAnchor& A : D.Anchors)
		{
			if (A.ArchetypeId != kGalaxy) { continue; }
			for (FFieldParams& P : A.EmitterParams)
			{
				P.V[24] = 0.015f; P.V[25] = 0.006f; P.V[26] = 1.2f; P.V[27] = 3.0f;   // the home galaxy's fixed set
				P.V[28] = 0.35f;  P.V[29] = 0.6f;   P.V[30] = 0.8f; P.V[31] = float(TexSize);
				++N;
			}
		}
		return N;
	}

	/** D-165: switch a web scene's root emitters to a delivered texture (the texture project's
	 *  adhesion web: R walls, G filaments, B nodes, A void, each a mass density in units of the
	 *  tile's mean). The tile becomes 2^89 cm (200 Mpc; periods are powers of two, the spec's
	 *  160 Mpc is not one). Field galaxies follow the walls, filaments and void matter
	 *  (R + G + A); clusters follow the node matter (B), and their members' share of all galaxies
	 *  is the node matter's share of the mass (NodeShare, from the texture's mean) instead of
	 *  uu.Cluster.Fraction. The texture must be bound (BindWebTexture) wherever these emitters
	 *  are sampled; unbound they read NaN and place nothing. Returns the emitters switched. */
	inline int UseDeliveredWeb(FUniverseDef& D, const FClusterTuning& T, uint32_t TexSize, double NodeShare, const std::vector<FWebNode>* Nodes = nullptr)
	{
		const bool bClusters = T.bEnabled && NodeShare > 0.0;
		const double Members = MeanClusterMembers(D.Arch(kCluster));
		if (bClusters && Nodes && !Nodes->empty())
		{
			// D-167: clusters from the node list, rank-mapped to the cluster luminosity function
			// (owner's choice): the tile holds K = (cluster density x tile volume) clusters, the K
			// heaviest nodes, the i-th heaviest at the LF's quantile (i + 1/2) / K, brightest first.
			// Members are uu.Cluster.Fraction of all galaxies again. Field galaxies follow the walls,
			// filaments, void matter and the node matter the clusters do not take (lighter nodes:
			// groups), uniformly scaled.
			const int P = 89;
			const double TileCm3 = std::pow(std::ldexp(1.0, P), 3.0);
			const double ClusterDensity = Members > 0.0 ? T.Fraction * kGalaxyDensityPerMpc3 / Members / std::pow(Units::Mpc, 3.0) : 0.0;
			const size_t K = std::min(Nodes->size(), size_t(std::llround(ClusterDensity * TileCm3)));
			const FMagnitudeLut& Lf = D.Arch(kCluster).AbsMag;
			FCatalogue Cat;
			Cat.ArchetypeId = kCluster;
			Cat.PeriodBits = P;
			double TopMass = 0.0;
			for (size_t I = 0; I < K; ++I)
			{
				const double U = (double(I) + 0.5) / double(K) * double(Lf.InvCdf.size() - 1);
				const size_t J = std::min(size_t(U), Lf.InvCdf.size() - 2);
				const double M = double(Lf.InvCdf[J]) + (U - double(J)) * (double(Lf.InvCdf[J + 1]) - double(Lf.InvCdf[J]));
				FCatalogueEntry E;
				for (int Ax = 0; Ax < 3; ++Ax) { E.Pos[Ax] = (*Nodes)[I].Pos[Ax]; }
				E.AbsMag = float(M);
				Cat.Entries.push_back(E);
				TopMass += (*Nodes)[I].Mass;
			}
			// D-177: the clusters' brightest members (the coarse galaxy levels, ~5%) are authored
			// with the catalogue; their emitter places the fainter ones (kMember), so a cluster is
			// woken and kept resident only as far as those finer windows reach.
			{
				FArchetype& Ca = D.Archetypes[kCluster];
				if (!Ca.Emitters.empty())
				{
					double PBright = 0.0;
					{
						FUniverseDef Dg;
						Dg.Seed = 69;
						Dg.Archetypes.resize(3);
						Dg.Archetypes[0].Id = 0; Dg.Archetypes[0].Name = "Root";
						Dg.Archetypes[kStar] = MakeStar();
						Dg.Archetypes[kGalaxy] = MakeGalaxy();
						const FUniverseModel Mg(Dg);
						for (const FSlice& Sl : Mg.SlicesOf(kGalaxy).Slices) { if (Sl.Level >= BrightMemberMinLevel()) { PBright += Sl.P; } }
					}
					FUniEmitter& Ce = Ca.Emitters[0];
					Ce.ChildArchetypeId = kMember;
					Ce.NTotal *= std::clamp(1.0 - PBright, 0.0, 1.0);
					Cat.bBrightMembers = PBright > 0.0;
					Cat.MemberArchetypeId = kGalaxy;
					Cat.MemberMinLevel = BrightMemberMinLevel();
					Cat.BrightRatio = PBright < 1.0 ? PBright / (1.0 - PBright) : 0.0;
					Cat.BcgShare = std::clamp(T.BcgShare, 0.0, 0.5);   // D-191
					Cat.IclShare = std::clamp(T.IclShare, 0.0, 0.5);
					Cat.BcgMaxMag = float(T.BcgMaxMag);
				}
			}
			D.Catalogues.push_back(Cat);
			// D-174: the remaining nodes are groups; each node's dwarfs go with its mass (the
			// median-mass group holds T.GroupDwarfs), and the group's magnitude is their light.
			// D-177 (owner): groups are labels only (named, uu.GotoGroup); their dwarfs come from
			// the web's node matter directly (a root emitter), with the same total.
			if (T.bGroups && T.GroupDwarfs > 0.0 && K < Nodes->size())
			{
				std::vector<double> Ms;
				for (size_t I = K; I < Nodes->size(); ++I) { Ms.push_back((*Nodes)[I].Mass); }
				std::vector<double> Sorted = Ms;
				std::nth_element(Sorted.begin(), Sorted.begin() + Sorted.size() / 2, Sorted.end());
				const double Median = std::max(Sorted[Sorted.size() / 2], 1.0e-30);
				const double F = DwarfMeanFluxV();
				FCatalogue Groups;
				Groups.ArchetypeId = kGroup;
				Groups.PeriodBits = P;
				for (size_t I = K; I < Nodes->size(); ++I)
				{
					const double N = T.GroupDwarfs * (*Nodes)[I].Mass / Median;
					if (!(N * F > 0.0)) { continue; }
					FCatalogueEntry E;
					for (int Ax = 0; Ax < 3; ++Ax) { E.Pos[Ax] = (*Nodes)[I].Pos[Ax]; }
					E.AbsMag = float(-2.5 * std::log10(N * F));
					Groups.Entries.push_back(E);
				}
				if (!Groups.Entries.empty())
				{
					D.Catalogues.push_back(Groups);
					D.Archetypes[kGroup].Emitters.clear();   // a label: no contributor, no bookkeeping
					double Dwarfs = 0.0;
					for (const FCatalogueEntry& E : Groups.Entries) { Dwarfs += std::pow(10.0, -0.4 * double(E.AbsMag)) / F; }
					FUniEmitter De;
					De.ChildArchetypeId = kDwarf;
					De.FieldId = H::UNI_FIELD_WEB;
					De.bDensityMode = true;
					De.PeriodBits = P;
					De.Exemplar.V[0] = 1.0f;
					De.Exemplar.V[1] = 0.12f;
					De.Exemplar.V[9] = 1.0f;                // node matter only (groups' and clusters' alike)
					De.Exemplar.V[10] = 1.0f;               // textured
					De.Exemplar.V[11] = float(TexSize);
					De.NTotal = Dwarfs / TileCm3;           // the dwarfs the groups held, per cm^3
					D.RootEmitters.push_back(De);
				}
			}
			D.RootEmitters.erase(std::remove_if(D.RootEmitters.begin(), D.RootEmitters.end(),
				[](const FUniEmitter& E) { return E.FieldId == H::UNI_FIELD_WEB && E.ChildArchetypeId == kCluster; }), D.RootEmitters.end());
			int N = 0;
			for (FUniEmitter& E : D.RootEmitters)
			{
				if (E.FieldId != H::UNI_FIELD_WEB || E.ChildArchetypeId == kDwarf) { continue; }   // D-177: the dwarfs' emitter is set above
				E.PeriodBits = P;
				E.Exemplar = FFieldParams();
				E.Exemplar.V[0] = 1.0f;
				E.Exemplar.V[1] = 0.12f;
				E.Exemplar.V[3] = 1.0f;                                                     // walls
				E.Exemplar.V[4] = 1.0f;                                                     // filaments
				E.Exemplar.V[9] = float(std::clamp(1.0 - TopMass / NodeShare, 0.0, 1.0));   // the node matter the clusters do not take
				E.Exemplar.V[10] = 1.0f;
				E.Exemplar.V[11] = float(TexSize);
				E.Exemplar.V[12] = 1.0f;                                                    // void matter
				E.NTotal = kGalaxyDensityPerMpc3 * std::clamp(1.0 - T.Fraction, 0.0, 1.0) / std::pow(Units::Mpc, 3.0);
				++N;
			}
			return N;
		}
		int N = 0;
		for (FUniEmitter& E : D.RootEmitters)
		{
			if (E.FieldId != H::UNI_FIELD_WEB) { continue; }
			E.PeriodBits = 89;
			E.Exemplar = FFieldParams();
			E.Exemplar.V[0] = 1.0f;                 // G, width: unused by the texture (kept valid)
			E.Exemplar.V[1] = 0.12f;
			E.Exemplar.V[10] = 1.0f;                // textured
			E.Exemplar.V[11] = float(TexSize);
			if (E.ChildArchetypeId == kCluster)
			{
				E.Exemplar.V[9] = 1.0f;             // node matter only
				E.NTotal = bClusters && Members > 0.0 ? NodeShare * kGalaxyDensityPerMpc3 / Members / std::pow(Units::Mpc, 3.0) : 0.0;
			}
			else
			{
				E.Exemplar.V[3] = 1.0f;             // walls
				E.Exemplar.V[4] = 1.0f;             // filaments
				E.Exemplar.V[12] = 1.0f;            // void matter
				E.NTotal = kGalaxyDensityPerMpc3 * (bClusters ? 1.0 - NodeShare : 1.0) / std::pow(Units::Mpc, 3.0);
			}
			++N;
		}
		return N;
	}

	/** D-197: thin a definition's children (FUniEmitter::SpriteKeep): stars (every emitter of
	 *  kStar) by StarKeep; cluster members (every emitter of a kCluster) and group dwarfs (every
	 *  emitter of kDwarf, root or not) by MemberKeep. Field galaxies are not thinned. */
	inline void UseSpriteKeep(FUniverseDef& D, double StarKeep, double MemberKeep)
	{
		auto Apply = [&](FUniEmitter& E, u32 ParentArch)
		{
			if (E.ChildArchetypeId == kStar) { E.SpriteKeep = std::clamp(StarKeep, 0.05, 1.0); }
			else if (ParentArch == kCluster || E.ChildArchetypeId == kDwarf) { E.SpriteKeep = std::clamp(MemberKeep, 0.05, 1.0); }
		};
		for (FUniEmitter& E : D.RootEmitters) { Apply(E, ~0u); }
		for (FArchetype& A : D.Archetypes) { for (FUniEmitter& E : A.Emitters) { Apply(E, A.Id); } }
	}

	/** D-198: every dusty archetype's dust x DustScale, its textured lanes' contrast DustLanes. */
	inline void UseDustTuning(FUniverseDef& D, double DustScale, double DustLanes)
	{
		for (FArchetype& A : D.Archetypes)
		{
			if (!A.Volume.bDust || !(A.Volume.DustTau > 0.0f)) { continue; }
			A.Volume.DustTau = float(double(A.Volume.DustTau) * std::clamp(DustScale, 0.0, 10.0));
			A.Volume.DustLanes = float(std::clamp(DustLanes, 0.0, 10.0));
		}
	}

	inline FUniverseDef Base()
	{
		FUniverseDef D;
		D.Seed = 69;   // UltraLargeScale's universe root seed
		D.Archetypes.resize(7);
		D.Archetypes[0].Id = 0; D.Archetypes[0].Name = "Root";
		D.Archetypes[kStar] = MakeStar();
		D.Archetypes[kGalaxy] = MakeGalaxy();
		D.Archetypes[kCluster] = MakeCluster();   // placed only by web scenes (AddWeb, with their tuning)
		D.Archetypes[kDwarf] = MakeDwarf();       // D-174: placed only by groups
		D.Archetypes[kGroup] = MakeGroup();       // D-174: placed only from the node list
		D.Archetypes[kMember] = MakeMember();     // D-177: catalogue clusters' fainter members
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
		// D-178 (owner): one fixed, plausible parameter set for the home galaxy; every other galaxy
		// rolls its own. A gentle warp (~0.9 kpc at the solar circle's outer disc), slight
		// lopsidedness and flare, mildly uneven arms.
		FFieldParams P = GalaxyExemplar();
		P.V[16] = 0.09f;        // warp amplitude
		P.V[17] = 0.4f;         // warp twist
		P.V[18] = 0.08f;        // lopsidedness
		P.V[19] = 0.5f;         // flare
		P.V[20] = 0.2f;         // arm asymmetry
		P.V[21] = 0.15f;        // pitch tightening
		P.V[22] = 5313203.0f;   // shape seed
		A.EmitterParams = { P };
		A.bFixedOrientation = true;
		A.Seed = 0xA2C40001u;
		return A;
	}

	inline FUniverseDef WebToGalaxy(const FClusterTuning& T = FClusterTuning())
	{
		FUniverseDef D = Base();
		AddWeb(D, T);
		return D;
	}

	inline FUniverseDef GalaxyToStar()
	{
		FUniverseDef D = Base();
		D.Anchors.push_back(MilkyWayAnchor());
		return D;
	}

	inline FUniverseDef Flythrough(const FClusterTuning& T = FClusterTuning())
	{
		FUniverseDef D = Base();
		AddWeb(D, T);
		D.Anchors.push_back(MilkyWayAnchor());
		// No cluster within 5 Mpc of the anchor (its members would cross it).
		FSuppressor Sc;
		Sc.Centre = FCoord::Origin();
		Sc.RadiusCm = 5.0 * Units::Mpc;
		Sc.ArchetypeId = kCluster;
		D.Suppressors.push_back(Sc);
		FSuppressor S;
		S.Centre = FCoord::Origin();
		S.RadiusCm = 300.0 * Units::Kpc;
		S.ArchetypeId = kGalaxy;
		D.Suppressors.push_back(S);
		// D-174: no group dwarf inside the anchor's disc (satellites beyond 50 kpc are fine).
		FSuppressor Sd;
		Sd.Centre = FCoord::Origin();
		Sd.RadiusCm = 50.0 * Units::Kpc;
		Sd.ArchetypeId = kDwarf;
		D.Suppressors.push_back(Sd);
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
