#include "FAServer.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SplineComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UnrealClient.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGCharacterPlayer.h"

namespace
{
	const TCHAR* ModVersion = TEXT("0.3.2");

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return {MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10) / 10),
		        MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10) / 10),
		        MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10) / 10)};
	}

	TArray<TSharedPtr<FJsonValue>> Quat(const FQuat& Q)
	{
		auto R = [](double D) { return MakeShared<FJsonValueNumber>(FMath::RoundToDouble(D * 10000) / 10000); };
		return {R(Q.X), R(Q.Y), R(Q.Z), R(Q.W)};
	}

	bool ReadVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr) || Arr->Num() != 3)
		{
			return false;
		}
		Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	FString ClassName(const UClass* C) { return C ? C->GetName() : FString(); }

	FString CapturesDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FicsitAgents") / TEXT("captures"));
	}

	FString NewCapturePath(const FString& Prefix)
	{
		return CapturesDir() / FString::Printf(TEXT("%s_%s.png"), *Prefix, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
	}

	/** The player's character, only when a save is loaded (the main menu has a different pawn). */
	APawn* PlayerPawn(UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		return PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	}

	// ---------------------------------------------------------------- hello / player

	void Hello(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("protocol"), FFAServer::ProtocolVersion);
		R->SetStringField(TEXT("mod"), TEXT("FicsitAgents"));
		R->SetStringField(TEXT("version"), ModVersion);
		R->SetStringField(TEXT("map"), World ? World->GetMapName() : FString());
		R->SetBoolField(TEXT("in_game"), PlayerPawn(World) != nullptr);
		R->SetStringField(TEXT("units"), TEXT("cm; x east, y south, z up; rotations as quaternion [x,y,z,w]"));
		Server.Reply(Req, R);
	}

	void GetPlayer(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			Server.ReplyError(Req, 409, TEXT("No player in a loaded game"));
			return;
		}
		const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetArrayField(TEXT("loc"), Vec(Pawn->GetActorLocation()));
		R->SetArrayField(TEXT("rot"), Quat(Pawn->GetActorQuat()));
		if (PC->PlayerCameraManager)
		{
			R->SetArrayField(TEXT("cam_loc"), Vec(PC->PlayerCameraManager->GetCameraLocation()));
			const FRotator CR = PC->PlayerCameraManager->GetCameraRotation();
			R->SetArrayField(TEXT("cam_pyr"), Vec(FVector(CR.Pitch, CR.Yaw, CR.Roll)));
			R->SetNumberField(TEXT("cam_fov"), PC->PlayerCameraManager->GetFOVAngle());
		}
		Server.Reply(Req, R);
	}

	// ---------------------------------------------------------------- query_area

	TSharedRef<FJsonObject> ConnJson(const UActorComponent* Comp, const UActorComponent* Other, int32 Dir, bool bPipe)
	{
		const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("n"), Comp->GetName());
		C->SetNumberField(TEXT("d"), Dir);
		if (bPipe)
		{
			C->SetBoolField(TEXT("pipe"), true);
		}
		if (const USceneComponent* SC = Cast<USceneComponent>(Comp))
		{
			C->SetArrayField(TEXT("loc"), Vec(SC->GetComponentLocation()));
		}
		if (Other && Other->GetOwner())
		{
			C->SetStringField(TEXT("to"), Other->GetOwner()->GetName());
			C->SetStringField(TEXT("toc"), Other->GetName());
		}
		return C;
	}

	void QueryArea(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		if (!PlayerPawn(World))
		{
			Server.ReplyError(Req, 409, TEXT("No game loaded"));
			return;
		}
		FVector Center;
		if (!ReadVec(Req.Params, TEXT("center"), Center))
		{
			Center = PlayerPawn(World)->GetActorLocation();
		}
		double Radius = 4000;
		Req.Params->TryGetNumberField(TEXT("radius"), Radius);
		bool bLightweight = true;
		Req.Params->TryGetBoolField(TEXT("lightweight"), bLightweight);
		int32 Max = 20000;
		Req.Params->TryGetNumberField(TEXT("max"), Max);
		const double R2 = Radius * Radius;
		auto Inside = [&](const FVector& P) { return FVector::DistSquared2D(P, Center) <= R2; };

		TArray<TSharedPtr<FJsonValue>> Out;
		bool bTruncated = false;
		for (TActorIterator<AFGBuildable> It(World); It; ++It)
		{
			AFGBuildable* B = *It;
			if (!IsValid(B) || !Inside(B->GetActorLocation()))
			{
				continue;
			}
			if (Out.Num() >= Max)
			{
				bTruncated = true;
				break;
			}
			const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("id"), B->GetName());
			E->SetStringField(TEXT("cls"), ClassName(B->GetClass()));
			E->SetArrayField(TEXT("loc"), Vec(B->GetActorLocation()));
			E->SetArrayField(TEXT("rot"), Quat(B->GetActorQuat()));
			if (B->GetBuiltWithRecipe())
			{
				E->SetStringField(TEXT("built"), ClassName(B->GetBuiltWithRecipe()));
			}
			if (const AFGBuildableManufacturer* M = Cast<AFGBuildableManufacturer>(B))
			{
				if (M->GetCurrentRecipe())
				{
					E->SetStringField(TEXT("recipe"), ClassName(M->GetCurrentRecipe()));
				}
			}
			const FBox Box = B->GetCombinedClearanceBox();
			if (Box.IsValid)
			{
				const TSharedRef<FJsonObject> BoxJ = MakeShared<FJsonObject>();
				BoxJ->SetArrayField(TEXT("min"), Vec(Box.Min));
				BoxJ->SetArrayField(TEXT("max"), Vec(Box.Max));
				E->SetObjectField(TEXT("clearance"), BoxJ);
			}

			TArray<TSharedPtr<FJsonValue>> Conns;
			TInlineComponentArray<UFGFactoryConnectionComponent*> Factory(B);
			for (UFGFactoryConnectionComponent* C : Factory)
			{
				Conns.Add(MakeShared<FJsonValueObject>(ConnJson(C, C->GetConnection(), (int32)C->GetDirection(), false)));
			}
			TInlineComponentArray<UFGPipeConnectionComponentBase*> Pipes(B);
			for (UFGPipeConnectionComponentBase* C : Pipes)
			{
				Conns.Add(MakeShared<FJsonValueObject>(ConnJson(C, C->GetConnection(), -1, true)));
			}
			if (Conns.Num())
			{
				E->SetArrayField(TEXT("conns"), Conns);
			}

			if (const AFGBuildableConveyorBelt* Belt = Cast<AFGBuildableConveyorBelt>(B))
			{
				if (USplineComponent* S = Belt->GetSplineComponent())
				{
					TArray<TSharedPtr<FJsonValue>> Pts;
					for (int32 i = 0; i < S->GetNumberOfSplinePoints(); ++i)
					{
						Pts.Add(MakeShared<FJsonValueArray>(Vec(S->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World))));
					}
					E->SetArrayField(TEXT("spline"), Pts);
				}
			}
			if (const AFGBuildableConveyorLift* Lift = Cast<AFGBuildableConveyorLift>(B))
			{
				E->SetArrayField(TEXT("top"), Vec(Lift->GetActorTransform().TransformPosition(Lift->GetTopTransform().GetLocation())));
			}
			if (const AFGBuildableBlueprintDesigner* D = Cast<AFGBuildableBlueprintDesigner>(B))
			{
				E->SetArrayField(TEXT("designer_size"), Vec(D->GetBlueprintDesignerSize()));
			}
			Out.Add(MakeShared<FJsonValueObject>(E));
		}

		int32 LightCount = 0;
		if (bLightweight && !bTruncated)
		{
			if (AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(World))
			{
				for (const auto& Pair : LW->GetAllLightweightBuildableInstances())
				{
					const FString Cls = ClassName(Pair.Key.Get());
					for (int32 i = 0; i < Pair.Value.Num(); ++i)
					{
						const FRuntimeBuildableInstanceData& D = Pair.Value[i];
						if (!D.BuiltWithRecipe || !Inside(D.Transform.GetLocation()))
						{
							continue;
						}
						if (Out.Num() >= Max)
						{
							bTruncated = true;
							break;
						}
						const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
						E->SetStringField(TEXT("id"), FString::Printf(TEXT("lw:%s:%d"), *Cls, i));
						E->SetStringField(TEXT("cls"), Cls);
						E->SetArrayField(TEXT("loc"), Vec(D.Transform.GetLocation()));
						E->SetArrayField(TEXT("rot"), Quat(D.Transform.GetRotation()));
						E->SetStringField(TEXT("built"), ClassName(D.BuiltWithRecipe));
						E->SetBoolField(TEXT("lw"), true);
						Out.Add(MakeShared<FJsonValueObject>(E));
						++LightCount;
					}
				}
			}
		}

		const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetArrayField(TEXT("center"), Vec(Center));
		R->SetNumberField(TEXT("radius"), Radius);
		R->SetNumberField(TEXT("count"), Out.Num());
		R->SetNumberField(TEXT("lightweight_count"), LightCount);
		R->SetBoolField(TEXT("truncated"), bTruncated);
		R->SetArrayField(TEXT("entities"), Out);
		Server.Reply(Req, R);
	}

	// ---------------------------------------------------------------- capture (off-screen camera)

	bool SavePng(const TArray<FColor>& Pixels, int32 W, int32 H, const FString& Path)
	{
		IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = Mod.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8>& Data = Png->GetCompressed();
		return FFileHelper::SaveArrayToFile(Data, *Path);
	}

	class FCaptureOp : public FFAPendingOp
	{
	public:
		FCaptureOp(const FFARequest& InReq, UWorld* World, const FVector& Loc, const FRotator& Rot, float Fov, float OrthoWidth, int32 InW, int32 InH, int32 InWarmup)
			: FFAPendingOp(InReq), W(InW), H(InH), Warmup(InWarmup)
		{
			FActorSpawnParameters SP;
			SP.ObjectFlags |= RF_Transient;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor = World->SpawnActor<ASceneCapture2D>(Loc, Rot, SP);
			Target = TStrongObjectPtr<UTextureRenderTarget2D>(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
			Target->ClearColor = FLinearColor::Black;
			Target->InitCustomFormat(W, H, PF_B8G8R8A8, false);
			Target->UpdateResourceImmediate(true);
			if (USceneCaptureComponent2D* C = Actor.IsValid() ? Actor->GetCaptureComponent2D() : nullptr)
			{
				C->TextureTarget = Target.Get();
				C->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
				C->FOVAngle = Fov;
				C->bCaptureEveryFrame = true;
				C->bCaptureOnMovement = false;
				C->bAlwaysPersistRenderingState = true;
				if (OrthoWidth > 0)
				{
					// Orthographic with the near plane at the camera: nothing behind (above) the camera is drawn,
					// so a downward camera placed inside a layer gives a cutaway floor plan.
					C->ProjectionType = ECameraProjectionMode::Orthographic;
					C->OrthoWidth = OrthoWidth;
					C->bAutoCalculateOrthoPlanes = false;
					C->bUpdateOrthoPlanes = false;
				}
			}
		}

		virtual bool Tick(FFAServer& Server) override
		{
			USceneCaptureComponent2D* C = Actor.IsValid() ? Actor->GetCaptureComponent2D() : nullptr;
			if (!C)
			{
				Server.ReplyError(Req, 500, TEXT("Capture actor was lost (world changed?)"));
				return true;
			}
			if (Frames++ < Warmup)
			{
				return false;  // let temporal effects, lighting and streaming settle
			}
			C->bCaptureEveryFrame = false;
			C->CaptureScene();
			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Res = Target->GameThread_GetRenderTargetResource();
			const bool bRead = Res && Res->ReadPixels(Pixels) && Pixels.Num() == W * H;
			Actor->Destroy();
			if (!bRead)
			{
				Server.ReplyError(Req, 500, TEXT("ReadPixels failed"));
				return true;
			}
			for (FColor& P : Pixels)
			{
				P.A = 255;
			}
			const FString Path = NewCapturePath(TEXT("cap"));
			if (!SavePng(Pixels, W, H, Path))
			{
				Server.ReplyError(Req, 500, TEXT("PNG encode or save failed"));
				return true;
			}
			const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("path"), Path);
			R->SetNumberField(TEXT("w"), W);
			R->SetNumberField(TEXT("h"), H);
			R->SetNumberField(TEXT("frames"), Frames);
			Server.Reply(Req, R);
			return true;
		}

	private:
		TWeakObjectPtr<ASceneCapture2D> Actor;
		TStrongObjectPtr<UTextureRenderTarget2D> Target;
		int32 W, H, Warmup, Frames = 0;
	};

	void Capture(FFAServer& Server, const FFARequest& Req)
	{
		UWorld* World = FACommands::FindGameWorld();
		if (!PlayerPawn(World))
		{
			Server.ReplyError(Req, 409, TEXT("No game loaded"));
			return;
		}
		FVector Loc;
		if (!ReadVec(Req.Params, TEXT("loc"), Loc))
		{
			Server.ReplyError(Req, 400, TEXT("capture needs loc [x,y,z] in cm"));
			return;
		}
		FRotator Rot = FRotator::ZeroRotator;
		FVector LookAt, Pyr;
		if (ReadVec(Req.Params, TEXT("look_at"), LookAt))
		{
			Rot = (LookAt - Loc).Rotation();
		}
		else if (ReadVec(Req.Params, TEXT("pyr"), Pyr))
		{
			Rot = FRotator(Pyr.X, Pyr.Y, Pyr.Z);
		}
		double Fov = 90, W = 1280, H = 720, Warmup = 8, Ortho = 0;
		Req.Params->TryGetNumberField(TEXT("fov"), Fov);
		Req.Params->TryGetNumberField(TEXT("ortho_width"), Ortho);
		Req.Params->TryGetNumberField(TEXT("w"), W);
		Req.Params->TryGetNumberField(TEXT("h"), H);
		Req.Params->TryGetNumberField(TEXT("warmup"), Warmup);
		W = FMath::Clamp(W, 64, 3840);
		H = FMath::Clamp(H, 64, 2160);
		Server.AddOp(MakeShared<FCaptureOp>(Req, World, Loc, Rot, (float)Fov, (float)Ortho, (int32)W, (int32)H, (int32)Warmup));
	}

	// ---------------------------------------------------------------- viewport screenshot (what the player sees)

	class FScreenshotOp : public FFAPendingOp
	{
	public:
		FScreenshotOp(const FFARequest& InReq, const FString& InPath) : FFAPendingOp(InReq), Path(InPath)
		{
			Deadline = FPlatformTime::Seconds() + 10.0;
			FScreenshotRequest::RequestScreenshot(Path, false, false);
		}
		virtual bool Tick(FFAServer& Server) override
		{
			if (IFileManager::Get().FileSize(*Path) > 0)
			{
				const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("path"), Path);
				Server.Reply(Req, R);
				return true;
			}
			if (FPlatformTime::Seconds() > Deadline)
			{
				Server.ReplyError(Req, 504, TEXT("Screenshot did not appear within 10 s"));
				return true;
			}
			return false;
		}
	private:
		FString Path;
		double Deadline;
	};

	void Screenshot(FFAServer& Server, const FFARequest& Req)
	{
		Server.AddOp(MakeShared<FScreenshotOp>(Req, NewCapturePath(TEXT("view"))));
	}
}

UWorld* FACommands::FindGameWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		UWorld* World = Ctx.World();
		if (World && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
		{
			return World;
		}
	}
	return nullptr;
}

bool FACommands::Handle(FFAServer& Server, const FFARequest& Req)
{
	using FHandler = void (*)(FFAServer&, const FFARequest&);
	static const TMap<FString, FHandler> Table = {
		{TEXT("hello"), &Hello},
		{TEXT("get_player"), &GetPlayer},
		{TEXT("query_area"), &QueryArea},
		{TEXT("capture"), &Capture},
		{TEXT("screenshot"), &Screenshot},
		{TEXT("build"), &FABuild::Build},
		{TEXT("dismantle"), &FABuild::Dismantle},
		{TEXT("connect"), &FABuild::Connect},
		{TEXT("undo"), &FABuild::Undo},
		{TEXT("list_recipes"), &FABuild::ListRecipes},
		{TEXT("set_recipe"), &FABuild::SetRecipe},
	};
	if (const FHandler* H = Table.Find(Req.Method))
	{
		(*H)(Server, Req);
		return true;
	}
	return false;
}
