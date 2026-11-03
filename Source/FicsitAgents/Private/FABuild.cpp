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
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGBuildingDescriptor.h"

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

	/** Every completed build, so it can be undone. Lives as long as the game process. */
	struct FBuildRecord
	{
		int32 Op = 0;
		TArray<TWeakObjectPtr<AActor>> Actors;
	};
	TArray<FBuildRecord> GBuildLog;
	int32 GNextOp = 1;

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
				const bool bInitializing = Disq.ContainsByPredicate([](const TSubclassOf<UFGConstructDisqualifier>& D) { return D && D->GetName() == TEXT("FGCDInitializing"); });
				if (bInitializing && Step < 240)
				{
					return false;
				}
				bReported = true;

				// The game's own cost check only looks at the player's inventory. Like the build gun, we also allow the Dimensional Depot.
				Disq.RemoveAll([](const TSubclassOf<UFGConstructDisqualifier>& D) { return D && D->GetName() == TEXT("FGCDCannotAfford"); });
				const bool bAfford = CanAfford(Pawn, Hologram->GetCost(true));
				const bool bPlacementOk = bHit && Disq.Num() == 0;

				Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("hit_surface"), bHit);
				Result->SetStringField(TEXT("hit_actor"), HitActor);
				Result->SetBoolField(TEXT("exact"), bExact);
				Result->SetNumberField(TEXT("ticks"), Step);
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
			UFGInventoryComponent* Inv = Pawn->GetInventory();
			const TArray<FItemAmount> Cost = Hologram->GetCost(true);
			if (!CanAfford(Pawn, Cost))
			{
				Result->SetBoolField(TEXT("built"), false);
				Result->SetStringField(TEXT("error"), TEXT("Not enough items in the player's inventory and Dimensional Depot"));
				return;
			}
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
			AFGBuildableSubsystem* BS = AFGBuildableSubsystem::Get(World.Get());
			TArray<AActor*> Children;
			AActor* Built = Hologram->Construct(Children, BS->GetNewNetConstructionID());
			if (!Built)
			{
				// Lightweight buildables (foundations, walls) may not return an actor. Charge anyway and say so.
				Pay(Pawn, Cost);
				Result->SetBoolField(TEXT("built"), true);
				Result->SetStringField(TEXT("note"), TEXT("Construct returned no actor (lightweight buildable?). Not undoable yet; verify with query_area."));
				return;
			}
			Pay(Pawn, Cost);
			FBuildRecord Rec;
			Rec.Op = GNextOp++;
			Rec.Actors.Add(Built);
			TArray<TSharedPtr<FJsonValue>> Ids;
			Ids.Add(MakeShared<FJsonValueString>(Built->GetName()));
			for (AActor* C : Children)
			{
				if (C)
				{
					Rec.Actors.Add(C);
					Ids.Add(MakeShared<FJsonValueString>(C->GetName()));
				}
			}
			GBuildLog.Add(Rec);
			Result->SetBoolField(TEXT("built"), true);
			Result->SetNumberField(TEXT("op"), Rec.Op);
			Result->SetArrayField(TEXT("ids"), Ids);
			Result->SetArrayField(TEXT("loc"), V3(Built->GetActorLocation()));
			Result->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Built->GetActorRotation().Yaw * 10) / 10);
		}

		/** Player inventory plus Dimensional Depot, like the build gun. */
		bool CanAfford(AFGCharacterPlayer* Pawn, const TArray<FItemAmount>& Cost) const
		{
			UFGInventoryComponent* Inv = Pawn->GetInventory();
			if (Inv->GetNoBuildCost()) return true;
			AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World.Get());
			for (const FItemAmount& A : Cost)
			{
				const int32 Have = Inv->GetNumItems(A.ItemClass) + (Depot ? Depot->GetNumItemsFromCentralStorage(A.ItemClass) : 0);
				if (Have < A.Amount) return false;
			}
			return true;
		}

		void Pay(AFGCharacterPlayer* Pawn, const TArray<FItemAmount>& Cost) const
		{
			UFGInventoryComponent* Inv = Pawn->GetInventory();
			if (Inv->GetNoBuildCost()) return;
			AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World.Get());
			for (const FItemAmount& A : Cost)
			{
				const int32 FromInv = FMath::Min(A.Amount, Inv->GetNumItems(A.ItemClass));
				if (FromInv > 0) Inv->Remove(A.ItemClass, FromInv);
				if (A.Amount - FromInv > 0 && Depot) Depot->TryRemoveItemsFromCentralStorage(A.ItemClass, A.Amount - FromInv);
			}
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
		AFGBuildable* B = FindBuildable(World, Id);
		TArray<FInventoryStack> Refund;
		if (!B || !DismantleActor(B, Pawn->GetInventory(), Refund))
		{
			Server.ReplyError(Req, 404, FString::Printf(TEXT("%s not found or cannot be dismantled"), *Id));
			return;
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
