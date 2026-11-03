#include "FAServer.h"

#include "Common/TcpListener.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace
{
	FString ToJsonLine(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		Out.AppendChar(TEXT('\n'));
		return Out;
	}

	FString TokenPath()
	{
		return FPaths::ProjectSavedDir() / TEXT("FicsitAgents") / TEXT("token.txt");
	}
}

void FFAServer::Start()
{
	Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FFileHelper::SaveStringToFile(Token, *TokenPath());

	const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), DefaultPort);
	// Not reusable: if another game instance holds the port, we want to fail rather than share it.
	Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(100), false);
	Listener->OnConnectionAccepted().BindRaw(this, &FFAServer::OnAccept);

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FFAServer::Tick), 0.0f);
	UE_LOG(LogFicsitAgents, Log, TEXT("Agent server listening on 127.0.0.1:%d, token file %s"), DefaultPort, *TokenPath());
}

void FFAServer::Stop()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Listener.Reset();
	FSocket* Pending = nullptr;
	while (Accepted.Dequeue(Pending))
	{
		Pending->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Pending);
	}
	TArray<int32> Ids;
	Clients.GetKeys(Ids);
	for (int32 Id : Ids)
	{
		CloseClient(Id);
	}
	Ops.Empty();
}

bool FFAServer::OnAccept(FSocket* Socket, const FIPv4Endpoint& Endpoint)
{
	// Listener thread. Hand the socket to the game thread.
	Accepted.Enqueue(Socket);
	return true;
}

bool FFAServer::Tick(float DeltaTime)
{
	FSocket* NewSocket = nullptr;
	while (Accepted.Dequeue(NewSocket))
	{
		NewSocket->SetNonBlocking(true);
		NewSocket->SetNoDelay(true);
		const int32 Id = NextClientId++;
		Clients.Add(Id, FClient{NewSocket});
		UE_LOG(LogFicsitAgents, Log, TEXT("Agent client %d connected"), Id);
	}

	TArray<int32> Closed;
	for (TPair<int32, FClient>& Pair : Clients)
	{
		bool bClosed = false;
		PumpClient(Pair.Key, Pair.Value, bClosed);
		if (bClosed)
		{
			Closed.Add(Pair.Key);
		}
	}
	for (int32 Id : Closed)
	{
		CloseClient(Id);
	}

	for (int32 i = Ops.Num() - 1; i >= 0; --i)
	{
		TSharedPtr<FFAPendingOp> Op = Ops[i];
		if (Op->Tick(*this))
		{
			Ops.RemoveAt(i);
		}
	}
	return true;
}

void FFAServer::PumpClient(int32 ClientId, FClient& Client, bool& bOutClosed)
{
	uint8 Buffer[16 * 1024];
	while (true)
	{
		int32 Read = 0;
		const bool bOk = Client.Socket->Recv(Buffer, sizeof(Buffer), Read, ESocketReceiveFlags::None);
		// FSocketBSD::Recv on a stream socket: false = peer closed or error, true with 0 bytes = nothing to read yet.
		if (!bOk)
		{
			bOutClosed = true;
			break;
		}
		if (Read == 0)
		{
			break;
		}
		Client.InBuf.Append(Buffer, Read);
		if (Client.InBuf.Num() > 8 * 1024 * 1024)
		{
			UE_LOG(LogFicsitAgents, Warning, TEXT("Client %d sent an oversized line, closing"), ClientId);
			bOutClosed = true;
			return;
		}
	}

	int32 Start = 0;
	for (int32 i = 0; i < Client.InBuf.Num(); ++i)
	{
		if (Client.InBuf[i] == '\n')
		{
			const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Client.InBuf.GetData() + Start), i - Start);
			FString Line(Conv.Length(), Conv.Get());
			Line.TrimStartAndEndInline();
			if (!Line.IsEmpty())
			{
				HandleLine(ClientId, Client, Line);
			}
			Start = i + 1;
		}
	}
	if (Start > 0)
	{
		Client.InBuf.RemoveAt(0, Start);
	}
}

void FFAServer::HandleLine(int32 ClientId, FClient& Client, const FString& Line)
{
	FFARequest Req;
	Req.ClientId = ClientId;

	TSharedPtr<FJsonObject> Msg;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Msg) || !Msg.IsValid())
	{
		ReplyError(Req, -32700, TEXT("Parse error: each line must be one JSON object"));
		return;
	}
	Req.Id = Msg->TryGetField(TEXT("id"));
	Msg->TryGetStringField(TEXT("method"), Req.Method);
	const TSharedPtr<FJsonObject>* Params = nullptr;
	Req.Params = Msg->TryGetObjectField(TEXT("params"), Params) ? *Params : MakeShared<FJsonObject>();

	if (!Client.bAuthed)
	{
		FString GivenToken;
		Req.Params->TryGetStringField(TEXT("token"), GivenToken);
		if (Req.Method != TEXT("hello") || GivenToken != Token)
		{
			ReplyError(Req, 401, FString::Printf(TEXT("Send hello with the token from %s first"), *TokenPath()));
			return;
		}
		Client.bAuthed = true;
	}
	Dispatch(Req);
}

void FFAServer::Dispatch(const FFARequest& Req)
{
	if (!FACommands::Handle(*this, Req))
	{
		ReplyError(Req, -32601, FString::Printf(TEXT("Unknown method '%s'"), *Req.Method));
	}
}

void FFAServer::Reply(const FFARequest& Req, const TSharedPtr<FJsonObject>& Result)
{
	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	if (Req.Id.IsValid())
	{
		Msg->SetField(TEXT("id"), Req.Id);
	}
	Msg->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonObject>());
	SendLine(Req.ClientId, ToJsonLine(Msg));
}

void FFAServer::ReplyError(const FFARequest& Req, int32 Code, const FString& Message)
{
	const TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
	Err->SetNumberField(TEXT("code"), Code);
	Err->SetStringField(TEXT("message"), Message);
	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	if (Req.Id.IsValid())
	{
		Msg->SetField(TEXT("id"), Req.Id);
	}
	Msg->SetObjectField(TEXT("error"), Err);
	SendLine(Req.ClientId, ToJsonLine(Msg));
}

bool FFAServer::SendLine(int32 ClientId, const FString& Line)
{
	FClient* Client = Clients.Find(ClientId);
	if (!Client)
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Line);
	const uint8* Data = reinterpret_cast<const uint8*>(Utf8.Get());
	int32 Remaining = Utf8.Length();
	double Deadline = FPlatformTime::Seconds() + 5.0;
	while (Remaining > 0)
	{
		int32 Sent = 0;
		if (!Client->Socket->Send(Data, Remaining, Sent))
		{
			const ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
			if (Err != SE_EWOULDBLOCK || FPlatformTime::Seconds() > Deadline)
			{
				UE_LOG(LogFicsitAgents, Warning, TEXT("Send to client %d failed"), ClientId);
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
			continue;
		}
		Data += Sent;
		Remaining -= Sent;
	}
	return true;
}

void FFAServer::CloseClient(int32 ClientId)
{
	if (FClient* Client = Clients.Find(ClientId))
	{
		Client->Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client->Socket);
		Clients.Remove(ClientId);
		UE_LOG(LogFicsitAgents, Log, TEXT("Agent client %d disconnected"), ClientId);
	}
}
