#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Containers/Queue.h"
#include "Dom/JsonObject.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFicsitAgents, Log, All);

class FSocket;
class FTcpListener;
struct FIPv4Endpoint;
class FFAServer;

/** One JSON-RPC style request from a client. */
struct FFARequest
{
	int32 ClientId = 0;
	TSharedPtr<FJsonValue> Id;
	FString Method;
	TSharedPtr<FJsonObject> Params;
};

/** A request that finishes over several game ticks (captures, builds). */
class FFAPendingOp
{
public:
	explicit FFAPendingOp(const FFARequest& InReq) : Req(InReq) {}
	virtual ~FFAPendingOp() = default;
	/** Return true when finished. The op must have sent its reply by then. */
	virtual bool Tick(FFAServer& Server) = 0;
	FFARequest Req;
};

/**
 * Line-based JSON-RPC server for agents.
 * Listens on 127.0.0.1 only. One JSON object per line in both directions.
 * Every request runs on the game thread.
 */
class FFAServer
{
public:
	static constexpr int32 ProtocolVersion = 1;
	static constexpr int32 DefaultPort = 47777;

	void Start();
	void Stop();

	void Reply(const FFARequest& Req, const TSharedPtr<FJsonObject>& Result);
	void ReplyError(const FFARequest& Req, int32 Code, const FString& Message);
	void AddOp(TSharedPtr<FFAPendingOp> Op) { Ops.Add(Op); }

	const FString& GetToken() const { return Token; }

private:
	struct FClient
	{
		FSocket* Socket = nullptr;
		TArray<uint8> InBuf;
		bool bAuthed = false;
	};

	bool Tick(float DeltaTime);
	bool OnAccept(FSocket* Socket, const FIPv4Endpoint& Endpoint);
	void PumpClient(int32 ClientId, FClient& Client, bool& bOutClosed);
	void HandleLine(int32 ClientId, FClient& Client, const FString& Line);
	void Dispatch(const FFARequest& Req);
	bool SendLine(int32 ClientId, const FString& Line);
	void CloseClient(int32 ClientId);

	TUniquePtr<FTcpListener> Listener;
	TQueue<FSocket*, EQueueMode::Mpsc> Accepted;
	TMap<int32, FClient> Clients;
	int32 NextClientId = 1;
	FString Token;
	FTSTicker::FDelegateHandle TickHandle;
	TArray<TSharedPtr<FFAPendingOp>> Ops;
};

/** Command handlers. Each returns true if it handled (replied to or queued) the request. */
namespace FACommands
{
	UWorld* FindGameWorld();
	bool Handle(FFAServer& Server, const FFARequest& Req);
}

namespace FABuild
{
	void Build(FFAServer& Server, const FFARequest& Req);
	void Dismantle(FFAServer& Server, const FFARequest& Req);
	void Undo(FFAServer& Server, const FFARequest& Req);
	void ListRecipes(FFAServer& Server, const FFARequest& Req);
	void SetRecipe(FFAServer& Server, const FFARequest& Req);
}
