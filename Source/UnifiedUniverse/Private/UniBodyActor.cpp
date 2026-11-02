#include "UniBodyActor.h"

#include "UniParallaxComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// The engine sphere is 100 cm across.
	constexpr double kEngineSphereRadiusCm = 50.0;
}

AUniBodyActor::AUniBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->bNeverDistanceCull = true;   // placed far out in render space; never culled by distance
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded()) { Mesh->SetStaticMesh(Sphere.Object); }
	Parallax = CreateDefaultSubobject<UUniParallaxComponent>(TEXT("Parallax"));
	Parallax->bCaptureBaseScaleOnBeginPlay = false;   // InitBody sets it from the radius
}

void AUniBodyActor::InitBody(bool bInStar, double InRadiusCm, const FLinearColor& InColour, uint32 InBodyId)
{
	bStar = bInStar;
	RadiusCm = InRadiusCm;
	BodyId = int64(InBodyId);
	Parallax->RadiusCm = InRadiusCm;
	Parallax->BaseScale = FVector(InRadiusCm / kEngineSphereRadiusCm);

	// Placeholder materials, from engine content only (nothing to author yet). Stars prefer an
	// emissive engine material when one exists; everything falls back to the basic-shape material
	// and its "Color" parameter.
	UMaterialInterface* Base = nullptr;
	if (bStar)
	{
		for (const TCHAR* Path : { TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"), TEXT("/Engine/EngineMaterials/EmissiveTexturedMaterial.EmissiveTexturedMaterial") })
		{
			Base = LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Base) { break; }
		}
	}
	if (!Base) { Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"), nullptr, LOAD_NoWarn | LOAD_Quiet); }
	if (Base)
	{
		Material = UMaterialInstanceDynamic::Create(Base, this);
		Material->SetVectorParameterValue(TEXT("Color"), InColour);
		Material->SetVectorParameterValue(TEXT("EmissiveColor"), InColour * 20.0f);
		Mesh->SetMaterial(0, Material);
	}
}
