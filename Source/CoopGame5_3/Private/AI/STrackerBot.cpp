// Fill out your copyright notice in the Description page of Project Settings.


#include "AI/STrackerBot.h"
#include "Components/StaticMeshComponent.h"
#include "Kismet/Gameplaystatics.h"
#include "NavigationSystem.h"
#include "GameFramework/Character.h"
#include "NavigationPath.h"
#include "Components/SHealthComponent.h"
#include "SCharacter.h"
#include "Components/SphereComponent.h"
#include "Sound/SoundCue.h"
#include "EngineUtils.h"


static int32 DebugTrackerBotDrawing = 0;
FAutoConsoleVariableRef CVARDebugTrackerBotDrawing(TEXT("COOP.DebugTrackerBot"), DebugTrackerBotDrawing, TEXT("Draw Debug Lines for TrackerBot"), ECVF_Cheat);


// Sets default values
ASTrackerBot::ASTrackerBot()
{
	// Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
	MeshComp->SetCanEverAffectNavigation(false);
	MeshComp->SetSimulatePhysics(true);
	SetRootComponent(MeshComp);

	HealthComp = CreateDefaultSubobject<USHealthComponent>(TEXT("HealthComponent"));
	HealthComp->OnHealthChanged.AddDynamic(this, &ASTrackerBot::HandleTakeDamage);

	SphereComp = CreateDefaultSubobject<USphereComponent>(TEXT("SphereComponent"));
	SphereComp->SetupAttachment(MeshComp);
	SphereComp->SetSphereRadius(200);
	SphereComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	SphereComp->SetCollisionResponseToAllChannels(ECR_Ignore);
	SphereComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	bUseVelocityChange = false;
	MovementForce = 1000;
	RequiredDistanceToTarget = 100;

	ExplosionRadius = 350;
	ExplosionDamage = 60;

	SelfDamageInterval = 0.25f;
}

// Called when the game starts or when spawned
void ASTrackerBot::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// Find initial move-to
		NextPathPoint = GetNextPathPoint();

		FTimerHandle TimerHandle_CheckPowerLevel;
		GetWorldTimerManager().SetTimer(TimerHandle_CheckPowerLevel, this, &ASTrackerBot::OnCheckNearbyBots, 1.0f, true);
	}



}

// Called every frame
void ASTrackerBot::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (HasAuthority() && !bExploded)
	{

		float DistanceToTarget = (GetActorLocation() - NextPathPoint).Size();

		if (DistanceToTarget > RequiredDistanceToTarget)
		{
			// Keep moving
			FVector ForceDirection = NextPathPoint - GetActorLocation();
			ForceDirection.Normalize();
			ForceDirection *= MovementForce;
			MeshComp->AddForce(ForceDirection, NAME_None, bUseVelocityChange);

			if (DebugTrackerBotDrawing)
			{
				DrawDebugDirectionalArrow(GetWorld(), GetActorLocation(), GetActorLocation() + ForceDirection, 32, FColor::Yellow, false, 0.0f, 0, 1.0f);
			}
		}
		else
		{
			NextPathPoint = GetNextPathPoint();
		}
	}

}

void ASTrackerBot::NotifyActorBeginOverlap(AActor* OtherActor)
{
	Super::NotifyActorBeginOverlap(OtherActor);

	if (bStartedSelfDestruction || bExploded)
	{
		return;
	}
	ASCharacter* PlayerPawn = Cast<ASCharacter>(OtherActor);
	if (PlayerPawn && !USHealthComponent::IsFriendly(OtherActor, this))
	{
		if (HasAuthority())
		{
			// We overlapped with a player
			GetWorldTimerManager().SetTimer(TimerHandle_SelfDamage, this, &ASTrackerBot::DamageSelf, SelfDamageInterval, true, 0.0f);

		}
		bStartedSelfDestruction = true;

		UGameplayStatics::SpawnSoundAttached(SelfDestructSound, RootComponent);
	}
}

void ASTrackerBot::RefreshPath()
{
	NextPathPoint = GetNextPathPoint();
}

void ASTrackerBot::DamageSelf()
{
	UGameplayStatics::ApplyDamage(this, 20, GetInstigatorController(), this, nullptr);
}

FVector ASTrackerBot::GetNextPathPoint()
{
	AActor* BestTarget = nullptr;
	float NearestTargetDistance = FLT_MAX;

	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* TestPawn = *It;
		if (TestPawn == nullptr || USHealthComponent::IsFriendly(TestPawn, this))
		{
			continue;
		}

		USHealthComponent* TestPawnHealthComp = Cast<USHealthComponent>(TestPawn->GetComponentByClass(USHealthComponent::StaticClass()));
		if (TestPawnHealthComp && TestPawnHealthComp->GetHealth() > 0.0f)
		{
			float Distance = (TestPawn->GetActorLocation() - GetActorLocation()).Size();
			if (Distance < NearestTargetDistance)
			{
				BestTarget = TestPawn;
				NearestTargetDistance = Distance;
			}
		}
	}
	if (BestTarget)
	{
		UNavigationPath* NavPath = UNavigationSystemV1::FindPathToActorSynchronously(this, GetActorLocation(), BestTarget);

		GetWorldTimerManager().ClearTimer(TimerHandle_RefreshPath);
		GetWorldTimerManager().SetTimer(TimerHandle_RefreshPath, this, &ASTrackerBot::RefreshPath, 5.0f, false);

		if (NavPath && NavPath->PathPoints.Num() > 1)
		{
			return NavPath->PathPoints[1];
		}
	}
	// Failed to find path
	return GetActorLocation();
}

void ASTrackerBot::HandleTakeDamage(USHealthComponent* OwningHealthComponent, float Health, float HealthDelta, const class UDamageType* DamageType,
									class AController* InstigatedBy, AActor* DamageCauser)
{
	if (MatInst == nullptr)
	{
		MatInst = MeshComp->CreateAndSetMaterialInstanceDynamicFromMaterial(0, MeshComp->GetMaterial(0));
	}
	if (MatInst)
	{
		MatInst->SetScalarParameterValue("LastTimeDamageTaken", GetWorld()->TimeSeconds);
	}

	// Explode on hitpoints == 0
	if (Health <= 0)
	{
		SelfDestruct();
	}
}

void ASTrackerBot::SelfDestruct()
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), ExplosionEffect, GetActorLocation());
	UGameplayStatics::PlaySoundAtLocation(this, ExplodeSound, GetActorLocation());

	MeshComp->SetVisibility(false, true);
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (HasAuthority())
	{
		TArray<AActor*> IgnoredActors;
		IgnoredActors.Add(this);

		// Increase damage based on the power level
		float ActualDamage = ExplosionDamage + (ExplosionDamage * PowerLevel);

		// Apply Damage
		UGameplayStatics::ApplyRadialDamage(this, ActualDamage, GetActorLocation(), ExplosionRadius, nullptr, IgnoredActors, this, GetInstigatorController(), true);

		if (DebugTrackerBotDrawing)
		{
			DrawDebugSphere(GetWorld(), GetActorLocation(), ExplosionRadius, 12, FColor::Red, false, 5, 0, 1);
		}

		SetLifeSpan(2.0f);

	}
}

void ASTrackerBot::OnCheckNearbyBots()
{
	const float Radius = 600;

	FCollisionShape CollShape;
	CollShape.SetSphere(Radius);

	// Only find Pawns
	FCollisionObjectQueryParams QueryParams;

	QueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	QueryParams.AddObjectTypesToQuery(ECC_Pawn);

	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByObjectType(Overlaps, GetActorLocation(), FQuat::Identity, QueryParams, CollShape);

	if (DebugTrackerBotDrawing)
	{
		DrawDebugSphere(GetWorld(), GetActorLocation(), Radius, 12, FColor::White, false, 1.0f);
	}

	int32 NrOfBots = 0;

	for (FOverlapResult& Result : Overlaps)
	{
		ASTrackerBot* Bot = Cast<ASTrackerBot>(Result.GetActor());
		if (Bot && Bot != this)
		{
			NrOfBots++;
		}
	}

	const int32 MaxPowerLevel = 4;

	PowerLevel = FMath::Clamp(NrOfBots, 0, MaxPowerLevel);

	if (MatInst == nullptr)
	{
		MatInst = MeshComp->CreateAndSetMaterialInstanceDynamicFromMaterial(0, MeshComp->GetMaterial(0));
	}
	if (MatInst)
	{
		float Alpha = PowerLevel / (float)MaxPowerLevel;
		MatInst->SetScalarParameterValue("PowerLevelAlpha", Alpha);
	}

	if (DebugTrackerBotDrawing)
	{
		DrawDebugString(GetWorld(), FVector::ZeroVector, FString::FromInt(PowerLevel), this, FColor::White, 1.0f, true);
	}
}

