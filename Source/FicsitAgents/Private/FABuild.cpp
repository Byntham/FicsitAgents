// Building, dismantling and recipes. Everything goes through the game's own hologram rules.
#include "FAServer.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

#include "FactoryGame.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "FGBuildableSubsystem.h"
#include "FGCentralStorageSubsystem.h"
#include "FGConstructDisqualifier.h"
#include "FGDismantleInterface.h"
#include "ItemAmount.h"
#include "Hologram/FGHologram.h"
#include "Hologram/FGSplineHologram.h"
#include "FGFactoryConnectionComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Hologram/FGConveyorLiftHologram.h"

/**
 * Friend of AFGConveyorLiftHologram (see Config/AccessTransformers.ini). The build gun sets a lift's height from the
 * player's aim; we set the top end to a target port instead.
 */
class FFALiftAccess
{
public:
	static FString ForceTop(AFGConveyorLiftHologram* H, UFGFactoryConnectionComponent* TopPort)
	{
		if (!H || !TopPort) return TEXT("no lift hologram");
		const FTransform Act = H->GetActorTransform();
		const float Height = TopPort->GetComponentLocation().Z - Act.GetLocation().Z;
		const FVector Want = -TopPort->GetConnectorNormal();  // the lift's top connection must face into the port
		auto Apply = [&](float RelYaw)
		{
			H->mTopTransform = FTransform(FRotator(0, RelYaw, 0), FVector(0, 0, Height));
			H->OnRep_TopTransform();
		};
		auto TopConn = [&]() -> UFGFactoryConnectionComponent*
		{
			UFGFactoryConnectionComponent* C0 = H->mConnectionComponents[0];
			UFGFactoryConnectionComponent* C1 = H->mConnectionComponents[1];
			if (!C0 || !C1) return C1 ? C1 : C0;
			return C1->GetComponentLocation().Z >= C0->GetComponentLocation().Z ? C1 : C0;
		};
		float Rel = FRotator::NormalizeAxis(Want.Rotation().Yaw - Act.Rotator().Yaw);
		Apply(Rel);
		float Dot = TopConn() ? FVector::DotProduct(TopConn()->GetConnectorNormal(), Want) : 0.f;
		if (Dot < 0.5f)
		{
			Rel = FRotator::NormalizeAxis(Rel + 180.f);
			Apply(Rel);
			Dot = TopConn() ? FVector::DotProduct(TopConn()->GetConnectorNormal(), Want) : 0.f;
		}
		H->mSnappedConnectionComponents[1] = TopPort;
		H->UpdateConnectionDirections();
		H->UpdateClearance();
		const UFGFactoryConnectionComponent* T = TopConn();
		return FString::Printf(TEXT("height %.1f m, top yaw %.0f, facing %.2f, top conn %.1f m from port"),
			Height / 100.f, Rel, Dot, T ? FVector::Dist(T->GetComponentLocation(), TopPort->GetComponentLocation()) / 100.f : -1.f);
	}

	static FString State(AFGConveyorLiftHologram* H)
	{
		if (!H) return TEXT("-");
		return FString::Printf(TEXT("top %s, snapped %s / %s"), *H->mTopTransform.GetLocation().ToCompactString(),
			H->mSnappedConnectionComponents[0] ? *H->mSnappedConnectionComponents[0]->GetOwner()->GetName() : TEXT("-"),
			H->mSnappedConnectionComponents[1] ? *H->mSnappedConnectionComponents[1]->GetOwner()->GetName() : TEXT("-"));
	}
};

namespace
{
	TArray<TSharedPtr<FJsonValue>> V3(const FVector& V)
	{
		return {MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10) / 10),
		        MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10) / 10),
		        MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10) / 10)};
	}

	bool ReadV3(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr) || Arr->Num() != 3) return false;
		Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	AFGCharacterPlayer* Player(UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		return PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> ItemsJson(const TArray<FItemAmount>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FItemAmount& A : Items)
		{
			const TSharedRef<FJsonObject> I = MakeShared<FJsonObject>();
			I->SetStringField(TEXT("item"), A.ItemClass ? A.ItemClass->GetName() : TEXT("?"));
			I->SetStringField(TEXT("name"), UFGItemDescriptor::GetItemName(A.ItemClass).ToString());
			I->SetNumberField(TEXT("amount"), A.Amount);
			Out.Add(MakeShared<FJsonValueObject>(I));
		}
		return Out;
	}

	/** Finds an unlocked recipe by class name ("Recipe_ConstructorMk1_C", the _C is optional). */
	TSubclassOf<UFGRecipe> FindAvailableRecipe(UWorld* World, FString Name)
	{
		if (!Name.EndsWith(TEXT("_C"))) Name += TEXT("_C");
		if (AFGRecipeManager* RM = AFGRecipeManager::Get(World))
		{
			for (const TSubclassOf<UFGRecipe>& R : RM->GetAllAvailableRecipes())
			{
				if (R && R->GetName() == Name) return R;
			}
		}
		return nullptr;
	}

	AFGBuildable* FindBuildable(UWorld* World, const FString& Id)
	{
		for (TActorIterator<AFGBuildable> It(World); It; ++It)
		{
			if (It->GetName() == Id) return *It;
		}
		return nullptr;
	}

	/** The blueprint designer containing this point, if any. Holograms must be told, like the build gun does. */
	AFGBuildableBlueprintDesigner* DesignerAt(UWorld* World, const FVector& Loc)
	{
		for (TActorIterator<AFGBuildableBlueprintDesigner> It(World); It; ++It)
		{
			if (It->IsLocationInsideDesigner(Loc)) return *It;
		}
		return nullptr;
	}

	/** Every completed build, so it can be undone. Lives as long as the game process. */
	struct FLightweightRecord
	{
		TSubclassOf<AFGBuildable> Cls;
		FVector Loc;
		TSubclassOf<UFGRecipe> Recipe;
	};
	struct FBuildRecord
	{
		int32 Op = 0;
		TArray<TWeakObjectPtr<AActor>> Actors;
		TArray<FLightweightRecord> Lightweights;
	};
	TArray<FBuildRecord> GBuildLog;
	int32 GNextOp = 1;

	/** Player inventory plus Dimensional Depot, like the build gun. */
	bool CanAfford(UWorld* World, AFGCharacterPlayer* Pawn, const TArray<FItemAmount>& Cost)
	{
		UFGInventoryComponent* Inv = Pawn->GetInventory();
		if (Inv->GetNoBuildCost()) return true;
		AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
		for (const FItemAmount& A : Cost)
		{
			const int32 Have = Inv->GetNumItems(A.ItemClass) + (Depot ? Depot->GetNumItemsFromCentralStorage(A.ItemClass) : 0);
			if (Have < A.Amount) return false;
		}
		return true;
	}

	void Pay(UWorld* World, AFGCharacterPlayer* Pawn, const TArray<FItemAmount>& Cost)
	{
		UFGInventoryComponent* Inv = Pawn->GetInventory();
		if (Inv->GetNoBuildCost()) return;
		AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
		for (const FItemAmount& A : Cost)
		{
			const int32 FromInv = FMath::Min(A.Amount, Inv->GetNumItems(A.ItemClass));
			if (FromInv > 0) Inv->Remove(A.ItemClass, FromInv);
			if (A.Amount - FromInv > 0 && Depot) Depot->TryRemoveItemsFromCentralStorage(A.ItemClass, A.Amount - FromInv);
		}
	}

	/** Removes a lightweight instance (foundation, wall...) found by class and position. Refunds the recipe if building costs items. */
	bool RemoveLightweight(UWorld* World, TSubclassOf<AFGBuildable> Cls, const FVector& Loc, TSubclassOf<UFGRecipe> Recipe, UFGInventoryComponent* Inv, TArray<FInventoryStack>& OutRefund)
	{
		AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(World);
		if (!LW || !Cls) return false;
		const TArray<FRuntimeBuildableInstanceData>* Arr = LW->GetAllLightweightBuildableInstances().Find(Cls);
		if (!Arr) return false;
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const FRuntimeBuildableInstanceData& D = (*Arr)[i];
			if (D.BuiltWithRecipe && FVector::Dist(D.Transform.GetLocation(), Loc) < 5.0)
			{
				FLightweightBuildableInstanceRef Ref;
				Ref.Initialize(LW, Cls, i);
				if (!Ref.Remove()) return false;
				if (Inv && !Inv->GetNoBuildCost() && Recipe)
				{
					for (const FItemAmount& A : UFGRecipe::GetIngredients(World, Recipe))
					{
						const FInventoryStack S(A.Amount, A.ItemClass);
						Inv->AddStack(S, true);
						OutRefund.Add(S);
					}
				}
				return true;
			}
		}
		return false;
	}

	/**
	 * Constructs a placed hologram, pays for it, records it for undo and fills Result.
	 * Lightweight buildables come back as a temporary actor that is gone a moment later, so record them by class and position.
	 */
	void FinishConstruct(UWorld* World, AFGHologram* Hologram, AFGCharacterPlayer* Pawn, const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<FItemAmount> Cost = Hologram->GetCost(true);
		if (!CanAfford(World, Pawn, Cost))
		{
			Result->SetBoolField(TEXT("built"), false);
			Result->SetStringField(TEXT("error"), TEXT("Not enough items in the player's inventory and Dimensional Depot"));
			return;
		}
		AFGBuildableSubsystem* BS = AFGBuildableSubsystem::Get(World);
		TArray<AActor*> Children;
		AActor* Built = Hologram->Construct(Children, BS->GetNewNetConstructionID());
		if (!Built)
		{
			Result->SetBoolField(TEXT("built"), false);
			Result->SetStringField(TEXT("error"), TEXT("Construct returned nothing"));
			return;
		}
		Pay(World, Pawn, Cost);
		FBuildRecord Rec;
		Rec.Op = GNextOp++;
		TArray<TSharedPtr<FJsonValue>> Ids;
		TArray<AActor*> All = Children;
		All.Insert(Built, 0);
		bool bAnyLightweight = false;
		for (AActor* A : All)
		{
			if (!A) continue;
			AFGBuildable* B = Cast<AFGBuildable>(A);
			if (B && (B->GetIsLightweightTemporary() || B->ShouldConvertToLightweight()))
			{
				Rec.Lightweights.Add({B->GetClass(), B->GetActorLocation(), B->GetBuiltWithRecipe() ? B->GetBuiltWithRecipe() : Hologram->GetRecipe()});
				bAnyLightweight = true;
			}
			else
			{
				Rec.Actors.Add(A);
			}
			Ids.Add(MakeShared<FJsonValueString>(A->GetName()));
		}
		GBuildLog.Add(Rec);
		Result->SetBoolField(TEXT("built"), true);
		Result->SetBoolField(TEXT("lightweight"), bAnyLightweight);
		Result->SetNumberField(TEXT("op"), Rec.Op);
		Result->SetArrayField(TEXT("ids"), Ids);
		Result->SetArrayField(TEXT("loc"), V3(Built->GetActorLocation()));
		Result->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Built->GetActorRotation().Yaw * 10) / 10);
		// For belts and lifts: report what the new piece actually connected to.
		TArray<TSharedPtr<FJsonValue>> Conns;
		TInlineComponentArray<UFGFactoryConnectionComponent*> FC(Built);
		for (UFGFactoryConnectionComponent* C : FC)
		{
			const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("n"), C->GetName());
			if (C->GetConnection() && C->GetConnection()->GetOwner())
			{
				J->SetStringField(TEXT("to"), C->GetConnection()->GetOwner()->GetName());
				J->SetStringField(TEXT("toc"), C->GetConnection()->GetName());
			}
			Conns.Add(MakeShared<FJsonValueObject>(J));
		}
		Result->SetArrayField(TEXT("conns"), Conns);
	}

	// ------------------------------------------------------------------ build

	class FBuildOp : public FFAPendingOp
	{
	public:
		FBuildOp(const FFARequest& InReq, UWorld* InWorld, TSubclassOf<UFGRecipe> InRecipe, const FVector& InLoc, float InYaw, bool bInDryRun, float InHold)
			: FFAPendingOp(InReq), World(InWorld), Recipe(InRecipe), Loc(InLoc), Yaw(InYaw), bDryRun(bInDryRun), Hold(InHold) {}

		virtual ~FBuildOp() override
		{
			if (Hologram.IsValid()) Hologram->Destroy();
		}

		virtual bool Tick(FFAServer& Server) override
		{
			AFGCharacterPlayer* Pawn = Player(World.Get());
			if (!Pawn)
			{
				Server.ReplyError(Req, 409, TEXT("Player left the game"));
				return true;
			}
			if (Step++ == 0)
			{
				Hologram = AFGHologram::SpawnHologramFromRecipe(Recipe, Pawn, Loc, Pawn);
				if (!Hologram.IsValid())
				{
					Server.ReplyError(Req, 500, TEXT("Could not spawn a hologram for this recipe"));
					return true;
				}
				if (AFGBuildableBlueprintDesigner* D = DesignerAt(World.Get(), Loc))
				{
					Hologram->SetInsideBlueprintDesigner(D);
					DesignerName = D->GetName();
				}
				Hologram->SetScrollRotateValue(FMath::RoundToInt(Yaw));
				Place();
				return false;
			}
			if (!Hologram.IsValid())
			{
				Server.ReplyError(Req, 500, TEXT("Hologram was destroyed"));
				return true;
			}
			if (!bReported)
			{
				// Re-place and validate every tick until the hologram has finished initializing.
				Place();
				Hologram->ValidatePlacementAndCost(Pawn->GetInventory());
				TArray<TSubclassOf<UFGConstructDisqualifier>> Disq;
				Hologram->GetConstructDisqualifiers(Disq);
				// FGCDInitializing never clears outside the build gun (it is a build gun UI state). Give placement a
				// few ticks to settle, then ignore it. Real problems (clearance, overlap) still show as their own disqualifiers.
				if (Step < 6)
				{
					return false;
				}
				const int32 Ignored = Disq.RemoveAll([](const TSubclassOf<UFGConstructDisqualifier>& D) { return D && D->GetName() == TEXT("FGCDInitializing"); });
				bReported = true;

				// The game's own cost check only looks at the player's inventory. Like the build gun, we also allow the Dimensional Depot.
				Disq.RemoveAll([](const TSubclassOf<UFGConstructDisqualifier>& D) { return D && D->GetName() == TEXT("FGCDCannotAfford"); });
				const bool bAfford = CanAfford(World.Get(), Pawn, Hologram->GetCost(true));
				const bool bPlacementOk = bHit && Disq.Num() == 0;  // CanConstruct() is false while FGCDInitializing is set, so we judge by the list

				Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("hit_surface"), bHit);
				Result->SetStringField(TEXT("hit_actor"), HitActor);
				Result->SetBoolField(TEXT("exact"), bExact);
				if (!DesignerName.IsEmpty()) Result->SetStringField(TEXT("designer"), DesignerName);
				Result->SetNumberField(TEXT("ticks"), Step);
				Result->SetBoolField(TEXT("ignored_initializing"), Ignored > 0);
				Result->SetArrayField(TEXT("loc"), V3(Hologram->GetActorLocation()));
				Result->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Hologram->GetActorRotation().Yaw * 10) / 10);
				Result->SetBoolField(TEXT("placement_ok"), bPlacementOk);
				Result->SetBoolField(TEXT("affordable"), bAfford);
				Result->SetBoolField(TEXT("valid"), bPlacementOk && bAfford);
				TArray<TSharedPtr<FJsonValue>> DJ;
				for (const TSubclassOf<UFGConstructDisqualifier>& D : Disq)
				{
					const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("cls"), D ? D->GetName() : TEXT("?"));
					O->SetStringField(TEXT("text"), UFGConstructDisqualifier::GetDisqualifyingText(D).ToString());
					DJ.Add(MakeShared<FJsonValueObject>(O));
				}
				Result->SetArrayField(TEXT("disqualifiers"), DJ);
				Result->SetArrayField(TEXT("cost"), ItemsJson(Hologram->GetCost(true)));
				Result->SetBoolField(TEXT("no_build_cost"), Pawn->GetInventory()->GetNoBuildCost());

				if (bDryRun || !Result->GetBoolField(TEXT("valid")))
				{
					Result->SetBoolField(TEXT("built"), false);
					HoldUntil = FPlatformTime::Seconds() + Hold;
				}
				else
				{
					Construct(Pawn);
				}
			}
			// Keep a dry-run hologram visible for `hold` seconds so a human can see the proposal.
			if (FPlatformTime::Seconds() < HoldUntil) return false;
			Server.Reply(Req, Result);
			return true;
		}

	private:
		void Place()
		{
			if (!Hologram.IsValid()) return;
			if (bExact)
			{
				// Exact placement: no surface needed (floating platforms). Feed the hologram a synthetic
				// upward-facing hit at the target, then pin it to the exact transform the agent asked for.
				FHitResult Hit;
				Hit.bBlockingHit = true;
				Hit.Location = Hit.ImpactPoint = Loc;
				Hit.Normal = Hit.ImpactNormal = FVector::UpVector;
				Hit.TraceStart = Loc + FVector(0, 0, 150);
				Hit.TraceEnd = Loc - FVector(0, 0, 400);
				if (Hologram->IsValidHitResult(Hit))
				{
					Hologram->UpdateHologramPlacement(Hit);
				}
				Hologram->SetActorLocationAndRotation(Loc, FRotator(0, Yaw, 0));
				bHit = true;
				HitActor = TEXT("(exact)");
				return;
			}
			FHitResult Hit;
			FCollisionQueryParams Q(TEXT("FicsitAgentsBuild"), false);
			Q.AddIgnoredActor(Hologram.Get());
			if (AFGCharacterPlayer* Pawn = Player(World.Get())) Q.AddIgnoredActor(Pawn);
			const FVector From = Loc + FVector(0, 0, 150);
			const FVector To = Loc - FVector(0, 0, 400);
			// TC_BuildGun is declared in FactoryGame.h but not exported by the game DLL. It is ECC_GameTraceChannel5 ("BuildGun") in DefaultEngine.ini.
			bHit = World->LineTraceSingleByChannel(Hit, From, To, ECC_GameTraceChannel5, Q);
			if (!bHit) return;
			HitActor = Hit.GetActor() ? Hit.GetActor()->GetClass()->GetName() : FString();
			if (Hologram->IsValidHitResult(Hit))
			{
				Hologram->UpdateHologramPlacement(Hit);
			}
			else
			{
				bHit = false;
			}
		}

		void Construct(AFGCharacterPlayer* Pawn)
		{
			bool bPlaced = false;
			for (int32 i = 0; i < 8 && !bPlaced; ++i)
			{
				bPlaced = Hologram->DoMultiStepPlacement(false);
			}
			if (!bPlaced)
			{
				Result->SetBoolField(TEXT("built"), false);
				Result->SetStringField(TEXT("error"), TEXT("This buildable needs multi-step placement, which build doesn't support yet"));
				return;
			}
			FinishConstruct(World.Get(), Hologram.Get(), Pawn, Result);
		}

	public:
		bool bExact = false;

	private:
		TWeakObjectPtr<UWorld> World;
		TSubclassOf<UFGRecipe> Recipe;
		FVector Loc;
		float Yaw;
		bool bDryRun;
		float Hold;
		int32 Step = 0;
		bool bHit = false;
		bool bReported = false;
		double HoldUntil = 0;
		FString HitActor;
		FString DesignerName;
		TWeakObjectPtr<AFGHologram> Hologram;
		TSharedPtr<FJsonObject> Result;
	};

	// ------------------------------------------------------------------ dismantle

	bool DismantleActor(AActor* A, UFGInventoryComponent* Inv, TArray<FInventoryStack>& OutRefund)
	{
		if (!IsValid(A) || !A->Implements<UFGDismantleInterface>() || !IFGDismantleInterface::Execute_CanDismantle(A))
		{
			return false;
		}
		const bool bNoCost = Inv && Inv->GetNoBuildCost();
		TArray<FInventoryStack> Refund;
		IFGDismantleInterface::Execute_GetDismantleRefund(A, Refund, bNoCost);
		IFGDismantleInterface::Execute_Dismantle(A);
		if (Inv && !bNoCost)
		{
			Inv->AddStacks(Refund);
		}
		OutRefund.Append(Refund);
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> RefundJson(const TArray<FInventoryStack>& Stacks)
	{
		TArray<FItemAmount> Items;
		for (const FInventoryStack& S : Stacks)
		{
			Items.Add(FItemAmount(S.Item.GetItemClass(), S.NumItems));
		}
		return ItemsJson(Items);
	}

	// ------------------------------------------------------------------ connect (belts and lifts, port to port)

	UFGFactoryConnectionComponent* FindPort(UWorld* World, const FString& Id, const FString& Port)
	{
		AFGBuildable* B = FindBuildable(World, Id);
		if (!B) return nullptr;
		TInlineComponentArray<UFGFactoryConnectionComponent*> FC(B);
		for (UFGFactoryConnectionComponent* C : FC)
		{
			if (C->GetName() == Port) return C;
		}
		return nullptr;
	}

	/** A hit result as if the build gun were aimed at this connection: on its owner, at the port, facing out of it. */
	FHitResult PortHit(UFGFactoryConnectionComponent* C)
	{
		FHitResult H;
		AActor* Owner = C->GetOwner();
		H.bBlockingHit = true;
		H.HitObjectHandle = FActorInstanceHandle(Owner);
		H.Component = Owner->FindComponentByClass<UPrimitiveComponent>();
		H.Location = H.ImpactPoint = C->GetComponentLocation();
		H.Normal = H.ImpactNormal = C->GetConnectorNormal();
		H.TraceStart = H.Location + H.Normal * 300.0;
		H.TraceEnd = H.Location - H.Normal * 100.0;
		H.Distance = 300.0f;
		return H;
	}

	/**
	 * Builds a belt or lift between two ports the way a player does: aim at port A, click, aim at port B, click.
	 * The game does the snapping and routing.
	 */
	class FConnectOp : public FFAPendingOp
	{
	public:
		FConnectOp(const FFARequest& InReq, UWorld* InWorld, TSubclassOf<UFGRecipe> InRecipe, UFGFactoryConnectionComponent* InA, UFGFactoryConnectionComponent* InB, bool bInDryRun)
			: FFAPendingOp(InReq), World(InWorld), Recipe(InRecipe), A(InA), B(InB), bDryRun(bInDryRun) {}

		virtual ~FConnectOp() override
		{
			if (Hologram.IsValid()) Hologram->Destroy();
		}

		virtual bool Tick(FFAServer& Server) override
		{
			AFGCharacterPlayer* Pawn = Player(World.Get());
			if (!Pawn || !A.IsValid() || !B.IsValid())
			{
				Server.ReplyError(Req, 409, TEXT("Player left the game or a port disappeared"));
				return true;
			}
			const int32 S = Step++;
			if (S == 0)
			{
				Hologram = AFGHologram::SpawnHologramFromRecipe(Recipe, Pawn, A->GetComponentLocation(), Pawn);
				if (!Hologram.IsValid())
				{
					Server.ReplyError(Req, 500, TEXT("Could not spawn a hologram for this recipe"));
					return true;
				}
				if (AFGBuildableBlueprintDesigner* D = DesignerAt(World.Get(), A->GetComponentLocation()))
				{
					Hologram->SetInsideBlueprintDesigner(D);
				}
				// Lifts are placed bottom first, whichever way items flow.
				bLift = Hologram->IsA<AFGConveyorLiftHologram>();
				First = A;
				Second = B;
				if (bLift && B->GetComponentLocation().Z < A->GetComponentLocation().Z)
				{
					Swap(First, Second);
				}
				return false;
			}
			if (!Hologram.IsValid() || !First.IsValid() || !Second.IsValid())
			{
				Server.ReplyError(Req, 500, TEXT("Hologram was destroyed"));
				return true;
			}
			if (S <= 3)
			{
				Aim(First.Get());
				return false;
			}
			if (S == 4)
			{
				StartStep = BuildStep();
				bFirstClickDone = Hologram->DoMultiStepPlacement(false);  // true would mean it finished in one click
				AfterClickStep = BuildStep();
				return false;
			}
			if (S <= 8)
			{
				if (bLift)
				{
					// Aiming doesn't set a lift's height for us, so set the top end to the target port directly.
					LiftReport = FFALiftAccess::ForceTop(Cast<AFGConveyorLiftHologram>(Hologram.Get()), Second.Get());
				}
				else
				{
					Aim(Second.Get());
				}
				return false;
			}

			Hologram->ValidatePlacementAndCost(Pawn->GetInventory());
			TArray<TSubclassOf<UFGConstructDisqualifier>> Disq;
			Hologram->GetConstructDisqualifiers(Disq);
			Disq.RemoveAll([](const TSubclassOf<UFGConstructDisqualifier>& D) { return D && (D->GetName() == TEXT("FGCDInitializing") || D->GetName() == TEXT("FGCDCannotAfford")); });
			const bool bAfford = CanAfford(World.Get(), Pawn, Hologram->GetCost(true));

			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("hologram"), Hologram->GetClass()->GetName());
			if (bLift)
			{
				Result->SetStringField(TEXT("lift"), LiftReport + TEXT(" | ") + FFALiftAccess::State(Cast<AFGConveyorLiftHologram>(Hologram.Get())));
			}
			Result->SetNumberField(TEXT("step_before_click"), StartStep);
			Result->SetNumberField(TEXT("step_after_click"), AfterClickStep);
			Result->SetBoolField(TEXT("finished_after_first_click"), bFirstClickDone);
			Result->SetBoolField(TEXT("placement_ok"), Disq.Num() == 0);
			Result->SetBoolField(TEXT("affordable"), bAfford);
			Result->SetBoolField(TEXT("valid"), Disq.Num() == 0 && bAfford);
			TArray<TSharedPtr<FJsonValue>> DJ;
			for (const TSubclassOf<UFGConstructDisqualifier>& D : Disq)
			{
				const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("cls"), D ? D->GetName() : TEXT("?"));
				O->SetStringField(TEXT("text"), UFGConstructDisqualifier::GetDisqualifyingText(D).ToString());
				DJ.Add(MakeShared<FJsonValueObject>(O));
			}
			Result->SetArrayField(TEXT("disqualifiers"), DJ);
			Result->SetArrayField(TEXT("cost"), ItemsJson(Hologram->GetCost(true)));

			if (bDryRun || !Result->GetBoolField(TEXT("valid")))
			{
				Result->SetBoolField(TEXT("built"), false);
			}
			else
			{
				bool bDone = bFirstClickDone;
				for (int32 i = 0; i < 6 && !bDone; ++i)
				{
					bDone = Hologram->DoMultiStepPlacement(false);
				}
				if (!bDone)
				{
					Result->SetBoolField(TEXT("built"), false);
					Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Placement did not finish (build step %d)"), BuildStep()));
				}
				else
				{
					FinishConstruct(World.Get(), Hologram.Get(), Pawn, Result);
				}
			}
			Server.Reply(Req, Result);
			return true;
		}

	private:
		void Aim(UFGFactoryConnectionComponent* C)
		{
			const FHitResult H = PortHit(C);
			if (Hologram->IsValidHitResult(H))
			{
				Hologram->UpdateHologramPlacement(H);
			}
		}

		int32 BuildStep() const
		{
			AFGSplineHologram* SH = Cast<AFGSplineHologram>(Hologram.Get());
			return SH ? (int32)SH->GetCurrentBuildStep() : -1;
		}

		TWeakObjectPtr<UWorld> World;
		TSubclassOf<UFGRecipe> Recipe;
		TWeakObjectPtr<UFGFactoryConnectionComponent> A, B, First, Second;
		bool bLift = false;
		FString LiftReport;
		bool bDryRun;
		int32 Step = 0;
		int32 StartStep = -1, AfterClickStep = -1;
		bool bFirstClickDone = false;
		TWeakObjectPtr<AFGHologram> Hologram;
	};
}

namespace FABuild
{
	void Build(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		if (!Player(World))
		{
			Server.ReplyError(Req, 409, TEXT("No game loaded"));
			return;
		}
		FString RecipeName;
		FVector Loc;
		if (!Req.Params->TryGetStringField(TEXT("recipe"), RecipeName) || !ReadV3(Req.Params, TEXT("loc"), Loc))
		{
			Server.ReplyError(Req, 400, TEXT("build needs recipe (e.g. Recipe_ConstructorMk1_C) and loc [x,y,z] in cm"));
			return;
		}
		const TSubclassOf<UFGRecipe> Recipe = FindAvailableRecipe(World, RecipeName);
		if (!Recipe)
		{
			Server.ReplyError(Req, 404, FString::Printf(TEXT("Recipe %s is unknown or not unlocked"), *RecipeName));
			return;
		}
		double Yaw = 0, Hold = 0;
		bool bDryRun = false;
		Req.Params->TryGetNumberField(TEXT("yaw"), Yaw);
		Req.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Req.Params->TryGetNumberField(TEXT("hold"), Hold);
		bool bExact = false;
		Req.Params->TryGetBoolField(TEXT("exact"), bExact);
		const TSharedRef<FBuildOp> Op = MakeShared<FBuildOp>(Req, World, Recipe, Loc, (float)Yaw, bDryRun, (float)FMath::Clamp(Hold, 0.0, 30.0));
		Op->bExact = bExact;
		Server.AddOp(Op);
	}

	void Connect(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		if (!Player(World))
		{
			Server.ReplyError(Req, 409, TEXT("No game loaded"));
			return;
		}
		FString RecipeName, FromId, FromPort, ToId, ToPort;
		const TSharedPtr<FJsonObject>* From = nullptr;
		const TSharedPtr<FJsonObject>* To = nullptr;
		if (!Req.Params->TryGetStringField(TEXT("recipe"), RecipeName) || !Req.Params->TryGetObjectField(TEXT("from"), From) || !Req.Params->TryGetObjectField(TEXT("to"), To) ||
		    !(*From)->TryGetStringField(TEXT("id"), FromId) || !(*From)->TryGetStringField(TEXT("port"), FromPort) ||
		    !(*To)->TryGetStringField(TEXT("id"), ToId) || !(*To)->TryGetStringField(TEXT("port"), ToPort))
		{
			Server.ReplyError(Req, 400, TEXT("connect needs recipe (belt or lift), from {id, port}, to {id, port}"));
			return;
		}
		const TSubclassOf<UFGRecipe> Recipe = FindAvailableRecipe(World, RecipeName);
		UFGFactoryConnectionComponent* A = FindPort(World, FromId, FromPort);
		UFGFactoryConnectionComponent* B = FindPort(World, ToId, ToPort);
		if (!Recipe || !A || !B)
		{
			Server.ReplyError(Req, 404, FString::Printf(TEXT("Not found: %s%s%s"), Recipe ? TEXT("") : TEXT("recipe (or locked) "), A ? TEXT("") : TEXT("from-port "), B ? TEXT("") : TEXT("to-port")));
			return;
		}
		if (A->IsConnected() || B->IsConnected())
		{
			Server.ReplyError(Req, 409, TEXT("One of the ports is already connected"));
			return;
		}
		bool bDryRun = false;
		Req.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Server.AddOp(MakeShared<FConnectOp>(Req, World, Recipe, A, B, bDryRun));
	}

	void Dismantle(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		AFGCharacterPlayer* Pawn = Player(World);
		FString Id;
		if (!Pawn || !Req.Params->TryGetStringField(TEXT("id"), Id))
		{
			Server.ReplyError(Req, 400, TEXT("dismantle needs id, in a loaded game"));
			return;
		}
		TArray<FInventoryStack> Refund;
		if (Id.StartsWith(TEXT("lw:")))
		{
			// lw:<Class>:<index>, as returned by query_area
			TArray<FString> Parts;
			Id.ParseIntoArray(Parts, TEXT(":"));
			AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(World);
			UClass* Cls = Parts.Num() == 3 ? FindFirstObject<UClass>(*Parts[1], EFindFirstObjectOptions::None) : nullptr;
			const TArray<FRuntimeBuildableInstanceData>* Arr = (LW && Cls) ? LW->GetAllLightweightBuildableInstances().Find(Cls) : nullptr;
			const int32 Idx = Parts.Num() == 3 ? FCString::Atoi(*Parts[2]) : -1;
			if (!Arr || !Arr->IsValidIndex(Idx) || !(*Arr)[Idx].BuiltWithRecipe ||
			    !RemoveLightweight(World, Cls, (*Arr)[Idx].Transform.GetLocation(), (*Arr)[Idx].BuiltWithRecipe, Pawn->GetInventory(), Refund))
			{
				Server.ReplyError(Req, 404, FString::Printf(TEXT("%s not found"), *Id));
				return;
			}
		}
		else
		{
			AFGBuildable* B = FindBuildable(World, Id);
			if (!B || !DismantleActor(B, Pawn->GetInventory(), Refund))
			{
				Server.ReplyError(Req, 404, FString::Printf(TEXT("%s not found or cannot be dismantled"), *Id));
				return;
			}
		}
		const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("dismantled"), Id);
		R->SetArrayField(TEXT("refund"), RefundJson(Refund));
		Server.Reply(Req, R);
	}

	void Undo(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		AFGCharacterPlayer* Pawn = Player(World);
		if (!Pawn)
		{
			Server.ReplyError(Req, 409, TEXT("No game loaded"));
			return;
		}
		int32 Op = GBuildLog.Num() ? GBuildLog.Last().Op : 0;
		Req.Params->TryGetNumberField(TEXT("op"), Op);
		const int32 Index = GBuildLog.IndexOfByPredicate([Op](const FBuildRecord& R) { return R.Op == Op; });
		if (Index == INDEX_NONE)
		{
			Server.ReplyError(Req, 404, FString::Printf(TEXT("No build op %d in this session"), Op));
			return;
		}
		TArray<FInventoryStack> Refund;
		TArray<TSharedPtr<FJsonValue>> Removed;
		for (const TWeakObjectPtr<AActor>& A : GBuildLog[Index].Actors)
		{
			if (A.IsValid())
			{
				const FString Name = A->GetName();
				if (DismantleActor(A.Get(), Pawn->GetInventory(), Refund)) Removed.Add(MakeShared<FJsonValueString>(Name));
			}
		}
		for (const FLightweightRecord& L : GBuildLog[Index].Lightweights)
		{
			if (RemoveLightweight(World, L.Cls, L.Loc, L.Recipe, Pawn->GetInventory(), Refund))
			{
				Removed.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s at %s"), *L.Cls->GetName(), *L.Loc.ToCompactString())));
			}
		}
		GBuildLog.RemoveAt(Index);
		const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("op"), Op);
		R->SetArrayField(TEXT("removed"), Removed);
		R->SetArrayField(TEXT("refund"), RefundJson(Refund));
		Server.Reply(Req, R);
	}

	void ListRecipes(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		AFGRecipeManager* RM = World ? AFGRecipeManager::Get(World) : nullptr;
		if (!RM)
		{
			Server.ReplyError(Req, 409, TEXT("No game loaded"));
			return;
		}
		FString Kind = TEXT("all");
		Req.Params->TryGetStringField(TEXT("kind"), Kind);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TSubclassOf<UFGRecipe>& R : RM->GetAllAvailableRecipes())
		{
			if (!R) continue;
			const TArray<FItemAmount> Products = UFGRecipe::GetProducts(R);
			const bool bBuilding = Products.Num() && Products[0].ItemClass && Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass());
			if ((Kind == TEXT("building") && !bBuilding) || (Kind == TEXT("production") && bBuilding)) continue;
			const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("recipe"), R->GetName());
			J->SetStringField(TEXT("name"), UFGRecipe::GetRecipeName(R).ToString());
			J->SetBoolField(TEXT("building"), bBuilding);
			J->SetArrayField(TEXT("in"), ItemsJson(UFGRecipe::GetIngredients(World, R)));
			J->SetArrayField(TEXT("out"), ItemsJson(Products));
			Out.Add(MakeShared<FJsonValueObject>(J));
		}
		const TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetNumberField(TEXT("count"), Out.Num());
		Res->SetArrayField(TEXT("recipes"), Out);
		Server.Reply(Req, Res);
	}

	void SetRecipe(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		FString Id, RecipeName;
		if (!Player(World) || !Req.Params->TryGetStringField(TEXT("id"), Id) || !Req.Params->TryGetStringField(TEXT("recipe"), RecipeName))
		{
			Server.ReplyError(Req, 400, TEXT("set_recipe needs id and recipe, in a loaded game"));
			return;
		}
		AFGBuildableManufacturer* M = Cast<AFGBuildableManufacturer>(FindBuildable(World, Id));
		const TSubclassOf<UFGRecipe> Recipe = FindAvailableRecipe(World, RecipeName);
		if (!M || !Recipe)
		{
			Server.ReplyError(Req, 404, TEXT("Machine not found, or recipe unknown or locked"));
			return;
		}
		M->SetRecipe(Recipe);
		const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("id"), Id);
		R->SetStringField(TEXT("recipe"), M->GetCurrentRecipe() ? M->GetCurrentRecipe()->GetName() : FString());
		R->SetBoolField(TEXT("ok"), M->GetCurrentRecipe() == Recipe);
		Server.Reply(Req, R);
	}
}
