#include "UniCoordLibrary.h"

#include "UniScaleLadder.h"

FUniCoord UUniCoordLibrary::CoordOrigin()
{
	return FUniCoord::Make(UniCore::FCoord::Origin());
}

FString UUniCoordLibrary::CoordToHex(const FUniCoord& Coord)
{
	return Coord.ToHexString();
}

bool UUniCoordLibrary::CoordFromHex(const FString& Hex, FUniCoord& Out)
{
	UniCore::FCoord C;
	if (!UniCore::FromHexString(std::string(TCHAR_TO_UTF8(*Hex)), C))
	{
		Out = CoordOrigin();
		return false;
	}
	Out = FUniCoord::Make(C);
	return true;
}

FUniCoord UUniCoordLibrary::CoordAddDisplacement(const FUniCoord& Coord, FVector FractionCm, FVector DisplacementCm, FVector& OutFractionCm)
{
	UniCore::FCoord C = Coord.ToCore();
	UniCore::FVec3f F{ float(FractionCm.X), float(FractionCm.Y), float(FractionCm.Z) };
	UniCore::AddDisplacement(C, F, UniCore::FVec3d{ DisplacementCm.X, DisplacementCm.Y, DisplacementCm.Z });
	OutFractionCm = FVector(F.X, F.Y, F.Z);
	return FUniCoord::Make(C);
}

double UUniCoordLibrary::CoordDistanceCm(const FUniCoord& From, const FUniCoord& To)
{
	return UniCore::Length(UniCore::ToDouble(UniCore::Sub(To.ToCore(), From.ToCore())));
}

FVector UUniCoordLibrary::CoordDeltaCm(const FUniCoord& From, const FUniCoord& To)
{
	const UniCore::FVec3d D = UniCore::ToDouble(UniCore::Sub(To.ToCore(), From.ToCore()));
	return FVector(D.X, D.Y, D.Z);
}

FUniCellKey UUniCoordLibrary::CoordCellOf(const FUniCoord& Coord, int32 Level)
{
	return FUniCellKey::Make(UniCore::CellOf(Coord.ToCore(), FMath::Clamp(Level, 0, 128)));
}

FUniCoord UUniCoordLibrary::CellOrigin(const FUniCellKey& Cell)
{
	return FUniCoord::Make(UniCore::CellOrigin(Cell.ToCore()));
}

FString UUniCoordLibrary::FormatDistanceCm(double Cm)
{
	return FString(UTF8_TO_TCHAR(UniCore::FormatDistance(Cm).c_str()));
}

FString UUniCoordLibrary::ObjectIdToString(const FUniObjectId& Id)
{
	const UniCore::FCellKey K = Id.Cell.ToCore();
	return FString::Printf(TEXT("L%d:%s%s%s#%d"), K.Level,
		UTF8_TO_TCHAR(UniCore::ToHex(K.C[0]).c_str()),
		UTF8_TO_TCHAR(UniCore::ToHex(K.C[1]).c_str()),
		UTF8_TO_TCHAR(UniCore::ToHex(K.C[2]).c_str()),
		Id.Index);
}
