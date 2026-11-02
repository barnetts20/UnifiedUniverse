// test_parallax.cpp
// M3 / D-146: actors placed by the parallax rule agree with the backdrop sprites (direction,
// angular size, render distance) at every distance, and the real-space switch is stable.

#include "../src/UniTest.h"
#include "../scenes/UniScenes.h"
#include "UniParallax.h"
#include "UniGenRef.h"

using namespace UniCore;
namespace H = UniHlsl;

UNI_TEST(parallax, equivalence)
{
	// Random objects from 10 m to 10^27 cm around a camera at a random coordinate with a sub-cm
	// fraction. The sprite path's render position is dir * f(r) from the exact cell delta
	// (Uni_ObjectRelative, as the GPU computes it); the actor's is ParallaxPlace's offset.
	Uni::FTestRng Rng(0x9A7A11Aull);
	double MaxAngle = 0.0, MaxDistRel = 0.0, MaxAngSize = 0.0;
	long long N = 0, Real = 0;
	for (int I = 0; I < 200000; ++I)
	{
		FCoord Cam = FCoord::Origin();
		FVec3f CamFrac{ float(Rng.Unit()), float(Rng.Unit()), float(Rng.Unit()) };
		{
			FVec3f Discard{};
			AddDisplacement(Cam, Discard, FVec3d{ (Rng.Unit() - 0.5) * 1.0e30, (Rng.Unit() - 0.5) * 1.0e30, (Rng.Unit() - 0.5) * 1.0e30 });
		}
		const double Dist = std::exp(std::log(1.0e3) + Rng.Unit() * (std::log(1.0e27) - std::log(1.0e3)));
		const double Th = std::acos(2.0 * Rng.Unit() - 1.0), Ph = 6.283185307179586 * Rng.Unit();
		const FVec3d Dir{ std::sin(Th) * std::cos(Ph), std::sin(Th) * std::sin(Ph), std::cos(Th) };
		FCoord Obj = Cam;
		FVec3f Of{};
		AddDisplacement(Obj, Of, FVec3d{ Dir.X * Dist, Dir.Y * Dist, Dir.Z * Dist });
		const double Radius = Dist * std::exp(std::log(1.0e-6) + Rng.Unit() * (std::log(0.5) - std::log(1.0e-6)));
		// Sprite path: the object as a record in a level cell, through the GPU's core.
		const int L = std::max(LevelForRelevance(Dist * 1.5), 20);
		const FCellKey K = CellOf(Obj, L);
		const FCoord O = CellOrigin(K);
		u32 Off[3];
		for (int A = 0; A < 3; ++A) { const FU128 Rl = Sub(Obj.Axis(A), O.Axis(A)); Off[A] = L >= 32 ? Shr(Rl, L - 32).L[3] : Shl(Rl, 32 - L).L[3]; }
		const FCoord Snapped = ObjectCoord(K, Off);   // the record's own coordinate (offset quantisation)
		const FDelta Dd = Sub(O, Cam);
		H::uint4 D[3];
		for (int A = 0; A < 3; ++A) { const FU128 T = Dd.TwosComplement(A); D[A] = H::uint4(T.L[0], T.L[1], T.L[2], T.L[3]); }
		const H::float3 Rel = H::Uni_ObjectRelative(D[0], D[1], D[2], H::uint3(Off[0], Off[1], Off[2]), u32(L), H::float3(CamFrac.X, CamFrac.Y, CamFrac.Z));
		const double Rs = std::sqrt(double(Rel.x) * Rel.x + double(Rel.y) * Rel.y + double(Rel.z) * Rel.z);
		const double SpriteRender = double(H::Uni_Compress(float(Rs)));
		// Actor path.
		const FParallaxPlacement P = ParallaxPlace(Snapped, Radius, Cam, CamFrac, false);
		const double Lo = Length(P.Offset);
		if (Lo <= 0.0 || Rs <= 0.0) { continue; }
		++N;
		Real += P.bRealSpace ? 1 : 0;
		const double Cos = (P.Offset.X * Rel.x + P.Offset.Y * Rel.y + P.Offset.Z * Rel.z) / (Lo * Rs);
		MaxAngle = std::max(MaxAngle, std::acos(std::clamp(Cos, -1.0, 1.0)));
		if (!P.bRealSpace) { MaxDistRel = std::max(MaxDistRel, std::abs(Lo / SpriteRender - 1.0)); }
		// Angular size: scaled radius over render distance equals true radius over true distance.
		MaxAngSize = std::max(MaxAngSize, std::abs((P.Scale * Radius / Lo) / (Radius / P.Distance) - 1.0));
	}
	Uni::Check(2, "T2.parallax.equivalence", "Actors placed by the parallax rule agree with the sprite path: direction within 2e-6 rad, render distance within 1e-5 (compressed), angular size exact to 1e-12, 10 m to 10^27 cm")
		.Metric("samples", double(N)).Metric("real_space", double(Real)).Metric("max_angle", MaxAngle).Metric("max_dist_rel", MaxDistRel).Metric("max_angular_size", MaxAngSize)
		.Pass(N > 150000 && Real > 1000 && MaxAngle < 2.0e-6 && MaxDistRel < 1.0e-5 && MaxAngSize < 1.0e-12,
			Uni::Fmt("%lld objects (%lld in real space): worst direction %.1e rad, render distance %.1e, angular size %.1e", N, Real, MaxAngle, MaxDistRel, MaxAngSize));
}

UNI_TEST(parallax, realspace)
{
	// Approach a Jupiter-sized body from 10^13 cm to 1,000 km above its surface and back, with a
	// wobble across the r0 edge. The real-space switch must flip exactly once each way (1.25x
	// hysteresis), angular size must be continuous throughout, and in real space the scale is 1.
	const double R = 6.9911e9;
	const FCoord Body = FCoord::Origin();
	int Flips = 0;
	bool Was = false, First = true;
	double MaxJump = 0.0, PrevAng = 0.0, WorstRealScale = 0.0;
	const int Frames = 4000;
	for (int F = 0; F <= Frames; ++F)
	{
		const double U = double(F) / Frames;   // in, then out
		const double Lg = U < 0.5 ? (13.0 + (std::log10(R + 1.0e8) - 13.0) * (U * 2.0)) : (std::log10(R + 1.0e8) + (13.0 - std::log10(R + 1.0e8)) * ((U - 0.5) * 2.0));
		const double Dc = std::pow(10.0, Lg) * (1.0 + 0.02 * std::sin(F * 0.9));
		FCoord Cam = Body;
		FVec3f Fr{};
		AddDisplacement(Cam, Fr, FVec3d{ -Dc * 0.6, Dc * 0.8, 0.0 });
		const FParallaxPlacement P = ParallaxPlace(Body, R, Cam, FVec3f{}, Was);
		if (!First && P.bRealSpace != Was) { ++Flips; }
		if (P.bRealSpace) { WorstRealScale = std::max(WorstRealScale, std::abs(P.Scale - 1.0)); }
		const double Ang = P.Scale * R / Length(P.Offset);
		if (!First) { MaxJump = std::max(MaxJump, std::abs(Ang / PrevAng - 1.0)); }
		PrevAng = Ang;
		Was = P.bRealSpace;
		First = false;
	}
	// The path's own frame-to-frame change in angular size, for reference.
	Uni::Check(2, "T2.parallax.realspace", "Real-space switch near a body: exactly 2 flips over an approach and departure with wobble at the edge; angular size continuous; scale 1 in real space")
		.Metric("flips", Flips).Metric("max_frame_change", MaxJump).Metric("real_scale_err", WorstRealScale)
		.Pass(Flips == 2 && WorstRealScale == 0.0 && MaxJump < 0.06,
			Uni::Fmt("%d switches between compressed and real space; worst frame-to-frame angular size change %.2f%% (the path's own motion, no jump at the switch); real-space scale error %.1e", Flips, 100.0 * MaxJump, WorstRealScale));
}

#include "UniPromotion.h"

UNI_TEST(parallax, promotion)
{
	// D-147: approach a gas giant from 300 radii to 1,000 km above its surface and back, while
	// its orbit moves. Every frame exactly one of {sprite, actor} represents it (sprite visible
	// through the GPU's projection core, or record flagged promoted with an actor placed); it
	// swaps once each way; at the swap the actor sits where the sprite was.
	FObjectGPU Star{};
	FSystem Sys;
	u32 Seed = 1;
	for (;; ++Seed)
	{
		Star = FromHlsl(H::Uni_MakeObject(H::uint3(0u, 0u, 0u), UniScenes::kStar, 0u, Seed, float(std::log2(6.957e10)), 4.83f, 0.0f, H::float3(1.0f, 0.96f, 0.9f)));
		Sys = BuildSystem(Star);
		bool G = false;
		for (const FPlanet& P : Sys.Planets) { G = G || P.Type == EPlanetType::GasGiant; }
		if (G) { break; }
	}
	const FPlanet* Giant = nullptr;
	for (const FPlanet& P : Sys.Planets) { if (P.Type == EPlanetType::GasGiant) { Giant = &P; break; } }
	const FCellKey StarCell = CellOf(FCoord::Origin(), 60);
	const FCoord StarC = CellOrigin(StarCell);
	FSystemManager Mgr;
	{
		FCoord Near = StarC;
		FVec3f F{};
		AddDisplacement(Near, F, FVec3d{ Sys.SpanCm * 0.5, 0.0, 0.0 });
		Mgr.Report(StarCell, Star, Near, 0);
	}
	FPromotionManager Prom;
	const double R = Giant->RadiusCm;
	int Flips = 0, Both = 0, Neither = 0, FlagMismatch = 0;
	bool Was = false;
	double SwapAngle = 0.0, GlareRatio = 0.0, PrevRatio = 0.0;
	H::float3 LastSpriteDir(0.0f, 0.0f, 0.0f);
	const int Frames = 3000;
	for (int Fr = 0; Fr <= Frames; ++Fr)
	{
		const double T = 1.0e7 + 600.0 * Fr;   // the planet moves ~ its own radius every few frames
		const double U = double(Fr) / Frames;
		const double Lo = std::log10(R + 1.0e8), Hi = std::log10(300.0 * R);
		const double Lg = U < 0.5 ? Hi + (Lo - Hi) * (2.0 * U) : Lo + (Hi - Lo) * (2.0 * U - 1.0);
		FCoord Pc = StarC;
		FVec3f Pf{};
		const FVec3d Off = PlanetOffset(*Giant, T);
		AddDisplacement(Pc, Pf, Off);
		// Camera on the day side, slightly above the orbit plane.
		const double Lof = Length(Off);
		const FVec3d Out{ -Off.X / Lof, -Off.Y / Lof + 0.2, 0.3 };
		const double Lout = Length(Out);
		const double Dc = std::pow(10.0, Lg);
		FCoord Cam = Pc;
		FVec3f Cf{};
		AddDisplacement(Cam, Cf, FVec3d{ Out.X / Lout * Dc, Out.Y / Lout * Dc, Out.Z / Lout * Dc });
		Mgr.Update(Cam);
		Prom.Update(Mgr, Cam, T, u64(Fr));
		const std::set<u32> Culled = Prom.CulledSpriteIds();
		std::vector<FCellGPU> Cells;
		std::vector<FObjectGPU> Recs;
		Mgr.Build(Cam, T, 0u, 256u, Cells, Recs, &Culled);
		// The giant's record and its sprite, looking straight at it.
		size_t K = 0;
		for (; K < Sys.Planets.size(); ++K) { if (Sys.Planets[K].Seed == Giant->Seed) { break; } }
		const FCellGPU& C = Cells[K];   // one cell per planet
		const FObjectGPU& O = Recs[C.PoolStart];
		const H::uint4 Dx(C.DeltaX[0], C.DeltaX[1], C.DeltaX[2], C.DeltaX[3]);
		const H::uint4 Dy(C.DeltaY[0], C.DeltaY[1], C.DeltaY[2], C.DeltaY[3]);
		const H::uint4 Dz(C.DeltaZ[0], C.DeltaZ[1], C.DeltaZ[2], C.DeltaZ[3]);
		const H::float3 Rel = H::Uni_ObjectRelative(Dx, Dy, Dz, H::uint3(O.A[0], O.A[1], O.A[2]), C.Level, H::float3(0.0f, 0.0f, 0.0f));
		const double Lr = std::sqrt(double(Rel.x) * Rel.x + double(Rel.y) * Rel.y + double(Rel.z) * Rel.z);   // double: a float norm alone is 1e-7 off
		H::FUniView V{};
		V.Forward = H::float3(float(Rel.x / Lr), float(Rel.y / Lr), float(Rel.z / Lr));
		const FVec3d Up0{ 0.0, 0.0, 1.0 };
		FVec3d Rt{ V.Forward.y * Up0.Z - V.Forward.z * Up0.Y, V.Forward.z * Up0.X - V.Forward.x * Up0.Z, V.Forward.x * Up0.Y - V.Forward.y * Up0.X };
		const double Lrt = Length(Rt);
		V.Right = H::float3(float(Rt.X / Lrt), float(Rt.Y / Lrt), float(Rt.Z / Lrt));
		V.Up = H::float3(V.Right.y * V.Forward.z - V.Right.z * V.Forward.y, V.Right.z * V.Forward.x - V.Right.x * V.Forward.z, V.Right.x * V.Forward.y - V.Right.y * V.Forward.x);
		V.TanHalfFovX = 1.0f; V.TanHalfFovY = 0.5625f;
		V.LimitingMag = H::Uni_LimitingMag(0.0f); V.SurfaceLimit = H::UNI_MU_REF; V.ExposureMag = V.LimitingMag - 1.0f;
		V.ThetaPixel = 1.0f / 960.0f; V.GlareGain = 0.35f;
		const H::FUniSprite S = H::Uni_ProjectObject(Dx, Dy, Dz, C.Level, 1.0f, ToHlsl(O), V, Prom.IsPromoted(Giant->Seed));
		const bool bSprite = (S.Flags & H::UNI_SPRITE_VISIBLE) != 0u;
		const bool bActor = Prom.IsPromoted(Giant->Seed);
		const bool bFlag = ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_PROMOTED) != 0u;
		FlagMismatch += bFlag != bActor ? 1 : 0;
		Both += (bSprite && bActor) ? 1 : 0;
		Neither += (!bSprite && !bActor) ? 1 : 0;
		if (Fr > 0 && bActor != Was)
		{
			++Flips;
			if (bActor)
			{
				// Actor placement against last frame's sprite direction is the motion of one frame;
				// compare with this frame's record direction instead (same frame, same camera).
				const FParallaxPlacement P = ParallaxPlace(Prom.Promoted().at(Giant->Seed).Coord, R, Cam, FVec3f{}, false);
				const double Lp = Length(P.Offset);
				const double Cos = (P.Offset.X * Rel.x + P.Offset.Y * Rel.y + P.Offset.Z * Rel.z) / (Lp * Lr);
				SwapAngle = std::acos(std::clamp(Cos, -1.0, 1.0));
				if (std::getenv("UNI_DEBUG_SWAP"))
				{
					const FVec3d Ex = ToDouble(Sub(Pc, Cam));
					std::printf("swap: offset %.6e %.6e %.6e | rel %.6e %.6e %.6e | exact %.6e %.6e %.6e | level %u\n", P.Offset.X / P.Scale, P.Offset.Y / P.Scale, P.Offset.Z / P.Scale, Rel.x, Rel.y, Rel.z, Ex.X, Ex.Y, Ex.Z, C.Level);
				}
			}
		}
		const double Ratio = (!bActor && bSprite) ? double(S.AngularRadius) / std::max(double(S.PhysAngularRadius), 1.0e-30) : 0.0;
		if (Fr > 0 && bActor && !Was) { GlareRatio = PrevRatio; }   // the frame just before the swap in
		PrevRatio = Ratio;
		Was = bActor;
	}
	Uni::Check(2, "T2.parallax.promotion", "Planet hand-off sprite -> actor (D-147, D-148): exactly one drawn every frame, one swap each way, record flag matches the actor, actor where the sprite was (< 1e-6 rad), sprite the body's size at the swap (within 1%)")
		.Metric("flips", Flips).Metric("both", Both).Metric("neither", Neither).Metric("flag_mismatch", FlagMismatch).Metric("swap_angle", SwapAngle).Metric("glare_to_body_at_swap", GlareRatio)
		.Pass(Flips == 2 && Both == 0 && Neither == 0 && FlagMismatch == 0 && SwapAngle < 1.0e-6 && std::abs(GlareRatio - 1.0) < 0.01,
			Uni::Fmt("%d swaps; frames drawn twice %d, not drawn %d; flag mismatches %d; actor vs sprite direction at the swap %.1e rad; sprite size / body size just before the swap %.3f (D-148)",
				Flips, Both, Neither, FlagMismatch, SwapAngle, GlareRatio));
}


UNI_TEST(parallax, sprite_profile)
{
	// D-148: the sprite profile sums to 1 over the pixels at any size and disc fraction (the
	// pixel shader relies on it: no per-sprite normalisation on the GPU), and the size blend
	// reaches the body's true size at theta_promote.
	Uni::FTestRng Rng(0x5915ull);
	double Worst = 0.0;
	int Cases = 0;
	for (int I = 0; I < 400; ++I)
	{
		const float Sigma = float(std::exp(std::log(0.5) + Rng.Unit() * (std::log(80.0) - std::log(0.5))));
		const float Fracs[4] = { 0.0f, 0.3f, 0.7f, 1.0f };
		const float F = Fracs[I % 4];
		if (F > 0.0f && F * Sigma < H::UNI_SPRITE_DISC_MIN_PX) { continue; }
		const double Cx = Rng.Unit(), Cy = Rng.Unit();
		const int Rad = int(std::ceil(std::max(5.0 * Sigma, double(F * Sigma) + 3.0)));
		double Sum = 0.0;
		for (int Y = -Rad; Y <= Rad; ++Y) for (int X = -Rad; X <= Rad; ++X)
		{
			const double Ex = X + 0.5 - Cx, Ey = Y + 0.5 - Cy;
			Sum += double(H::Uni_SpriteProfile(float(std::sqrt(Ex * Ex + Ey * Ey)), Sigma, F));
		}
		// A Gaussian narrower than a pixel is under-sampled at pixel centres: judge those only
		// from sigma >= 0.8 px (the shader clamps sigma to 0.5 px; sub-pixel stars are not
		// energy-critical, the volume and fade bands are).
		if (F == 0.0f && Sigma < 0.8f) { continue; }
		Worst = std::max(Worst, std::abs(Sum - 1.0));
		++Cases;
	}
	// The blend: a body's sprite at theta_promote has exactly its physical size.
	H::FUniView V{};
	V.Forward = H::float3(1, 0, 0); V.Right = H::float3(0, 1, 0); V.Up = H::float3(0, 0, 1);
	V.TanHalfFovX = 1.0f; V.TanHalfFovY = 0.5625f;
	V.LimitingMag = H::Uni_LimitingMag(0.0f); V.SurfaceLimit = H::UNI_MU_REF; V.ExposureMag = V.LimitingMag - 1.0f;
	V.ThetaPixel = 1.0f / 960.0f; V.GlareGain = 0.35f;
	double WorstAtPromote = 0.0, GlareAtQuarter = 0.0;
	for (int K = 0; K < 2; ++K)
	{
		const double Theta = K == 0 ? double(H::UNI_THETA_PROMOTE) * 0.999 : double(H::UNI_THETA_PROMOTE) * 0.2;
		const double R = 7.0e9, D = R / Theta;
		const FCellKey Kc = CellOf(FCoord::Origin(), 50);
		FCoord P = CellOrigin(Kc);
		FVec3f Fr{};
		AddDisplacement(P, Fr, FVec3d{ 1.0e12, 0.0, 0.0 });
		FCoord Cam = P;
		AddDisplacement(Cam, Fr, FVec3d{ -D, 0.0, 0.0 });
		const FCellKey Kp = CellOf(P, 40);
		const FCoord O = CellOrigin(Kp);
		u32 Off[3];
		for (int A = 0; A < 3; ++A) { Off[A] = Shr(Sub(P.Axis(A), O.Axis(A)), 8).L[3]; }
		const FDelta Dd = Sub(O, Cam);
		H::uint4 Dl[3];
		for (int A = 0; A < 3; ++A) { const FU128 T = Dd.TwosComplement(A); Dl[A] = H::uint4(T.L[0], T.L[1], T.L[2], T.L[3]); }
		const H::FUniObject Obj = H::Uni_MakeObject(H::uint3(Off[0], Off[1], Off[2]), H::UNI_ARCH_DYNAMIC_PLANET, H::UNI_OBJ_FLAG_EXTENDED, 7u, float(std::log2(R)), -10.0f, H::Uni_SurfaceBrightness(-10.0f, float(std::log2(R))), H::float3(1, 1, 1));
		const H::FUniSprite S = H::Uni_ProjectObject(Dl[0], Dl[1], Dl[2], 40u, 1.0f, Obj, V, false);
		if (K == 0) { WorstAtPromote = std::abs(double(S.AngularRadius) / double(S.PhysAngularRadius) - 1.0); }
		else { GlareAtQuarter = double(S.AngularRadius) / double(S.PhysAngularRadius); }
	}
	Uni::Check(1, "T1.sprite.profile", "Sprite profile (D-148): sums to 1 over pixels within 2% for Gaussian, disc + halo and full disc, 0.8-80 px; a body's sprite is its true size at theta_promote")
		.Metric("cases", Cases).Metric("worst_energy_err", Worst).Metric("size_err_at_promote", WorstAtPromote).Metric("glare_ratio_at_fifth", GlareAtQuarter)
		.Pass(Cases > 200 && Worst < 0.02 && WorstAtPromote < 0.005,
			Uni::Fmt("%d profiles: worst energy error %.2f%%; at theta_promote the sprite is %.3f%% off the body's size (at a fifth of it, still glare-sized: %.2fx)", Cases, 100.0 * Worst, 100.0 * WorstAtPromote, GlareAtQuarter));
}

#include "UniLocalFrame.h"

UNI_TEST(parallax, recentre)
{
	// D-149: a pawn wandering in Local mode, re-centred whenever it is more than 1,000 km from
	// the anchor. The universal camera must equal the exactly accumulated path at every frame,
	// must not move at all across a re-centre, and a body's local position must shift by exactly
	// the integer shift.
	Uni::FTestRng Rng(0x2EC3ull);
	FCoord Anchor = FCoord::Origin();
	{
		FVec3f D{};
		AddDisplacement(Anchor, D, FVec3d{ 3.3e26, -1.7e25, 9.1e24 });   // somewhere far out
	}
	FVec3d Pawn{ 0.0, 0.0, 0.0 };
	FCoord Truth = Anchor;
	FVec3f TruthFrac{};
	FCoord Body = Anchor;   // a body 20,000 km from the anchor
	{
		FVec3f D{};
		AddDisplacement(Body, D, FVec3d{ 2.0e9, 0.0, 0.0 });
	}
	int Recentres = 0;
	double WorstDrift = 0.0, WorstJump = 0.0, WorstBody = 0.0;
	for (int F = 0; F < 200000; ++F)
	{
		const FVec3d Step{ (Rng.Unit() - 0.3) * 4.0e5, (Rng.Unit() - 0.45) * 4.0e5, (Rng.Unit() - 0.5) * 1.0e5 };
		Pawn = FVec3d{ Pawn.X + Step.X, Pawn.Y + Step.Y, Pawn.Z + Step.Z };
		AddDisplacement(Truth, TruthFrac, Step);
		FVec3f Fr{};
		const FCoord Cam = LocalToUniversal(Anchor, Pawn, Fr);
		const FVec3d Dv = ToDouble(Sub(Cam, Truth));
		WorstDrift = std::max(WorstDrift, Length(FVec3d{ Dv.X + Fr.X - TruthFrac.X, Dv.Y + Fr.Y - TruthFrac.Y, Dv.Z + Fr.Z - TruthFrac.Z }));
		if (Length(Pawn) > 1.0e8)
		{
			const FVec3d BodyLocalBefore = ToDouble(Sub(Body, Anchor));
			const FVec3d S = RecentreShift(Pawn);
			ShiftAnchor(Anchor, S);
			Pawn = FVec3d{ Pawn.X - S.X, Pawn.Y - S.Y, Pawn.Z - S.Z };
			FVec3f Fa{};
			const FCoord After = LocalToUniversal(Anchor, Pawn, Fa);
			const FVec3d J = ToDouble(Sub(After, Cam));
			WorstJump = std::max(WorstJump, Length(FVec3d{ J.X + Fa.X - Fr.X, J.Y + Fa.Y - Fr.Y, J.Z + Fa.Z - Fr.Z }));
			const FVec3d BodyLocalAfter = ToDouble(Sub(Body, Anchor));
			WorstBody = std::max(WorstBody, Length(FVec3d{ BodyLocalBefore.X - S.X - BodyLocalAfter.X, BodyLocalBefore.Y - S.Y - BodyLocalAfter.Y, BodyLocalBefore.Z - S.Z - BodyLocalAfter.Z }));
			++Recentres;
		}
	}
	Uni::Check(2, "T2.local.recentre", "Local frame re-centring (D-149): the camera tracks the exact path within 1 mm, does not move across a re-centre (< 1e-3 cm), and bodies shift by exactly the integer shift")
		.Metric("recentres", Recentres).Metric("worst_drift_cm", WorstDrift).Metric("worst_jump_cm", WorstJump).Metric("worst_body_cm", WorstBody)
		.Pass(Recentres > 50 && WorstDrift < 0.1 && WorstJump < 1.0e-3 && WorstBody == 0.0,
			Uni::Fmt("200k frames, %d re-centres: worst camera drift from the exact path %.2e cm, worst jump at a re-centre %.2e cm, body shift error %.1e cm", Recentres, WorstDrift, WorstJump, WorstBody));
}

UNI_TEST(parallax, arrival)
{
	// D-150 (owner): a brightness-boosted sprite keeps its boost far out and is back at its true
	// perceived size (physical angular radius, never below a pixel) by the time it "arrives":
	// a star at its system radius, where the planets spawn; anything else at its promotion
	// distance. Stars from red dwarfs to supergiants, approached from 10^4 x the arrival distance.
	H::FUniView V{};
	V.Forward = H::float3(1, 0, 0); V.Right = H::float3(0, 1, 0); V.Up = H::float3(0, 0, 1);
	V.TanHalfFovX = 1.0f; V.TanHalfFovY = 0.5625f;
	V.LimitingMag = H::Uni_LimitingMag(0.0f); V.SurfaceLimit = H::UNI_MU_REF; V.ExposureMag = V.LimitingMag - 1.0f;
	V.ThetaPixel = 1.0f / 960.0f; V.GlareGain = 0.35f;
	V.SystemArchetypeMask = 1u << UniScenes::kStar;
	struct FStar { const char* Name; float M; double RSun; };
	const FStar Stars[] = { { "red dwarf", 10.0f, 0.3 }, { "Sun", 4.83f, 1.0 }, { "giant", 0.0f, 10.0 }, { "supergiant", -7.0f, 500.0 } };
	double WorstAtArrival = 0.0, MinFarBoost = 1.0e30;
	int NonMonotone = 0;
	std::string Detail;
	for (const FStar& St : Stars)
	{
		const double R = St.RSun * 6.957e10;
		const double Arrive = double(H::Uni_ArrivalDistance(UniScenes::kStar, V.SystemArchetypeMask, St.M, float(R)));
		double Prev = 1.0e30;
		double FarBoost = 0.0, AtArrive = 0.0;
		for (int K = 0; K <= 400; ++K)
		{
			const double D = Arrive * std::pow(10.0, 4.0 - 4.5 * K / 400.0);   // 10^4 x arrival down to 0.3 x
			if (D < 2.0 * R) { break; }
			FCoord Cam = CellOrigin(CellOf(FCoord::Origin(), 70));
			FVec3f Fr{};
			const FCellKey Kc = CellOf(Cam, 64);
			const FCoord O = CellOrigin(Kc);
			FCoord P = O;
			AddDisplacement(P, Fr, FVec3d{ 1.0e17, 0.0, 0.0 });
			Cam = P;
			AddDisplacement(Cam, Fr, FVec3d{ -D, 0.0, 0.0 });
			u32 Off[3];
			for (int A = 0; A < 3; ++A) { Off[A] = Shr(Sub(P.Axis(A), O.Axis(A)), 64 - 32).L[3]; }
			const FDelta Dd = Sub(O, Cam);
			H::uint4 Dl[3];
			for (int A = 0; A < 3; ++A) { const FU128 T = Dd.TwosComplement(A); Dl[A] = H::uint4(T.L[0], T.L[1], T.L[2], T.L[3]); }
			const H::FUniObject Obj = H::Uni_MakeObject(H::uint3(Off[0], Off[1], Off[2]), UniScenes::kStar, 0u, 9u, float(std::log2(R)), St.M, 0.0f, H::float3(1, 1, 1));
			const H::FUniSprite S = H::Uni_ProjectObject(Dl[0], Dl[1], Dl[2], 64u, 1.0f, Obj, V, false);
			if (!(S.Flags & H::UNI_SPRITE_VISIBLE)) { continue; }
			const double TrueSize = std::max(double(S.PhysAngularRadius), double(V.ThetaPixel));
			const double Ratio = double(S.AngularRadius) / TrueSize;
			if (K == 0) { FarBoost = Ratio; }
			if (D <= Arrive * 1.0001) { AtArrive = std::max(AtArrive, std::abs(Ratio - 1.0)); }
			// The boost may only shrink on the way in (a little growth from brightness far out, before the ramp, is fine).
			if (D < Arrive * H::UNI_ARRIVAL_RANGE && Ratio > Prev * 1.0001) { ++NonMonotone; }
			Prev = Ratio;
		}
		WorstAtArrival = std::max(WorstAtArrival, AtArrive);
		MinFarBoost = std::min(MinFarBoost, FarBoost);
		Detail += Uni::Fmt("%s%s: arrives at %s, boost far out %.1fx, at arrival %.4f off true", Detail.empty() ? "" : "; ", St.Name, FormatDistance(Arrive).c_str(), FarBoost, AtArrive);
	}
	Uni::Check(1, "T1.sprite.arrival", "Brightness boost fades to the true perceived size by arrival (D-150): stars at their system radius, within 0.1%, monotone over the ramp, still boosted far out")
		.Metric("worst_at_arrival", WorstAtArrival).Metric("min_far_boost", MinFarBoost).Metric("non_monotone", NonMonotone)
		.Pass(WorstAtArrival < 1.0e-3 && NonMonotone == 0 && MinFarBoost > 1.2, Detail);
}
