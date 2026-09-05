#include "MonolithHttpServer.h"
#include "MonolithCoreModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithCoordination.h"
#include "MonolithSettings.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Internationalization/Regex.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

FMonolithHttpServer::FMonolithHttpServer()
{
}

FMonolithHttpServer::~FMonolithHttpServer()
{
	Stop();
}

bool FMonolithHttpServer::Start(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogMonolith, Warning, TEXT("HTTP server already running on port %d"), BoundPort);
		return true;
	}

	// On a fresh editor launch, the OS keeps the port in TIME_WAIT for up to
	// 2*MSL (~30s on macOS/Linux) after the previous editor shut down. Budget
	// ~40s total so a rapid close+reopen cycle doesn't drop the MCP server on
	// the floor.
	constexpr int32 MaxAttempts = 20;
	constexpr float BackoffSeconds = 2.0f;

	for (int32 Attempt = 1; Attempt <= MaxAttempts; ++Attempt)
	{
		if (Attempt > 1)
		{
			UE_LOG(LogMonolith, Warning, TEXT("HTTP bind attempt %d/%d on port %d — waiting %.1fs"),
				Attempt, MaxAttempts, Port, BackoffSeconds);
			FPlatformProcess::Sleep(BackoffSeconds);

			// Drop our routes only. We deliberately do NOT call
			// StopAllListeners() — it is process-wide and would stop every
			// other subsystem's listener too. Leaving the module's
			// bHttpListenersEnabled flag true is what lets the GetHttpRouter
			// call below evict a failed listener and build a fresh one; with
			// listeners disabled it would hand back the dead one instead.
			DeactivateRoutes();
		}

		HttpRouter = FHttpServerModule::Get().GetHttpRouter(Port, true);
		if (!HttpRouter.IsValid())
		{
			UE_LOG(LogMonolith, Warning, TEXT("GetHttpRouter failed on port %d (attempt %d)"), Port, Attempt);
			continue;
		}

		BindRoutes();
		FHttpServerModule::Get().StartAllListeners();

		// Brief wait for OS to complete bind before probing
		FPlatformProcess::Sleep(0.1f);

		if (ProbePort(Port))
		{
			bIsRunning = true;
			BoundPort = Port;
			StartTime = FDateTime::UtcNow();
			UE_LOG(LogMonolith, Log, TEXT("Monolith MCP server listening on port %d (attempt %d)"), Port, Attempt);
			return true;
		}

		UE_LOG(LogMonolith, Warning, TEXT("Port %d not listening after StartAllListeners (attempt %d)"), Port, Attempt);
	}

	UE_LOG(LogMonolith, Error, TEXT("Failed to bind Monolith MCP server on port %d after %d attempts (~%ds total)"),
		Port, MaxAttempts, static_cast<int32>(MaxAttempts * BackoffSeconds));
	// Clean up
	DeactivateRoutes();
	return false;
}

void FMonolithHttpServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	DeactivateRoutes();

	UE_LOG(LogMonolith, Log, TEXT("Monolith MCP server stopped"));
}

void FMonolithHttpServer::DeactivateRoutes()
{
	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
	}
	RouteHandles.Empty();

	// The HttpRouter and the module's listeners are intentionally left alone.
	// FHttpServerModule::StopAllListeners() is process-wide: it would silently
	// kill Web Remote Control, PerfCounters, ExternalRpcRegistry and every
	// other plugin's HTTP routes along with ours. Unbinding our own handles is
	// enough to take Monolith off the wire; the listener keeps holding the
	// port until the editor exits.
	bIsRunning = false;
	BoundPort = 0;
}

void FMonolithHttpServer::BindRoutes()
{
	if (!HttpRouter.IsValid()) return;

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandlePostMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleGetMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleDeleteMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleOptions)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleHealthCheck)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleOptions)));
}

bool FMonolithHttpServer::ProbePort(int32 Port)
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem) return false;

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(TEXT("127.0.0.1"), bValid);
	if (!bValid) return false;
	Addr->SetPort(Port);

	FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MonolithProbe"), false);
	if (!Socket) return false;

	bool bConnected = false;

	if (Socket->SetNonBlocking(true))
	{
		// Connect() on a non-blocking socket reports success for both
		// SE_EWOULDBLOCK and SE_EINPROGRESS, so its return value says nothing
		// about whether anything is listening. Wait for writability, then ask
		// the socket for its actual state — that is the verdict. Without this,
		// a probe of a closed port would "succeed" and we would log that we
		// are listening having bound nothing.
		Socket->Connect(*Addr);
		Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(100));
		bConnected = Socket->GetConnectionState() == SCS_Connected;
	}
	else
	{
		// Platform refused the mode change — fall back to a blocking connect.
		bConnected = Socket->Connect(*Addr);
	}

	Socket->Close();
	SocketSubsystem->DestroySocket(Socket);
	return bConnected;
}

bool FMonolithHttpServer::Restart(int32 Port)
{
	DeactivateRoutes();

	// Drop our router handle so Start() re-fetches one. The module's listeners
	// stay enabled, which is what lets GetHttpRouter evict a wedged listener
	// and rebuild it — StopAllListeners() would disable them and make the
	// module hand back the dead listener instead (besides stopping every other
	// plugin's routes).
	HttpRouter.Reset();

	return Start(Port);
}

// ============================================================================
// Route Handlers
// ============================================================================

bool FMonolithHttpServer::HandlePostMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectOrigin(Request, OnComplete)) return true;
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const int32 BodyLimit = FMath::Clamp(Settings ? Settings->MaxRequestBodyMB : 32, 1, 256) * 1024 * 1024;
	if (Request.Body.Num() > BodyLimit)
	{
		auto Response = MakeRejectedResponse(TEXT("MCP request body exceeds configured limit"),
			static_cast<EHttpServerResponseCodes>(413));
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}
	if (Request.Body.Contains(0))
	{
		auto Error = FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrParseError,
			TEXT("NUL bytes are not valid JSON; no actions executed."));
		auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Error), EHttpServerResponseCodes::BadRequest);
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("MCP-Protocol-Version"), ESearchCase::IgnoreCase))
		{
			const auto& Values = Header.Value;
			if (Values.Num() != 1 || (Values[0] != TEXT("2024-11-05") && Values[0] != TEXT("2025-03-26")
				&& Values[0] != TEXT("2025-06-18") && Values[0] != TEXT("2025-11-25")))
			{
				auto Response = MakeRejectedResponse(TEXT("Unsupported MCP-Protocol-Version"), EHttpServerResponseCodes::BadRequest);
				AddCorsHeaders(*Response, Request);
				OnComplete(MoveTemp(Response));
				return true;
			}
		}
	}
	// Parse body as UTF-8 JSON (Body is NOT null-terminated — must add terminator)
	TArray<uint8> NullTermBody(Request.Body);
	NullTermBody.Add(0);
	FString BodyString = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(NullTermBody.GetData())));
	if (BodyString.IsEmpty())
	{
		TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
			nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Empty request body — send a JSON-RPC 2.0 request, e.g. {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}."));
		auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Try parse as JSON
	TSharedPtr<FJsonObject> JsonRequest = FMonolithJsonUtils::Parse(BodyString);

	// Could be a single request or a batch (array)
	bool bBatch = false;
	TArray<TSharedPtr<FJsonObject>> Requests;
	TArray<TSharedPtr<FJsonObject>> Responses;

	if (JsonRequest.IsValid())
	{
		// Single request
		Requests.Add(JsonRequest);
	}
	else
	{
		// Try parsing as array (batch)
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (FJsonSerializer::Deserialize(Reader, JsonArray) && JsonArray.Num() > 0)
		{
			bBatch = true;
			if (JsonArray.Num() > FMath::Clamp(Settings ? Settings->MaxBatchRequests : 64, 1, 256))
			{
				auto Err = FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest,
					TEXT("Batch exceeds configured request limit; no actions executed."));
				auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
				AddCorsHeaders(*Response, Request);
				OnComplete(MoveTemp(Response));
				return true;
			}
			for (const TSharedPtr<FJsonValue>& Value : JsonArray)
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					Requests.Add(Value->AsObject());
				}
				else Requests.Add(nullptr);
			}
		}
		else
		{
			// UE's reader accepts object/array roots, so wrap a primitive to
			// distinguish valid scalar JSON (-32600) from broken syntax (-32700).
			TArray<TSharedPtr<FJsonValue>> WrappedValues;
			const bool bValidJson = FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(TEXT("[") + BodyString + TEXT("]")), WrappedValues) && WrappedValues.Num() == 1;
			TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
				nullptr, bValidJson ? FMonolithJsonUtils::ErrInvalidRequest : FMonolithJsonUtils::ErrParseError,
				TEXT("Invalid JSON-RPC body; expected a request object or nonempty legacy batch."));
			auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
			AddCorsHeaders(*Response, Request);
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	// Process each request
	TUniquePtr<FMonolithCoordination::FBatchScope> BatchScope;
	if (bBatch) { BatchScope = MakeUnique<FMonolithCoordination::FBatchScope>(FMonolithCoordination::Get()); }
	for (const TSharedPtr<FJsonObject>& Req : Requests)
	{
		TSharedPtr<FJsonObject> Resp = ProcessJsonRpcRequest(Req);
		if (Resp.IsValid())
		{
			// Only add response if it's not a notification (notifications have no id)
			Responses.Add(Resp);
		}
	}
	BatchScope.Reset();

	// Build response
	FString ResponseBody;
	if (Responses.Num() == 0)
	{
		// All notifications — 202 Accepted with no body
		auto Response = FHttpServerResponse::Ok();
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCorsHeaders(*Response, Request);
		OnComplete(MoveTemp(Response));
		return true;
	}
	else if (!bBatch && Responses.Num() == 1)
	{
		ResponseBody = FMonolithJsonUtils::Serialize(Responses[0]);
	}
	else
	{
		// Batch response — serialize as array
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		for (const TSharedPtr<FJsonObject>& Resp : Responses)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(Resp));
		}
		FString ArrayStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArrayStr);
		FJsonSerializer::Serialize(JsonArray, Writer);
		ResponseBody = ArrayStr;
	}

	auto Response = MakeJsonResponse(ResponseBody);
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleGetMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectOrigin(Request, OnComplete)) return true;
	// Stateless Streamable HTTP: no long-lived server event stream.
	auto Response = MakeJsonResponse(TEXT("{}"), static_cast<EHttpServerResponseCodes>(405));
	Response->Headers.Add(TEXT("Allow"), {TEXT("POST, OPTIONS")});
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleDeleteMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return HandleGetMcp(Request, OnComplete); // No server-side MCP session to delete.
}

bool FMonolithHttpServer::HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectOrigin(Request, OnComplete)) return true;
	auto Response = FHttpServerResponse::Ok();
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleHealthCheck(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectOrigin(Request, OnComplete)) return true;
	TSharedPtr<FJsonObject> Health = MakeShared<FJsonObject>();
	Health->SetStringField(TEXT("status"), TEXT("ok"));
	Health->SetNumberField(TEXT("port"), BoundPort);
	Health->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Health->SetStringField(TEXT("version"), MONOLITH_VERSION);

	const FTimespan Uptime = FDateTime::UtcNow() - StartTime;
	Health->SetNumberField(TEXT("uptime_seconds"), static_cast<double>(Uptime.GetTotalSeconds()));

	Health->SetNumberField(TEXT("tools_registered"), FMonolithToolRegistry::Get().GetActionCount());

	FString Body;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	FJsonSerializer::Serialize(Health.ToSharedRef(), Writer);

	auto Response = MakeJsonResponse(Body);
	AddCorsHeaders(*Response, Request);
	OnComplete(MoveTemp(Response));
	return true;
}

// ============================================================================
// JSON-RPC 2.0 Processing
// ============================================================================

TSharedPtr<FJsonObject> FMonolithHttpServer::ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	if (!Request.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Invalid request object — must be a JSON object with jsonrpc, method, and id fields."));
	}

	// Validate jsonrpc version
	FString Version;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), Version) || Version != TEXT("2.0"))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing or invalid jsonrpc version — set \"jsonrpc\" to the string \"2.0\"."));
	}

	// Get method
	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing method field — set \"method\" to one of: initialize, tools/list, tools/call, ping."));
	}

	// Get id (null for notifications)
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
	bool bIsNotification = !Id.IsValid();
	if (bIsNotification) return nullptr; // Never execute a tools/call notification.
	if (Id->Type != EJson::String && Id->Type != EJson::Number && Id->Type != EJson::Null)
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Invalid request id"));
	if (Request->HasField(TEXT("params")) && !Request->HasTypedField<EJson::Object>(TEXT("params")))
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("params must be an object"));

	// Get params
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Request->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj)
	{
		Params = *ParamsObj;
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	UE_LOG(LogMonolith, Verbose, TEXT("JSON-RPC: %s (id=%s)"), *Method, Id->Type == EJson::String ? *Id->AsString() : TEXT("numeric/null"));

	// Dispatch by method
	TSharedPtr<FJsonObject> Response;

	if (Method == TEXT("initialize"))
	{
		Response = HandleInitialize(Id, Params);
	}
	else if (Method == TEXT("notifications/initialized"))
	{
		// Notification — no response
		return nullptr;
	}
	else if (Method == TEXT("tools/list"))
	{
		Response = HandleToolsList(Id, Params);
	}
	else if (Method == TEXT("tools/call"))
	{
		Response = HandleToolsCall(Id, Params);
	}
	else if (Method == TEXT("ping"))
	{
		Response = HandlePing(Id);
	}
	else
	{
		Response = FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown method: %s — use tools/list to enumerate available tools, then tools/call."), *Method));
	}

	// Notifications don't get responses
	if (bIsNotification)
	{
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleInitialize(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Protocol version negotiation: echo the client's requested version if we
	// support it, otherwise fall back to the latest we support.
	FString ClientVersion;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), ClientVersion)
		&& (ClientVersion == TEXT("2024-11-05") || ClientVersion == TEXT("2025-03-26")
			|| ClientVersion == TEXT("2025-06-18") || ClientVersion == TEXT("2025-11-25")))
	{
		Result->SetStringField(TEXT("protocolVersion"), ClientVersion);
	}
	else
	{
		Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-11-25"));
	}

	// Server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("monolith"));
	ServerInfo->SetStringField(TEXT("version"), MONOLITH_VERSION);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	// Capabilities
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();

	// We support tools
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);

	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	// Onboarding hint so agents discover schemas instead of guessing parameter names.
	Result->SetStringField(TEXT("instructions"),
		TEXT("Monolith MCP server for Unreal Engine. ")
		TEXT("Before calling a domain action, check its schema instead of guessing: ")
		TEXT("monolith_discover() lists namespaces, monolith_discover('<namespace>') lists a ")
		TEXT("namespace's action names + descriptions (terse by default — pass detail=true to ")
		TEXT("inline param schemas), and describe_query('action_schema', ...) returns one action's ")
		TEXT("exact parameter schema. monolith_guide(section='recipes') gives cross-namespace ")
		TEXT("workflows, decision matrices, and gotchas. For multi-agent edits acquire monolith_coordination ")
		TEXT("and pass _lease_token on domain calls; renew before expiry. Transport timeouts have unknown execution outcome."));

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// Each namespace becomes a tool
	TArray<FString> Namespaces = Registry.GetNamespaces();
	for (const FString& Namespace : Namespaces)
	{
		TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);
		if (Actions.Num() == 0) continue;

		// Build the tool entry for this namespace
		// Format: "namespace_query" with action as a parameter
		TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();

		if (Namespace == TEXT("monolith"))
		{
			// Core tools are individual: monolith_discover, monolith_status
			for (const FMonolithActionInfo& ActionInfo : Actions)
			{
				TSharedPtr<FJsonObject> CoreTool = MakeShared<FJsonObject>();
				CoreTool->SetStringField(TEXT("name"), FString::Printf(TEXT("monolith_%s"), *ActionInfo.Action));
				CoreTool->SetStringField(TEXT("description"), ActionInfo.Description);

				// Input schema
				TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
				InputSchema->SetStringField(TEXT("type"), TEXT("object"));
				// Transport metadata must be visible to schema-driven MCP clients.
				// Copy the property map so listing tools never mutates action schemas.
				auto CoreProperties = MakeShared<FJsonObject>();
				if (ActionInfo.ParamSchema.IsValid())
				{
					CoreProperties->Values = ActionInfo.ParamSchema->Values;
				}
				if (!CoreProperties->HasField(TEXT("_lease_token")))
				{
					auto LeaseTokenProperty = MakeShared<FJsonObject>();
					LeaseTokenProperty->SetStringField(TEXT("type"), TEXT("string"));
					LeaseTokenProperty->SetStringField(TEXT("description"),
						TEXT("Workflow token returned by monolith_coordination. Required for protected tools while the editor is leased."));
					CoreProperties->SetObjectField(TEXT("_lease_token"), LeaseTokenProperty);
				}
				InputSchema->SetObjectField(TEXT("properties"), CoreProperties);
				CoreTool->SetObjectField(TEXT("inputSchema"), InputSchema);

				// Survivor A (plan §3.A) — MCP-spec tool annotations. Only emit
				// the `annotations` block when at least one hint is non-default;
				// avoids bloating the wire with default-false annotations on every
				// individually-registered top-level tool. Spec ref:
				// modelcontextprotocol.io/specification/2025-06-18/server/tools
				const bool bAnyHint = ActionInfo.bReadOnlyHint
					|| ActionInfo.bDestructiveHint
					|| ActionInfo.bIdempotentHint
					|| !ActionInfo.Title.IsEmpty();
				if (bAnyHint)
				{
					TSharedPtr<FJsonObject> Ann = MakeShared<FJsonObject>();
					Ann->SetBoolField(TEXT("readOnlyHint"), ActionInfo.bReadOnlyHint);
					Ann->SetBoolField(TEXT("destructiveHint"), ActionInfo.bDestructiveHint);
					Ann->SetBoolField(TEXT("idempotentHint"), ActionInfo.bIdempotentHint);
					if (!ActionInfo.Title.IsEmpty())
					{
						Ann->SetStringField(TEXT("title"), ActionInfo.Title);
					}
					CoreTool->SetObjectField(TEXT("annotations"), Ann);
				}

				ToolsArray.Add(MakeShared<FJsonValueObject>(CoreTool));
			}
		}
		else
		{
			// Domain tools use the dispatch pattern: namespace_query (underscore, not dot)
			// Dots in tool names break Claude Code's mcp__server__tool mapping.
			FString ToolName = FString::Printf(TEXT("%s_query"), *Namespace);
			Tool->SetStringField(TEXT("name"), ToolName);

			// Action names still drive the `action` enum below (authoritative value
			// constraint). The full list is NOT duplicated into the description prose —
			// that copy was ~32k chars (~41% of the tools/list manifest). Clients fetch
			// the list on demand via monolith_discover("<namespace>").
			TArray<FString> ActionNames;
			for (const FMonolithActionInfo& ActionInfo : Actions)
			{
				ActionNames.Add(ActionInfo.Action);
			}

			const FString Description = FString::Printf(
				TEXT("Query the %s domain. Call monolith_discover(\"%s\") for the action list (name + description); pass detail=true or call describe_query action_schema for an action's full param schema."),
				*Namespace, *Namespace);
			Tool->SetStringField(TEXT("description"), Description);

			// Build input schema
			TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));

			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

			// "action" property (required)
			TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
			ActionProp->SetStringField(TEXT("type"), TEXT("string"));
			ActionProp->SetStringField(TEXT("description"), TEXT("The action to execute"));
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (const FString& Name : ActionNames)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Name));
			}
			ActionProp->SetArrayField(TEXT("enum"), EnumValues);
			Properties->SetObjectField(TEXT("action"), ActionProp);

			// "params" property — keep lightweight; per-action schemas come from
			// describe_query action_schema (or monolith_discover detail=true).
			TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
			ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
			ParamsProp->SetStringField(TEXT("description"),
				FString::Printf(TEXT("Parameters for the action. monolith_discover(\"%s\") is terse by default (name + description); pass detail=true for full param schemas, or call describe_query action_schema for one action."), *Namespace));
			Properties->SetObjectField(TEXT("params"), ParamsProp);

			InputSchema->SetObjectField(TEXT("properties"), Properties);
			InputSchema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("action"))});

			Tool->SetObjectField(TEXT("inputSchema"), InputSchema);

			// Survivor A (plan §3.A) — MCP-spec dispatcher annotations. Pulled
			// from the registry's per-namespace dispatcher map (set via
			// FMonolithToolRegistry::SetDispatcherAnnotations at module init).
			// Untagged dispatchers leave IsAnyNonDefault()==false → no
			// `annotations` block on the wire.
			const FMonolithDispatcherAnnotations DispatcherAnn = Registry.GetDispatcherAnnotations(Namespace);
			if (DispatcherAnn.IsAnyNonDefault())
			{
				TSharedPtr<FJsonObject> Ann = MakeShared<FJsonObject>();
				Ann->SetBoolField(TEXT("readOnlyHint"), DispatcherAnn.bReadOnlyHint);
				Ann->SetBoolField(TEXT("destructiveHint"), DispatcherAnn.bDestructiveHint);
				Ann->SetBoolField(TEXT("idempotentHint"), DispatcherAnn.bIdempotentHint);
				if (!DispatcherAnn.Title.IsEmpty())
				{
					Ann->SetStringField(TEXT("title"), DispatcherAnn.Title);
				}
				Tool->SetObjectField(TEXT("annotations"), Ann);
			}

			ToolsArray.Add(MakeShared<FJsonValueObject>(Tool));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), ToolsArray);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing params — tools/call params must include \"name\" and optionally \"arguments\"."));
	}

	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing tool name — set params.name to a tool like monolith_discover or <namespace>_query."));
	}

	if (Params->HasField(TEXT("arguments")) && !Params->HasTypedField<EJson::Object>(TEXT("arguments")))
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("arguments must be an object"));
	// Get arguments
	TSharedPtr<FJsonObject> Arguments;
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj)
	{
		Arguments = *ArgsObj;
	}
	if (!Arguments.IsValid())
	{
		Arguments = MakeShared<FJsonObject>();
	}

	FString Namespace;
	FString Action;

	// Determine dispatch pattern
	if (ToolName.StartsWith(TEXT("monolith_")))
	{
		// Core tool: monolith_discover -> namespace="monolith", action="discover"
		Namespace = TEXT("monolith");
		Action = ToolName.Mid(9);

		// Symmetric string-unwrap + top-level-extras merge (mirrors the *_query
		// branch below). Some MCP clients (Claude Code) serialize the "params"
		// object as a JSON-encoded string; others nest a real object; others
		// scatter optional shaping flags (_fields/_omit/_compact_json) at the
		// top level alongside the tool-specific args. Normalise all shapes
		// so the dispatched action handler sees a single flat params object.
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : Arguments->Values)
		{
			if (Pair.Key != TEXT("params"))
			{
				TopLevelExtras->SetField(Pair.Key, Pair.Value);
			}
		}

		const TSharedPtr<FJsonObject>* NestedParams = nullptr;
		TSharedPtr<FJsonObject> ParsedParamsObj; // lifetime holder for string-parsed params
		bool bHasNestedParams = false;

		if (Arguments->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams)
		{
			bHasNestedParams = true;
		}
		else
		{
			// Try parsing "params" as a JSON string (Claude Code serializes objects to strings)
			FString ParamsStr;
			if (Arguments->TryGetStringField(TEXT("params"), ParamsStr) && !ParamsStr.IsEmpty())
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsStr);
				if (FJsonSerializer::Deserialize(Reader, ParsedParamsObj) && ParsedParamsObj.IsValid())
				{
					NestedParams = &ParsedParamsObj;
					bHasNestedParams = true;
				}
			}
		}

		if (bHasNestedParams && NestedParams)
		{
			Arguments = MakeShared<FJsonObject>();
			// Start with top-level extras (lower priority)
			for (const auto& Pair : TopLevelExtras->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
			// Overlay nested params (higher priority)
			for (const auto& Pair : (*NestedParams)->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
		}
		else
		{
			// No nested "params" object — use top-level fields as params directly.
			// If a "params" field exists but is NOT an object (or an object-parsable
			// string), it is an ACTION param that happens to be named "params"
			// (e.g. set_event_dispatcher_params) — keep it instead of silently
			// dropping it; the registry's string-recovery pass restores its
			// declared type.
			if (TSharedPtr<FJsonValue> RawParamsField = Arguments->TryGetField(TEXT("params")))
			{
				TopLevelExtras->SetField(TEXT("params"), RawParamsField);
			}
			Arguments = TopLevelExtras;
		}
	}
	else if (ToolName.EndsWith(TEXT("_query")) || ToolName.EndsWith(TEXT(".query")))
	{
		// Domain tool: blueprint_query (or legacy blueprint.query) -> namespace="blueprint"
		Namespace = ToolName.Left(ToolName.Len() - 6); // strip "_query" or ".query"

		if (!Arguments->TryGetStringField(TEXT("action"), Action))
		{
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' in arguments — for *_query tools, set arguments.action; call monolith_discover(\"<namespace>\") to enumerate."));
		}

		// Collect top-level fields (excluding reserved keys) — MCP clients may
		// place optional params like members_only alongside "action" rather than
		// nesting them inside "params".
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : Arguments->Values)
		{
			if (Pair.Key != TEXT("action") && Pair.Key != TEXT("params"))
			{
				TopLevelExtras->SetField(Pair.Key, Pair.Value);
			}
		}

		// Extract nested params if present, then merge in any top-level extras
		// NOTE: Claude Code sends "params" as a JSON-encoded string, not a nested object.
		// We must handle both cases.
		const TSharedPtr<FJsonObject>* NestedParams = nullptr;
		TSharedPtr<FJsonObject> ParsedParamsObj; // lifetime holder for string-parsed params
		bool bHasNestedParams = false;

		if (Arguments->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams)
		{
			bHasNestedParams = true;
		}
		else
		{
			// Try parsing "params" as a JSON string (Claude Code serializes objects to strings)
			FString ParamsStr;
			if (Arguments->TryGetStringField(TEXT("params"), ParamsStr) && !ParamsStr.IsEmpty())
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsStr);
				if (FJsonSerializer::Deserialize(Reader, ParsedParamsObj) && ParsedParamsObj.IsValid())
				{
					NestedParams = &ParsedParamsObj;
					bHasNestedParams = true;
				}
			}
		}

		if (bHasNestedParams && NestedParams)
		{
			Arguments = MakeShared<FJsonObject>();
			// Start with top-level extras (lower priority)
			for (const auto& Pair : TopLevelExtras->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
			// Overlay nested params (higher priority)
			for (const auto& Pair : (*NestedParams)->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
		}
		else
		{
			// No nested "params" object — use top-level fields as params directly.
			// If a "params" field exists but is NOT an object (or an object-parsable
			// string), it is an ACTION param that happens to be named "params"
			// (e.g. set_event_dispatcher_params) — keep it instead of silently
			// dropping it; the registry's string-recovery pass restores its
			// declared type.
			if (TSharedPtr<FJsonValue> RawParamsField = Arguments->TryGetField(TEXT("params")))
			{
				TopLevelExtras->SetField(TEXT("params"), RawParamsField);
			}
			Arguments = TopLevelExtras;
		}
	}
	else
	{
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown tool: %s — tool must start with monolith_ or end with _query; call tools/list to enumerate."), *ToolName));
	}

	// Record start time for duration measurement without shadowing the server start timestamp member.
	double ActionStartTimeSeconds = FPlatformTime::Seconds();

	// Execute via registry
	FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, Arguments, /*bInheritLeaseContext=*/false);

	// Calculate duration
	double DurationMs = (FPlatformTime::Seconds() - ActionStartTimeSeconds) * 1000.0;
	UE_LOG(LogMonolith, Verbose, TEXT("Monolith action %s.%s completed in %.2f ms"), *Namespace, *Action, DurationMs);

	// Build MCP tool result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Content;

	if (ActionResult.bSuccess)
	{
		Result->SetObjectField(TEXT("structuredContent"), ActionResult.Result.IsValid()
			? ActionResult.Result : MakeShared<FJsonObject>());
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		if (ActionResult.Result.IsValid())
		{
			TextContent->SetStringField(TEXT("text"), FMonolithJsonUtils::Serialize(ActionResult.Result));
		}
		else
		{
			TextContent->SetStringField(TEXT("text"), TEXT("{}"));
		}
		Content.Add(MakeShared<FJsonValueObject>(TextContent));
		Result->SetBoolField(TEXT("isError"), false);
	}
	else
	{
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), ActionResult.ErrorMessage);
		Content.Add(MakeShared<FJsonValueObject>(TextContent));
		Result->SetBoolField(TEXT("isError"), true);
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("error"), ActionResult.ErrorMessage);
		Error->SetNumberField(TEXT("code"), ActionResult.ErrorCode);
		if (ActionResult.ErrorData.IsValid()) Error->SetField(TEXT("data"), ActionResult.ErrorData);
		Result->SetObjectField(TEXT("structuredContent"), Error);
		TextContent->SetStringField(TEXT("text"), FMonolithJsonUtils::Serialize(Error));
	}

	Result->SetArrayField(TEXT("content"), Content);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandlePing(const TSharedPtr<FJsonValue>& Id)
{
	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
}

// ============================================================================
// Helpers
// ============================================================================

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeJsonResponse(const FString& JsonBody, EHttpServerResponseCodes Code)
{
	auto Response = FHttpServerResponse::Create(JsonBody, TEXT("application/json"));
	Response->Code = Code;
	return Response;
}

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeSseResponse(const TArray<TSharedPtr<FJsonObject>>& Messages)
{
	FString SseBody;
	for (const TSharedPtr<FJsonObject>& Msg : Messages)
	{
		SseBody += TEXT("event: message\ndata: ");
		SseBody += FMonolithJsonUtils::Serialize(Msg);
		SseBody += TEXT("\n\n");
	}

	auto Response = FHttpServerResponse::Create(SseBody, TEXT("text/event-stream"));
	Response->Code = EHttpServerResponseCodes::Ok;
	return Response;
}

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeRejectedResponse(const FString& Message, EHttpServerResponseCodes Code)
{
	auto Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("executed"), false);
	// The body has not been parsed, so the request ID is deliberately unknown.
	auto Error = FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest,
		Message, MakeShared<FJsonValueObject>(Data));
	return MakeJsonResponse(FMonolithJsonUtils::Serialize(Error), Code);
}

namespace
{
	// Allowlisted origins for browser CORS. Loopback only — the MCP server
	// is a developer tool and should never be exposed cross-origin to the
	// public web. Replaces the previous wildcard `*` that allowed any
	// website to read project data via a tab pinging localhost (Issue #38).
	//
	// Includes IPv6 loopback `[::1]` because some browsers prefer it over
	// 127.0.0.1 when resolving `localhost`. Anchored with ^ and $ so
	// subdomain attacks like `http://localhost.evil.com` are rejected.
	bool IsAllowedOrigin(const FString& Origin)
	{
		if (Origin.IsEmpty()) return false;

		// Reject the literal string "null" (sandboxed iframes / file:// origins).
		if (Origin.Equals(TEXT("null"), ESearchCase::IgnoreCase)) return false;

		// Match: http(s)://localhost[:NNNN], http(s)://127.0.0.1[:NNNN],
		// http(s)://[::1][:NNNN]. Reject anything else.
		static const FRegexPattern Pattern(
			TEXT("^https?://(localhost|127\\.0\\.0\\.1|\\[::1\\])(:\\d+)?$"));
		FRegexMatcher Matcher(Pattern, Origin);
		return Matcher.FindNext();
	}
}

void FMonolithHttpServer::AddCorsHeaders(FHttpServerResponse& Response, const FHttpServerRequest& Request)
{
	// Always advertise the methods/headers we support — these are not
	// origin-sensitive. The allow-origin echo is the gated piece.
	Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), {TEXT("GET, POST, DELETE, OPTIONS")});
	Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), {TEXT("Content-Type, Accept, MCP-Protocol-Version, MCP-Session-Id")});
	Response.Headers.Add(TEXT("Vary"), {TEXT("Origin")});

	FString Origin;
	for (const auto& Header : Request.Headers)
		if (Header.Key.Equals(TEXT("Origin"), ESearchCase::IgnoreCase) && Header.Value.Num() == 1)
			Origin = Header.Value[0];

	if (IsAllowedOrigin(Origin))
	{
		Response.Headers.Add(TEXT("Access-Control-Allow-Origin"), {Origin});
	}
	// else: omit ACAO entirely — browsers will block the response from being
	// read by the requesting page. Same-origin and non-browser callers
	// (Claude Code via the proxy) are unaffected.
}


bool FMonolithHttpServer::RejectOrigin(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	int32 OriginCount = 0;
	bool bInvalid = false;
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Origin"), ESearchCase::IgnoreCase))
		{
			++OriginCount;
			bInvalid |= Header.Value.Num() != 1 || !IsAllowedOrigin(Header.Value.Num() == 1 ? Header.Value[0] : FString());
		}
	}
	if (!bInvalid && OriginCount <= 1) return false;
	auto Response = MakeRejectedResponse(TEXT("Origin not allowed"), static_cast<EHttpServerResponseCodes>(403));
	OnComplete(MoveTemp(Response));
	return true;
}
