#include "Misc/AutomationTest.h"
#include "MonolithHttpServer.h"
#include "MonolithCoreModule.h"
#include "MonolithCoreTools.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "MonolithCoordination.h"
#include "MonolithParamSchema.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithHttpProtocolTest,
    "Monolith.Core.HttpProtocol", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHttpProtocolTest::RunTest(const FString& Parameters)
{
    FMonolithHttpServer Server;
    int32 Executed = 0;
    const FString Instance = FMonolithCoreModule::Get().GetServerInstance().ToString(EGuidFormats::DigitsWithHyphens);
    FGuid InstanceGuid;
    TestTrue(TEXT("Module startup identity is a valid GUID"), FGuid::Parse(Instance, InstanceGuid) && InstanceGuid.IsValid());
    auto& Registry = FMonolithToolRegistry::Get();

    // Core tools need an explicit transport-token argument even when their
    // action has no parameter schema. tools/list must not modify registry data.
    TMap<FString, FString> OriginalSchemas;
    for (const auto& Action : Registry.GetActions(TEXT("monolith")))
        OriginalSchemas.Add(Action.Action, Action.ParamSchema.IsValid()
            ? FMonolithJsonUtils::Serialize(Action.ParamSchema) : TEXT("<no schema>"));
    const auto ToolsReply = Server.HandleToolsList(MakeShared<FJsonValueNumber>(99), MakeShared<FJsonObject>());
    const auto& ListedTools = ToolsReply->GetObjectField(TEXT("result"))->GetArrayField(TEXT("tools"));
    int32 CoreToolsChecked = 0;
    for (const auto& Value : ListedTools)
    {
        const auto Tool = Value->AsObject();
        if (!Tool->GetStringField(TEXT("name")).StartsWith(TEXT("monolith_"))) continue;
        ++CoreToolsChecked;
        const auto Properties = Tool->GetObjectField(TEXT("inputSchema"))->GetObjectField(TEXT("properties"));
        TestTrue(TEXT("Core tool advertises lease token"), Properties->HasTypedField<EJson::Object>(TEXT("_lease_token")));
        if (Properties->HasTypedField<EJson::Object>(TEXT("_lease_token")))
            TestEqual(TEXT("Lease argument is a string"), Properties->GetObjectField(TEXT("_lease_token"))->GetStringField(TEXT("type")), FString(TEXT("string")));
    }
    TestEqual(TEXT("Every registered core tool checked"), CoreToolsChecked, OriginalSchemas.Num());
    TestTrue(TEXT("Core tool schemas actually exercised"), CoreToolsChecked > 0);
    for (const auto& Action : Registry.GetActions(TEXT("monolith")))
        TestEqual(TEXT("Listing did not modify registry schema"), Action.ParamSchema.IsValid()
            ? FMonolithJsonUtils::Serialize(Action.ParamSchema) : FString(TEXT("<no schema>")), OriginalSchemas.FindRef(Action.Action));

    Registry.RegisterAction(TEXT("http_fixture"), TEXT("write"), TEXT("Test-only side effect counter"),
        FMonolithActionHandler::CreateLambda([&Executed](const TSharedPtr<FJsonObject>&)
        {
            ++Executed;
            return FMonolithActionResult::Success(MakeShared<FJsonObject>());
        }));
    Registry.RegisterAction(TEXT("http_fixture"), TEXT("fail"), TEXT("Test-only structured error"),
        FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
        {
            auto Data = MakeShared<FJsonObject>();
            Data->SetBoolField(TEXT("executed"), false);
            return FMonolithActionResult::Error(TEXT("busy"), FMonolithJsonUtils::ErrCoordinationBusy).WithErrorData(Data);
        }));

    for (bool bEmpty : {false, true})
    {
        const FString Action = bEmpty ? TEXT("empty_success") : TEXT("success");
        Registry.RegisterAction(TEXT("http_fixture"), Action, TEXT("Test-only structured success"),
            FMonolithActionHandler::CreateLambda([bEmpty](const TSharedPtr<FJsonObject>&)
            {
                if (bEmpty) return FMonolithActionResult::Success(nullptr);
                auto Data = MakeShared<FJsonObject>();
                Data->SetStringField(TEXT("answer"), TEXT("ready"));
                Data->SetNumberField(TEXT("count"), 42);
                return FMonolithActionResult::Success(Data);
            }));
        auto Call = FMonolithJsonUtils::Parse(TEXT("{\"name\":\"http_fixture_query\",\"arguments\":{\"action\":\"success\"}}"));
        Call->GetObjectField(TEXT("arguments"))->SetStringField(TEXT("action"), Action);
        const auto SuccessReply = Server.HandleToolsCall(MakeShared<FJsonValueNumber>(100), Call);
        const auto ToolResult = SuccessReply->GetObjectField(TEXT("result"));
        TestFalse(TEXT("Successful tool remains non-error"), ToolResult->GetBoolField(TEXT("isError")));
        const auto Evidence = ToolResult->GetObjectField(TEXT("_meta"))->GetObjectField(TEXT("monolith"));
        FGuid RequestGuid;
        TestTrue(TEXT("Headerless direct call receives a request UUID"), FGuid::Parse(Evidence->GetStringField(TEXT("request_id")), RequestGuid) && RequestGuid.IsValid());
        TestEqual(TEXT("Success identifies this module startup"), Evidence->GetStringField(TEXT("server_instance")), Instance);
        TestTrue(TEXT("Success reports public lease owner"), Evidence->HasTypedField<EJson::String>(TEXT("lease_owner")));
        const TSharedPtr<FJsonObject>* Structured = nullptr;
        if (TestTrue(TEXT("Success includes structuredContent object"), ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured)))
        {
            const auto Text = FMonolithJsonUtils::Parse(ToolResult->GetArrayField(TEXT("content"))[0]->AsObject()->GetStringField(TEXT("text")));
            TestTrue(TEXT("Success text remains JSON"), Text.IsValid());
            if (Text.IsValid()) TestEqual(TEXT("Structured and text results agree"), FMonolithJsonUtils::Serialize(*Structured), FMonolithJsonUtils::Serialize(Text));
            TestEqual(TEXT("Empty success is an empty object"), (*Structured)->Values.Num(), bEmpty ? 0 : 2);
            if (!bEmpty) TestEqual(TEXT("Success fields retained"), (*Structured)->GetStringField(TEXT("answer")), FString(TEXT("ready")));
        }
    }

    FString Body;
    int32 Status = 0;
    auto Complete = [&Body, &Status](TUniquePtr<FHttpServerResponse>&& Response)
    {
        Status = static_cast<int32>(Response->Code);
        auto Bytes = Response->Body;
        Bytes.Add(0);
        Body = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
    };
    auto Post = [&Server, &Complete](const FString& Json, const FString& Origin = FString(), bool bIncludeOrigin = false)
    {
        FHttpServerRequest Request;
        FTCHARToUTF8 Utf8(*Json);
        Request.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        if (bIncludeOrigin) Request.Headers.Add(TEXT("oRiGiN"), {Origin});
        Server.HandlePostMcp(Request, Complete);
    };
    // Exercise the actual HTTP header path without changing the mutation counter.
    const FString CorrelationId = TEXT("a9c5de1b-358d-4f32-a33e-8bd817210203");
    auto CorrelatedPost = [&](const FString& Json, const FString& RequestId, const FString& Client, bool bDuplicateId = false)
    {
        FHttpServerRequest Request;
        FTCHARToUTF8 Utf8(*Json);
        Request.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        Request.Headers.Add(TEXT("x-mOnOlItH-rEqUeSt-Id"), { RequestId });
        Request.Headers.Add(TEXT("X-Monolith-Client"), { Client });
        if (bDuplicateId) Request.Headers.Add(TEXT("X-Monolith-Request-Id"), { RequestId, RequestId });
        Server.HandlePostMcp(Request, Complete);
    };
    const FString SuccessCall = TEXT("{\"jsonrpc\":\"2.0\",\"id\":321,\"method\":\"tools/call\",\"params\":{\"name\":\"http_fixture_query\",\"arguments\":{\"action\":\"success\"}}}");
    struct FRequestLogCapture : FOutputDevice
    {
        FCriticalSection Mutex;
        TArray<FString> Lines;
        // Unbuffered delivery makes assertions independent of the logging thread's flush timing.
        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
        {
            if (Category == FName(TEXT("LogMonolith")) && Verbosity == ELogVerbosity::Log && FString(Message).Contains(TEXT("[req=")))
            {
                FScopeLock Lock(&Mutex);
                Lines.Add(Message);
            }
        }
        TArray<FString> Snapshot()
        {
            FScopeLock Lock(&Mutex);
            return Lines;
        }
    };
    {
        FRequestLogCapture Capture;
        GLog->AddOutputDevice(&Capture);
        ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&Capture); };
        CorrelatedPost(SuccessCall, CorrelationId, TEXT("fixture\r\n[forged]/123"));
        const TArray<FString> Lines = Capture.Snapshot();
        TestEqual(TEXT("One Log line per HTTP tools call"), Lines.Num(), 1);
        if (Lines.Num() == 1)
        {
            TestTrue(TEXT("Log retains exact correlation UUID"), Lines[0].Contains(TEXT("req=") + CorrelationId));
            TestTrue(TEXT("Log includes sanitized client"), Lines[0].Contains(TEXT("client=fixture___forged_/123")));
            TestTrue(TEXT("Log includes action and outcome"), Lines[0].Contains(TEXT("http_fixture.success ok=true ms=")));
            TestFalse(TEXT("Client cannot inject newline into log"), Lines[0].Contains(TEXT("\n")) || Lines[0].Contains(TEXT("\r")));
        }
    }
    auto HeaderEvidence = FMonolithJsonUtils::Parse(Body)->GetObjectField(TEXT("result"))->GetObjectField(TEXT("_meta"))->GetObjectField(TEXT("monolith"));
    TestEqual(TEXT("HTTP header UUID is echoed exactly"), HeaderEvidence->GetStringField(TEXT("request_id")), CorrelationId);
    TestEqual(TEXT("HTTP metadata startup identity"), HeaderEvidence->GetStringField(TEXT("server_instance")), Instance);
    TestEqual(TEXT("Status exposes the same startup identity"), FMonolithCoreTools::HandleStatus(MakeShared<FJsonObject>()).Result->GetStringField(TEXT("server_instance")), Instance);
    FHttpServerRequest HealthRequest;
    Server.HandleHealthCheck(HealthRequest, Complete);
    TestEqual(TEXT("Health exposes the same startup identity"), FMonolithJsonUtils::Parse(Body)->GetStringField(TEXT("server_instance")), Instance);
    for (const FString& BadId : { FString(TEXT("unsafe\r\nvalue")), FString::ChrN(129, 'x') })
    {
        CorrelatedPost(SuccessCall, BadId, TEXT("fixture/123"));
        const FString Replacement = FMonolithJsonUtils::Parse(Body)->GetObjectField(TEXT("result"))->GetObjectField(TEXT("_meta"))->GetObjectField(TEXT("monolith"))->GetStringField(TEXT("request_id"));
        FGuid Parsed;
        TestTrue(TEXT("Unsafe/overlong request ids become fresh UUIDs"), FGuid::Parse(Replacement, Parsed) && Parsed.IsValid());
    }
    CorrelatedPost(SuccessCall, CorrelationId, TEXT("fixture/123"), true);
    TestNotEqual(TEXT("Ambiguous duplicate request ids are replaced"), FMonolithJsonUtils::Parse(Body)->GetObjectField(TEXT("result"))->GetObjectField(TEXT("_meta"))->GetObjectField(TEXT("monolith"))->GetStringField(TEXT("request_id")), CorrelationId);
    for (const FString& InvalidCall : {
        FString(TEXT("{\"jsonrpc\":\"2.0\",\"id\":322,\"method\":\"tools/call\",\"params\":{}}")),
        FString(TEXT("{\"jsonrpc\":\"2.0\",\"id\":323,\"method\":\"tools/call\",\"params\":[]}")) })
    {
        FRequestLogCapture Capture;
        GLog->AddOutputDevice(&Capture);
        ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&Capture); };
        CorrelatedPost(InvalidCall, CorrelationId, TEXT("fixture/123"));
        const auto Data = FMonolithJsonUtils::Parse(Body)->GetObjectField(TEXT("error"))->GetObjectField(TEXT("data"));
        TestEqual(TEXT("Protocol tool failure retains request correlation"), Data->GetStringField(TEXT("request_id")), CorrelationId);
        TestEqual(TEXT("Protocol tool failure retains startup identity"), Data->GetStringField(TEXT("server_instance")), Instance);
        TestEqual(TEXT("Protocol rejection writes exactly one call log"), Capture.Snapshot().Num(), 1);
    }

    CorrelatedPost(SuccessCall.Replace(TEXT("success"), TEXT("fail")), CorrelationId, TEXT("fixture/123"));
    const auto CorrelatedFailure = FMonolithJsonUtils::Parse(Body)->GetObjectField(TEXT("result"));
    const auto FailureData = CorrelatedFailure->GetObjectField(TEXT("structuredContent"))->GetObjectField(TEXT("data"));
    TestEqual(TEXT("HTTP action failure retains request UUID"), FailureData->GetStringField(TEXT("request_id")), CorrelationId);
    TestEqual(TEXT("HTTP action failure retains startup identity"), FailureData->GetStringField(TEXT("server_instance")), Instance);
    TestFalse(TEXT("HTTP correlation preserves execution evidence"), FailureData->GetBoolField(TEXT("executed")));

    const FString Write = TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"http_fixture_query\",\"arguments\":{\"action\":\"write\"}}}");

    auto CheckRejection = [this, &Body](const TCHAR* Context)
    {
        const auto Reply = FMonolithJsonUtils::Parse(Body);
        if (!TestTrue(Context, Reply.IsValid())) return;
        TestEqual(TEXT("Rejection is JSON-RPC"), Reply->GetStringField(TEXT("jsonrpc")), FString(TEXT("2.0")));
        TestTrue(TEXT("Pre-parse rejection has null id"), Reply->HasTypedField<EJson::Null>(TEXT("id")));
        const TSharedPtr<FJsonObject>* Error = nullptr;
        if (!TestTrue(TEXT("Rejection error object"), Reply->TryGetObjectField(TEXT("error"), Error))) return;
        TestEqual(TEXT("Rejection is invalid request"), (*Error)->GetIntegerField(TEXT("code")), FMonolithJsonUtils::ErrInvalidRequest);
        const TSharedPtr<FJsonObject>* Data = nullptr;
        if (!TestTrue(TEXT("Rejection includes data"), (*Error)->TryGetObjectField(TEXT("data"), Data))) return;
        TestTrue(TEXT("Execution evidence is boolean"), (*Data)->HasTypedField<EJson::Boolean>(TEXT("executed")));
        TestFalse(TEXT("Rejected request never executed"), (*Data)->GetBoolField(TEXT("executed")));
        TestEqual(TEXT("Protocol rejection has a structured class"), (*Data)->GetStringField(TEXT("class")), FString(TEXT("invalid_param")));
    };

    Post(Write, TEXT("https://untrusted.example"), true);
    TestEqual(TEXT("Foreign origin rejected before execution"), Status, 403);
    TestEqual(TEXT("Foreign origin cannot mutate editor"), Executed, 0);
    CheckRejection(TEXT("Origin rejection parses"));

    FHttpServerRequest UnsupportedVersion;
    FTCHARToUTF8 VersionBody(*Write);
    UnsupportedVersion.Body.Append(reinterpret_cast<const uint8*>(VersionBody.Get()), VersionBody.Length());
    UnsupportedVersion.Headers.Add(TEXT("MCP-Protocol-Version"), {TEXT("unsupported")});
    Server.HandlePostMcp(UnsupportedVersion, Complete);
    TestEqual(TEXT("Unsupported version rejected"), Status, 400);
    CheckRejection(TEXT("Version rejection parses"));
    TestEqual(TEXT("Unsupported version never executes"), Executed, 0);

    // Keep the fixture small regardless of the user's configured body limit.
    UMonolithSettings* MutableSettings = GetMutableDefault<UMonolithSettings>();
    const int32 OriginalBodyLimit = MutableSettings->MaxRequestBodyMB;
    MutableSettings->MaxRequestBodyMB = 1;
    FHttpServerRequest Oversized;
    Oversized.Body.Init(' ', 1024 * 1024 + 1);
    Oversized.Body[0] = 0; // Size rejection must win before the NUL scan.
    Server.HandlePostMcp(Oversized, Complete);
    MutableSettings->MaxRequestBodyMB = OriginalBodyLimit;
    TestEqual(TEXT("Oversized body rejected"), Status, 413);
    CheckRejection(TEXT("Size rejection parses"));
    TestEqual(TEXT("Oversized body never executes"), Executed, 0);
    Post(Write, TEXT(""), true);
    TestEqual(TEXT("Empty explicit Origin rejected"), Status, 403);
    Post(Write, TEXT("http://localhost.evil.example"), true);
    TestEqual(TEXT("Origin suffix attack rejected"), Status, 403);
    Post(Write, TEXT("http://localhost:1234"), true);
    TestEqual(TEXT("Allowed origin executes"), Executed, 1);
    TestEqual(TEXT("Normal response"), Status, 200);

    FHttpServerRequest EmbeddedZero;
    FTCHARToUTF8 WriteUtf8(*Write);
    EmbeddedZero.Body.Append(reinterpret_cast<const uint8*>(WriteUtf8.Get()), WriteUtf8.Length());
    EmbeddedZero.Body.Add(0);
    EmbeddedZero.Body.Add('x');
    Server.HandlePostMcp(EmbeddedZero, Complete);
    TestEqual(TEXT("Embedded NUL rejected"), Status, 400);
    TestEqual(TEXT("Valid prefix before NUL never executes"), Executed, 1);

    Post(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"http_fixture_query\",\"arguments\":{\"action\":\"write\"}}}"));
    TestEqual(TEXT("Notification acknowledged"), Status, 202);
    TestTrue(TEXT("Notification body empty"), Body.IsEmpty());
    TestEqual(TEXT("Notification does not execute tool"), Executed, 1);

    Post(TEXT("[") + Write + TEXT("]"));
    TestTrue(TEXT("Single-reply legacy batch stays an array"), Body.StartsWith(TEXT("[")));
    Post(TEXT("[") + Write + TEXT(",42]"));
    TestTrue(TEXT("Invalid batch entry returns error"), Body.Contains(TEXT("-32600")));
    Post(TEXT("[]"));
    TestTrue(TEXT("Empty batch is invalid request, not malformed JSON"), Body.Contains(TEXT("-32600")));
    Post(TEXT("42"));
    TestTrue(TEXT("Scalar JSON is invalid request"), Body.Contains(TEXT("-32600")));
    Post(TEXT("{"));
    TestTrue(TEXT("Malformed JSON is parse error"), Body.Contains(TEXT("-32700")));
    const int32 Before = Executed;
    Post(TEXT("{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"tools/call\"}"));
    TestTrue(TEXT("Object id rejected"), Body.Contains(TEXT("-32600")));
    Post(TEXT("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":[]}"));
    TestTrue(TEXT("Array params rejected"), Body.Contains(TEXT("-32602")));
    TestEqual(TEXT("Invalid requests never mutate"), Executed, Before);

    Post(TEXT("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"http_fixture_query\",\"arguments\":{\"action\":\"fail\"}}}"));
    const auto Reply = FMonolithJsonUtils::Parse(Body);
    if (TestTrue(TEXT("Error result is valid JSON"), Reply.IsValid()))
    {
        const auto Result = Reply->GetObjectField(TEXT("result"));
        TestTrue(TEXT("Tool error marked"), Result->GetBoolField(TEXT("isError")));
        const auto Structured = Result->GetObjectField(TEXT("structuredContent"));
        TestEqual(TEXT("Error code retained"), Structured->GetIntegerField(TEXT("code")), FMonolithJsonUtils::ErrCoordinationBusy);
        TestFalse(TEXT("Retry evidence retained"), Structured->GetObjectField(TEXT("data"))->GetBoolField(TEXT("executed")));
        TestEqual(TEXT("Coordination class retained in HTTP result"), Structured->GetObjectField(TEXT("data"))->GetStringField(TEXT("class")), FString(TEXT("lease_busy")));
        const auto Text = FMonolithJsonUtils::Parse(Result->GetArrayField(TEXT("content"))[0]->AsObject()->GetStringField(TEXT("text")));
        if (TestTrue(TEXT("Error text remains JSON"), Text.IsValid()))
            TestEqual(TEXT("Error structured and text payloads agree"), FMonolithJsonUtils::Serialize(Structured), FMonolithJsonUtils::Serialize(Text));
    }

    // HTTP must classify legacy error payloads without rewriting their execution
    // evidence or losing scalar/array data. It must not mutate handler-owned data.
    TArray<TPair<FString, TSharedPtr<FJsonValue>>> ErrorCases;
    auto ExecutedData = MakeShared<FJsonObject>();
    ExecutedData->SetBoolField(TEXT("executed"), true);
    ExecutedData->SetBoolField(TEXT("partial"), true);
    ErrorCases.Emplace(TEXT("executed"), MakeShared<FJsonValueObject>(ExecutedData));
    auto UnknownData = MakeShared<FJsonObject>();
    UnknownData->SetStringField(TEXT("executed"), TEXT("unknown"));
    UnknownData->SetStringField(TEXT("class"), TEXT("unknown_outcome"));
    ErrorCases.Emplace(TEXT("unknown"), MakeShared<FJsonValueObject>(UnknownData));
    ErrorCases.Emplace(TEXT("scalar"), MakeShared<FJsonValueNumber>(42));
    ErrorCases.Emplace(TEXT("array"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(TEXT("retained")) }));
    ErrorCases.Emplace(TEXT("null"), MakeShared<FJsonValueNull>());
    ErrorCases.Emplace(TEXT("absent"), TSharedPtr<FJsonValue>());
    for (const auto& Case : ErrorCases)
    {
        const FString Action = TEXT("error_") + Case.Key;
        const TSharedPtr<FJsonValue> OriginalData = Case.Value;
        Registry.RegisterAction(TEXT("http_fixture"), Action, TEXT("Test-only legacy error shape"),
            FMonolithActionHandler::CreateLambda([OriginalData](const TSharedPtr<FJsonObject>&)
            {
                FMonolithActionResult Result = FMonolithActionResult::Error(TEXT("fixture error"), FMonolithJsonUtils::ErrInternalError);
                Result.ErrorData = OriginalData;
                return Result;
            }), FParamSchemaBuilder().Build());
        auto Call = FMonolithJsonUtils::Parse(TEXT("{\"name\":\"http_fixture_query\",\"arguments\":{}}"));
        Call->GetObjectField(TEXT("arguments"))->SetStringField(TEXT("action"), Action);
        const auto ToolReply = Server.HandleToolsCall(MakeShared<FJsonValueNumber>(200), Call);
        const auto ToolResult = ToolReply->GetObjectField(TEXT("result"));
        TestTrue(TEXT("Legacy failure remains a tool error"), ToolResult->GetBoolField(TEXT("isError")));
        const auto Structured = ToolResult->GetObjectField(TEXT("structuredContent"));
        const auto Data = Structured->GetObjectField(TEXT("data"));
        TestEqual(TEXT("Legacy error carries startup correlation"), Data->GetStringField(TEXT("server_instance")), Instance);
        FGuid ErrorRequestGuid;
        TestTrue(TEXT("Legacy error carries request correlation"), FGuid::Parse(Data->GetStringField(TEXT("request_id")), ErrorRequestGuid));
        if (OriginalData.IsValid() && OriginalData->Type == EJson::Object)
            TestFalse(TEXT("Correlation never changes handler-owned data"), OriginalData->AsObject()->HasField(TEXT("request_id")));
        const auto Text = FMonolithJsonUtils::Parse(ToolResult->GetArrayField(TEXT("content"))[0]->AsObject()->GetStringField(TEXT("text")));
        if (TestTrue(TEXT("Legacy error text is JSON"), Text.IsValid()))
            TestEqual(TEXT("Legacy error text matches structured content"), FMonolithJsonUtils::Serialize(Text), FMonolithJsonUtils::Serialize(Structured));
        TestEqual(TEXT("Class is supplied or preserved"), Data->GetStringField(TEXT("class")),
            Case.Key == TEXT("unknown") ? FString(TEXT("unknown_outcome")) : FString(TEXT("engine_error")));
        if (Case.Key == TEXT("executed"))
        {
            TestTrue(TEXT("Executed mutation stays true"), Data->GetBoolField(TEXT("executed")));
            TestTrue(TEXT("Partial result detail retained"), Data->GetBoolField(TEXT("partial")));
            TestFalse(TEXT("Normalization does not add class to caller object"), ExecutedData->HasField(TEXT("class")));
        }
        else if (Case.Key == TEXT("unknown"))
        {
            TestEqual(TEXT("Unknown execution stays unknown"), Data->GetStringField(TEXT("executed")), FString(TEXT("unknown")));
        }
        else
        {
            TestFalse(TEXT("No execution evidence invented"), Data->HasField(TEXT("executed")));
            TestFalse(TEXT("No retry evidence invented"), Data->HasField(TEXT("retryable")));
            if (Case.Key == TEXT("scalar")) TestEqual(TEXT("Scalar payload retained under value"), Data->GetIntegerField(TEXT("value")), 42);
            else if (Case.Key == TEXT("array")) TestEqual(TEXT("Array payload retained under value"), Data->GetArrayField(TEXT("value"))[0]->AsString(), FString(TEXT("retained")));
            else if (Case.Key == TEXT("null")) TestTrue(TEXT("Explicit null value retained"), Data->HasTypedField<EJson::Null>(TEXT("value")));
            else TestFalse(TEXT("Absent payload has no invented value"), Data->HasField(TEXT("value")));
        }

        const auto ProtocolReply = FMonolithJsonUtils::ErrorResponse(MakeShared<FJsonValueNumber>(201),
            FMonolithJsonUtils::ErrInvalidParams, TEXT("invalid envelope"), OriginalData);
        const auto ProtocolData = ProtocolReply->GetObjectField(TEXT("error"))->GetObjectField(TEXT("data"));
        TestEqual(TEXT("Protocol error class supplied or preserved"), ProtocolData->GetStringField(TEXT("class")),
            Case.Key == TEXT("unknown") ? FString(TEXT("unknown_outcome")) : FString(TEXT("invalid_param")));
        if (Case.Key == TEXT("executed")) TestTrue(TEXT("Protocol execution evidence retained"), ProtocolData->GetBoolField(TEXT("executed")));
        else if (Case.Key == TEXT("unknown")) TestEqual(TEXT("Protocol unknown evidence retained"), ProtocolData->GetStringField(TEXT("executed")), FString(TEXT("unknown")));
        else TestFalse(TEXT("Protocol normalization invents no execution evidence"), ProtocolData->HasField(TEXT("executed")));
    }

    auto& Coordinator = FMonolithCoordination::Get();
    auto LeaseRequest = MakeShared<FJsonObject>();
    LeaseRequest->SetStringField(TEXT("operation"), TEXT("acquire"));
    LeaseRequest->SetStringField(TEXT("owner"), TEXT("http-batch-automation"));
    LeaseRequest->SetNumberField(TEXT("ttl_seconds"), 120);
    const auto Acquired = Coordinator.Handle(LeaseRequest);
    if (TestTrue(TEXT("Batch fixture acquires lease"), Acquired.bSuccess))
    {
        const FString Token = Acquired.Result->GetStringField(TEXT("_lease_token"));
        const auto StatusCall = FMonolithJsonUtils::Parse(TEXT("{\"name\":\"monolith_status\",\"arguments\":{}}"));
        const auto StatusReply = Server.HandleToolsCall(MakeShared<FJsonValueNumber>(324), StatusCall, { CorrelationId, TEXT("different-client/456") });
        TestEqual(TEXT("Success metadata reports coordinator owner, not client identity"),
            StatusReply->GetObjectField(TEXT("result"))->GetObjectField(TEXT("_meta"))->GetObjectField(TEXT("monolith"))->GetStringField(TEXT("lease_owner")), FString(TEXT("http-batch-automation")));
        auto MakeOwnedWrite = [&Write](const FString& ItemToken, bool bStringParams)
        {
            auto Item = FMonolithJsonUtils::Parse(Write);
            auto Arguments = Item->GetObjectField(TEXT("params"))->GetObjectField(TEXT("arguments"));
            auto Nested = MakeShared<FJsonObject>();
            Nested->SetStringField(TEXT("_lease_token"), ItemToken);
            if (bStringParams) Arguments->SetStringField(TEXT("params"), FMonolithJsonUtils::Serialize(Nested));
            else Arguments->SetObjectField(TEXT("params"), Nested);
            return FMonolithJsonUtils::Serialize(Item);
        };
        const FString OwnedWrite = MakeOwnedWrite(Token, false);
        const FString StringOwnedWrite = MakeOwnedWrite(Token, true);
        const int32 BeforeBatch = Executed;
        Post(TEXT("[42,") + OwnedWrite + TEXT("]"));
        TestEqual(TEXT("Malformed item does not prevent later owned execution"), Executed, BeforeBatch + 1);
        TestTrue(TEXT("Malformed batch item retains invalid-request response"), Body.Contains(TEXT("-32600")));
        Post(TEXT("[") + OwnedWrite + TEXT(",") + MakeOwnedWrite(TEXT("wrong"), false) + TEXT(",") + StringOwnedWrite + TEXT("]"));
        TestEqual(TEXT("Only valid mixed-token items execute"), Executed, BeforeBatch + 3);
        TestTrue(TEXT("Mixed-token batch retains invalid-lease error"), Body.Contains(FString::FromInt(FMonolithJsonUtils::ErrInvalidLease)));
        Post(TEXT("[") + OwnedWrite + TEXT(",") + Write + TEXT("]"));
        TestEqual(TEXT("Unowned item never inherits batch token"), Executed, BeforeBatch + 4);
        TestTrue(TEXT("Unowned item retains busy error"), Body.Contains(FString::FromInt(FMonolithJsonUtils::ErrCoordinationBusy)));
        LeaseRequest->SetStringField(TEXT("operation"), TEXT("release"));
        LeaseRequest->SetStringField(TEXT("_lease_token"), Token);
        TestTrue(TEXT("Release succeeds after batch response"), Coordinator.Handle(LeaseRequest).bSuccess);
    }

    FHttpServerRequest Get;
    Server.HandleGetMcp(Get, Complete);
    TestEqual(TEXT("No fake SSE stream"), Status, 405);
    Server.HandleDeleteMcp(Get, Complete);
    TestEqual(TEXT("Stateless DELETE"), Status, 405);
    Registry.UnregisterNamespace(TEXT("http_fixture"));
    return true;
}
#endif
