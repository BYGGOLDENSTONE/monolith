#include "Misc/AutomationTest.h"
#include "MonolithHttpServer.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithHttpProtocolTest,
    "Monolith.Core.HttpProtocol", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHttpProtocolTest::RunTest(const FString& Parameters)
{
    FMonolithHttpServer Server;
    int32 Executed = 0;
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
            return FMonolithActionResult::Error(TEXT("busy"), -32010).WithErrorData(Data);
        }));

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
        TestEqual(TEXT("Error code retained"), Structured->GetIntegerField(TEXT("code")), -32010);
        TestFalse(TEXT("Retry evidence retained"), Structured->GetObjectField(TEXT("data"))->GetBoolField(TEXT("executed")));
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
