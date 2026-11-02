#pragma once

// UniPromotion.h
// Promotion of nearby bodies to scene actors (M3, D-147). Engine-agnostic.
//
// Which bodies: the stars and planets of active systems (UniSystem.h). Every body that can pass
// the promotion test is inside its system's activation radius: a star must be within
// R / theta_promote = 50 R of the camera, far inside 100 AU x sqrt(L) (even a 1,000 R_sun
// supergiant: 230 AU against its 10^4 AU cap). So promotion needs no GPU readback of its own:
// the CPU already knows these bodies exactly.
//
// The test is the GPU's own (Uni_PromoteTest, UniProject.ush): surface within r0 or angular
// radius above theta_promote = 0.02 rad, with 1.25x hysteresis, so the actor appears exactly
// where the sprite is flagged as a candidate. Hand-off:
//   - planets: their records get UNI_OBJ_FLAG_PROMOTED (the sprite is culled) on the same frame
//     the actor is placed, so exactly one of the two is ever drawn;
//   - stars: the sprite stays. The actor's sphere hides it by the scene-depth test (D-146), and
//     its glare outside the silhouette reads as a corona until B-003 hands glare to bloom.
//
// The actor itself is placed by UniParallax.h every frame from the universal camera.

#include "UniSystem.h"
#include "UniParallax.h"

#include <map>
#include <set>
#include <vector>

namespace UniCore
{
	struct FPromotedBody
	{
		u32 Id = 0;                 // the body's seed: star seed or planet seed
		u32 SystemSeed = 0;         // the star it belongs to
		bool bStar = false;
		EPlanetType PlanetType = EPlanetType::Rocky;
		FCoord Coord;               // centre, exact, this frame
		double RadiusCm = 0.0;
		float Rgb[3] = { 1.0f, 1.0f, 1.0f };
		u64 PromotedFrame = 0;
	};

	class FPromotionManager
	{
	public:
		size_t Promotions = 0, Demotions = 0;

		/** Re-test every body of every active system against the camera at time t. */
		void Update(const FSystemManager& Systems, const FCoord& Camera, double TimeS, u64 Frame)
		{
			std::set<u32> Seen;
			for (const auto& KV : Systems.ActiveSystems())
			{
				const FActiveSystem& A = KV.second;
				Consider(A, true, nullptr, A.Star, Camera, Frame, Seen);
				for (const FPlanet& P : A.Sys.Planets)
				{
					FCoord Pc = A.Star;
					FVec3f Frac{};
					AddDisplacement(Pc, Frac, PlanetOffset(P, TimeS));
					Consider(A, false, &P, Pc, Camera, Frame, Seen);
				}
			}
			for (auto It = Bodies.begin(); It != Bodies.end();)
			{
				if (!Seen.count(It->first)) { It = Bodies.erase(It); ++Demotions; }
				else { ++It; }
			}
		}

		const std::map<u32, FPromotedBody>& Promoted() const { return Bodies; }
		bool IsPromoted(u32 Id) const { return Bodies.count(Id) != 0; }

		/** Ids whose sprite must be culled (planets; stars keep theirs, see above). */
		std::set<u32> CulledSpriteIds() const
		{
			std::set<u32> Out;
			for (const auto& KV : Bodies) { if (!KV.second.bStar) { Out.insert(KV.first); } }
			return Out;
		}

		void Clear() { Bodies.clear(); }

	private:
		void Consider(const FActiveSystem& A, bool bStar, const FPlanet* P, const FCoord& Centre, const FCoord& Camera, u64 Frame, std::set<u32>& Seen)
		{
			const u32 Id = bStar ? A.Sys.StarSeed : P->Seed;
			const double Radius = bStar ? A.Sys.StarRadiusCm : P->RadiusCm;
			const double D = Length(ToDouble(Sub(Centre, Camera)));
			auto It = Bodies.find(Id);
			const bool bWas = It != Bodies.end();
			if (!UniHlsl::Uni_PromoteTest(float(std::max(D, 1.0e-3)), float(Radius), bWas)) { return; }   // demoted below if it was
			Seen.insert(Id);
			if (!bWas)
			{
				FPromotedBody B;
				B.Id = Id;
				B.SystemSeed = A.Sys.StarSeed;
				B.bStar = bStar;
				B.RadiusCm = Radius;
				B.PromotedFrame = Frame;
				if (bStar) { for (int C = 0; C < 3; ++C) { B.Rgb[C] = A.Sys.StarRgb[C]; } }
				else
				{
					B.PlanetType = P->Type;
					float Rgb[3] = { A.Sys.StarRgb[0] * P->Tint[0], A.Sys.StarRgb[1] * P->Tint[1], A.Sys.StarRgb[2] * P->Tint[2] };
					const float Mx = std::max({ Rgb[0], Rgb[1], Rgb[2], 1.0e-6f });
					for (int C = 0; C < 3; ++C) { B.Rgb[C] = Rgb[C] / Mx; }
				}
				It = Bodies.emplace(Id, B).first;
				++Promotions;
			}
			It->second.Coord = Centre;
		}

		std::map<u32, FPromotedBody> Bodies;
	};
}
