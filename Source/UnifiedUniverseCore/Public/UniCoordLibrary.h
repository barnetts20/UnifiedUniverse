#pragma once

// UniCoordLibrary.h
// Blueprint helpers for FUniCoord / FUniCellKey / FUniObjectId: hex string serialisation,
// exact distances (as double cm), cells, and scale-ladder formatting.

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UniCoordTypes.h"
#include "UniCoordLibrary.generated.h"

UCLASS()
class UNIFIEDUNIVERSECORE_API UUniCoordLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The nominal origin, 2^127 cm on each axis. */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FUniCoord CoordOrigin();

	/** 96 hex digits, X then Y then Z, each most significant limb first. */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FString CoordToHex(const FUniCoord& Coord);

	/** Parses CoordToHex output. Returns false and leaves Out at the origin on bad input. */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static bool CoordFromHex(const FString& Hex, FUniCoord& Out);

	/** Coord moved by a displacement in cm, of any magnitude. The sub-cm remainder is
	 *  returned separately so repeated small moves do not lose it. */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FUniCoord CoordAddDisplacement(const FUniCoord& Coord, FVector FractionCm, FVector DisplacementCm, FVector& OutFractionCm);

	/** Exact shortest-image separation |To - From| on the root torus, as double cm. */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static double CoordDistanceCm(const FUniCoord& From, const FUniCoord& To);

	/** To - From as a double vector in cm. A RELATIVE vector: fine for nearby pairs or for
	 *  directions, never to be stored as a position (gameplay rule). */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FVector CoordDeltaCm(const FUniCoord& From, const FUniCoord& To);

	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FUniCellKey CoordCellOf(const FUniCoord& Coord, int32 Level);

	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FUniCoord CellOrigin(const FUniCellKey& Cell);

	/** "3.26 ly", "12.4 Mpc", ... */
	UFUNCTION(BlueprintPure, Category = "Universe|Coordinate")
	static FString FormatDistanceCm(double Cm);

	UFUNCTION(BlueprintPure, Category = "Universe|Object")
	static FString ObjectIdToString(const FUniObjectId& Id);
};
