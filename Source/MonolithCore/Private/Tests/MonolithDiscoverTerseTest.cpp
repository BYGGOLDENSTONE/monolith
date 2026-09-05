// SPDX-License-Identifier: MIT
// Automation tests for terse monolith_discover(namespace).
// Plan: Plugins/Monolith/Docs/plans/2026-06-19-terse-discover.md (Field 10)
//
// Goals:
//   - Per-namespace discover is terse by default: action + description, NO `params`.
//   - `detail=true` (canonical) inlines the full per-action param schema.
//   - `verbose=true` is an accepted alias for `detail=true`.
//   - `filter` substring-matches name OR description (case-insensitive).
//   - Default (no limit) returns the FULL action list (discoverability gate).
//   - Pagination (offset/limit) is opt-in; limit=0 = ALL; out-of-range clamps.
//   - Unknown namespace still errors.
//
// Lives under Private/Tests/ for the same UBT auto-include reason as the other
// MonolithCore tests in this folder.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithDiscoverTerseTestDetail
{
	/** Run monolith.discover with the given params. */
	static FMonolithActionResult Discover(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), Params);
	}

	/** Build a map of action name -> emitted description from a discover result's actions array. */
	static TMap<FString, FString> DescriptionsByAction(const FMonolithActionResult& R)
	{
		TMap<FString, FString> Out;
		if (!R.bSuccess || !R.Result.IsValid())
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!R.Result->TryGetArrayField(TEXT("actions"), Arr) || !Arr)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj)
			{
				FString Name, Desc;
				(*Obj)->TryGetStringField(TEXT("action"), Name);
				(*Obj)->TryGetStringField(TEXT("description"), Desc);
				Out.Add(Name, Desc);
			}
		}
		return Out;
	}

	/** Full (unfiltered) action count for the monolith namespace — the discoverability baseline. */
	static int32 FullActionCount()
	{
		return FMonolithToolRegistry::Get().GetActions(TEXT("monolith")).Num();
	}

	/** Pull the `actions` array out of a successful discover result, or null. */
	static const TArray<TSharedPtr<FJsonValue>>* GetActionsArray(const FMonolithActionResult& R)
	{
		if (!R.bSuccess || !R.Result.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		R.Result->TryGetArrayField(TEXT("actions"), Arr);
		return Arr;
	}
}

// ---------------------------------------------------------------------------
// Test 1: Terse default — each action obj has action+description, NO `params`;
// top-level `schema_hint` present.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverTerseDefaultTest,
	"Monolith.Discover.Terse.Default",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverTerseDefaultTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("terse discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		TestTrue(TEXT("at least one action returned"), Arr->Num() > 0);
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj)
			{
				TestTrue(TEXT("action obj has 'action'"), (*Obj)->HasField(TEXT("action")));
				TestTrue(TEXT("action obj has 'description'"), (*Obj)->HasField(TEXT("description")));
				TestFalse(TEXT("terse action obj has NO 'params'"), (*Obj)->HasField(TEXT("params")));
			}
		}
	}

	if (R.bSuccess && R.Result.IsValid())
	{
		TestTrue(TEXT("terse result has top-level schema_hint"), R.Result->HasField(TEXT("schema_hint")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Detail opt-in — each action obj HAS `params`.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverDetailOptInTest,
	"Monolith.Discover.Terse.DetailOptIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverDetailOptInTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Params->SetBoolField(TEXT("detail"), true);

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("detail discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		// In detail mode every monolith action that carries a ParamSchema emits
		// `params`. At least one monolith action (discover/update) has a schema, so
		// assert at least one action obj carries params.
		bool bAnyHasParams = false;
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj && (*Obj)->HasField(TEXT("params")))
			{
				bAnyHasParams = true;
				break;
			}
		}
		TestTrue(TEXT("detail mode inlines params on at least one action"), bAnyHasParams);
	}

	if (R.bSuccess && R.Result.IsValid())
	{
		TestFalse(TEXT("detail result has NO schema_hint"), R.Result->HasField(TEXT("schema_hint")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: verbose alias — output is byte-IDENTICAL to detail:true (verbose is a
// registered param, so no spurious unknown-param warning is emitted).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverVerboseAliasTest,
	"Monolith.Discover.Terse.VerboseAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverVerboseAliasTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> VerboseParams = MakeShared<FJsonObject>();
	VerboseParams->SetStringField(TEXT("namespace"), TEXT("monolith"));
	VerboseParams->SetBoolField(TEXT("verbose"), true);
	const FMonolithActionResult VerboseR = Discover(VerboseParams);
	TestTrue(TEXT("verbose discover succeeds"), VerboseR.bSuccess);

	TSharedPtr<FJsonObject> DetailParams = MakeShared<FJsonObject>();
	DetailParams->SetStringField(TEXT("namespace"), TEXT("monolith"));
	DetailParams->SetBoolField(TEXT("detail"), true);
	const FMonolithActionResult DetailR = Discover(DetailParams);
	TestTrue(TEXT("detail discover succeeds"), DetailR.bSuccess);

	if (VerboseR.bSuccess && VerboseR.Result.IsValid())
	{
		// verbose must be a recognized param — no spurious unknown-param warning.
		TestFalse(TEXT("verbose result carries no warnings"), VerboseR.Result->HasField(TEXT("warnings")));
	}

	if (VerboseR.Result.IsValid() && DetailR.Result.IsValid())
	{
		const FString VerboseStr = FMonolithJsonUtils::Serialize(VerboseR.Result);
		const FString DetailStr = FMonolithJsonUtils::Serialize(DetailR.Result);
		TestEqual(TEXT("verbose output is byte-identical to detail output"), VerboseStr, DetailStr);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: filter matches full registered descriptions (even when the matching
// text is omitted from the terse preview), case-insensitively and without loss.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverFilterTest,
	"Monolith.Discover.Terse.Filter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverFilterTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;
	const FString Filter = TEXT("StAtUs");
	TSet<FString> ExpectedActions;
	for (const FMonolithActionInfo& Info : FMonolithToolRegistry::Get().GetActions(TEXT("monolith")))
	{
		if (Info.Action.Contains(Filter, ESearchCase::IgnoreCase)
			|| Info.Description.Contains(Filter, ESearchCase::IgnoreCase))
		{
			ExpectedActions.Add(Info.Action);
		}
	}
	TestTrue(TEXT("Full registry contains matching actions"), ExpectedActions.Num() > 0);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Params->SetStringField(TEXT("filter"), Filter);

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("filtered discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		TSet<FString> ActualActions;
		// Match against source metadata, not the deliberately truncated preview.
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj)
			{
				FString Name;
				(*Obj)->TryGetStringField(TEXT("action"), Name);
				TestTrue(TEXT("Action name or full description matches filter"), ExpectedActions.Contains(Name));
				TestFalse(TEXT("Filtered action is not duplicated"), ActualActions.Contains(Name));
				ActualActions.Add(Name);
			}
			else { AddError(TEXT("Filtered action must be a JSON object")); }
		}
		TestEqual(TEXT("All full-description matches returned"), ActualActions.Num(), ExpectedActions.Num());
		TestTrue(TEXT("No matching action omitted"), ActualActions.Includes(ExpectedActions));

		// `total` equals the filtered (returned) count when no pagination is applied.
		if (R.Result.IsValid())
		{
			int32 Total = -1;
			R.Result->TryGetNumberField(TEXT("total"), Total);
			TestEqual(TEXT("total equals filtered count"), Total, Arr->Num());
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: default returns FULL list — no limit => array length == total ==
// full action count; NO next_offset (nothing truncated).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverFullListTest,
	"Monolith.Discover.Terse.FullList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverFullListTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const int32 Full = FullActionCount();
	TestTrue(TEXT("namespace has at least one action"), Full > 0);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("default discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		TestEqual(TEXT("default array length == full action count"), Arr->Num(), Full);
	}
	if (R.bSuccess && R.Result.IsValid())
	{
		int32 Total = -1;
		R.Result->TryGetNumberField(TEXT("total"), Total);
		TestEqual(TEXT("total == full action count"), Total, Full);
		TestFalse(TEXT("no next_offset when nothing truncated"), R.Result->HasField(TEXT("next_offset")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: opt-in pagination — offset:1,limit:1 => exactly 1 action, total=full
// count, next_offset present when more remain; limit:0 returns ALL; out-of-range
// offset/limit clamp without error.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverPaginationTest,
	"Monolith.Discover.Terse.Pagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverPaginationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const int32 Full = FullActionCount();
	TestTrue(TEXT("namespace has multiple actions for pagination"), Full >= 2);

	// offset:1, limit:1 => exactly one action returned.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetNumberField(TEXT("offset"), 1);
		Params->SetNumberField(TEXT("limit"), 1);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("paginated discover succeeds"), R.bSuccess);

		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("limit:1 returns exactly 1 action"), Arr->Num(), 1);
		}
		if (R.Result.IsValid())
		{
			int32 Total = -1;
			R.Result->TryGetNumberField(TEXT("total"), Total);
			TestEqual(TEXT("total still reports full count under pagination"), Total, Full);

			// More remain after offset 1 + limit 1 when Full > 2 ; equals when Full == 2.
			const bool bMoreRemain = (1 + 1) < Full;
			if (bMoreRemain)
			{
				TestTrue(TEXT("next_offset present when more remain"), R.Result->HasField(TEXT("next_offset")));
				int32 NextOffset = -1;
				R.Result->TryGetNumberField(TEXT("next_offset"), NextOffset);
				TestEqual(TEXT("next_offset == offset + limit"), NextOffset, 2);
			}
			else
			{
				TestFalse(TEXT("no next_offset when slice reaches end"), R.Result->HasField(TEXT("next_offset")));
			}
		}
	}

	// limit:0 returns ALL (explicit no-cap sentinel).
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetNumberField(TEXT("limit"), 0);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("limit:0 discover succeeds"), R.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("limit:0 returns ALL actions"), Arr->Num(), Full);
		}
		if (R.Result.IsValid())
		{
			TestFalse(TEXT("limit:0 emits no next_offset"), R.Result->HasField(TEXT("next_offset")));
		}
	}

	// Out-of-range offset/limit clamp without error.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetNumberField(TEXT("offset"), Full + 100);
		Params->SetNumberField(TEXT("limit"), 50);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("out-of-range pagination still succeeds (clamped)"), R.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("offset past end yields empty slice"), Arr->Num(), 0);
		}
		if (R.Result.IsValid())
		{
			int32 Total = -1;
			R.Result->TryGetNumberField(TEXT("total"), Total);
			TestEqual(TEXT("total unaffected by clamped offset"), Total, Full);
			TestFalse(TEXT("no next_offset when offset clamped past end"), R.Result->HasField(TEXT("next_offset")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 8: terse description trim invariant (blueprint namespace).
// For every action: (a) terse description length <= HardCap + 3 (the "..."),
// and (b) the terse description with any trailing "..." removed is a prefix of
// the full (detail) description. AND at least one action whose full description
// exceeds HardCap must have a terse description ending in "..." (proves trimming
// actually fires). HardCap mirrors MonolithTerseOneLineDescription (150).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverTerseTrimInvariantTest,
	"Monolith.Discover.Terse.TrimInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverTerseTrimInvariantTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	// Mirror the helper's HardCap; terse descriptions never exceed HardCap + len("...").
	const int32 HardCap = 150;
	const FString Ellipsis = TEXT("...");

	// blueprint is registered in-editor and has long, multi-paragraph descriptions.
	TSharedPtr<FJsonObject> TerseParams = MakeShared<FJsonObject>();
	TerseParams->SetStringField(TEXT("namespace"), TEXT("blueprint"));
	const FMonolithActionResult TerseR =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), TerseParams);

	TSharedPtr<FJsonObject> DetailParams = MakeShared<FJsonObject>();
	DetailParams->SetStringField(TEXT("namespace"), TEXT("blueprint"));
	DetailParams->SetBoolField(TEXT("detail"), true);
	const FMonolithActionResult DetailR =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), DetailParams);

	if (!TerseR.bSuccess || !DetailR.bSuccess)
	{
		// blueprint namespace not registered (e.g. headless) — skip rather than fail.
		AddInfo(TEXT("blueprint namespace unavailable; skipping trim-invariant test"));
		return true;
	}

	const TMap<FString, FString> Terse = DescriptionsByAction(TerseR);
	const TMap<FString, FString> Full = DescriptionsByAction(DetailR);
	TestTrue(TEXT("blueprint returned at least one action"), Terse.Num() > 0);

	bool bAnyTrimmed = false;
	for (const TPair<FString, FString>& Pair : Terse)
	{
		const FString& TerseDesc = Pair.Value;
		const FString* FullDescPtr = Full.Find(Pair.Key);
		TestNotNull(TEXT("detail description exists for action"), FullDescPtr);
		if (!FullDescPtr)
		{
			continue;
		}
		const FString& FullDesc = *FullDescPtr;

		// (a) terse length bounded by HardCap + len("...").
		TestTrue(TEXT("terse description length <= HardCap + 3"),
			TerseDesc.Len() <= HardCap + Ellipsis.Len());

		// (b) terse-minus-ellipsis is a prefix of the full description.
		FString Core = TerseDesc;
		const bool bEndsWithEllipsis = Core.EndsWith(Ellipsis, ESearchCase::CaseSensitive);
		if (bEndsWithEllipsis)
		{
			Core.LeftChopInline(Ellipsis.Len());
		}
		// Trimming right-trims trailing whitespace before appending "..."; a prefix
		// match after TrimEnd on the full description tolerates that boundary.
		TestTrue(TEXT("terse core is a prefix of the full description"),
			FullDesc.StartsWith(Core, ESearchCase::CaseSensitive));

		if (bEndsWithEllipsis && FullDesc.Len() > HardCap)
		{
			bAnyTrimmed = true;
		}
	}

	TestTrue(TEXT("at least one long description was trimmed with '...'"), bAnyTrimmed);
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: unknown namespace — error path still fires.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverUnknownNamespaceTest,
	"Monolith.Discover.Terse.UnknownNamespace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverUnknownNamespaceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("definitely_not_a_real_namespace_xyz"));

	const FMonolithActionResult R = Discover(Params);
	TestFalse(TEXT("unknown namespace must error"), R.bSuccess);
	if (!R.bSuccess)
	{
		TestTrue(TEXT("error mentions the unknown namespace"),
			R.ErrorMessage.Contains(TEXT("Unknown namespace")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 9: pagination overflow. offset=1 with limit=INT32_MAX used to form
// SliceStart + Limit as int32 BEFORE clamping, which signed-overflows. On MSVC
// it wrapped negative, so Clamp returned SliceStart -> an EMPTY actions array
// and a NEGATIVE next_offset, from a well-formed call (TryGetNumberField
// range-checks against TNumericLimits<int32>, so 2147483647 arrives verbatim).
// A huge limit must behave exactly like "no cap": every remaining action, and
// no next_offset. Asserting the negative value is the point -- a test that only
// checked "succeeds" would have passed against the bug.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverPaginationOverflowTest,
	"Monolith.Discover.Terse.PaginationOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverPaginationOverflowTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const int32 Full = FullActionCount();
	TestTrue(TEXT("namespace has multiple actions"), Full >= 2);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Params->SetNumberField(TEXT("offset"), 1);
	Params->SetNumberField(TEXT("limit"), MAX_int32);

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("INT32_MAX limit succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		// Pre-fix this was 0 (the wrapped-negative clamp collapsed the slice).
		TestEqual(TEXT("INT32_MAX limit returns all actions from offset 1"), Arr->Num(), Full - 1);
	}
	if (R.Result.IsValid())
	{
		// Pre-fix this field was present and negative.
		TestFalse(TEXT("no next_offset when the slice reaches the end"), R.Result->HasField(TEXT("next_offset")));

		int32 NextOffset = 0;
		if (R.Result->TryGetNumberField(TEXT("next_offset"), NextOffset))
		{
			TestTrue(TEXT("next_offset is never negative"), NextOffset >= 0);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 10: cross-namespace search. A filter with NO namespace used to be
// silently ignored -- the caller got the namespace inventory back and no error.
// It must now search every namespace, tag each row with its owning namespace,
// report matched_namespaces pre-pagination, and cap at 50 when limit is absent
// while still honouring an explicit limit=0 as ALL.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverCrossNamespaceTest,
	"Monolith.Discover.Terse.CrossNamespace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverCrossNamespaceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	// "get_" appears in many actions across many namespaces.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("filter"), TEXT("get_"));

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("cross-namespace discover succeeds"), R.bSuccess);
	if (!R.Result.IsValid())
	{
		return false;
	}

	// It must NOT have fallen through to the namespace inventory.
	TestFalse(TEXT("did not return the namespace inventory"), R.Result->HasField(TEXT("namespaces")));

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);

	const TArray<TSharedPtr<FJsonValue>>* NsArr = nullptr;
	TestTrue(TEXT("matched_namespaces present"), R.Result->TryGetArrayField(TEXT("matched_namespaces"), NsArr));
	if (NsArr)
	{
		TestTrue(TEXT("matched more than one namespace"), NsArr->Num() >= 2);
	}

	int32 Total = 0;
	R.Result->TryGetNumberField(TEXT("total"), Total);
	TestTrue(TEXT("total is positive"), Total > 0);

	if (Arr)
	{
		// Absent limit caps at 50; total is reported pre-pagination.
		TestTrue(TEXT("absent limit caps rows at 50"), Arr->Num() <= 50);
		if (Total > 50)
		{
			TestEqual(TEXT("capped at exactly 50 when more remain"), Arr->Num(), 50);
			TestTrue(TEXT("next_offset present when capped"), R.Result->HasField(TEXT("next_offset")));
		}

		// Every row carries its owning namespace -- that is what makes a flat
		// cross-namespace result actionable.
		bool bAllTagged = true;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!V->TryGetObject(Obj) || !(*Obj)->HasField(TEXT("namespace")))
			{
				bAllTagged = false;
				break;
			}
		}
		TestTrue(TEXT("every cross-namespace row is namespace-tagged"), bAllTagged);
	}

	// Explicit limit=0 still means ALL, matching the per-namespace contract.
	{
		TSharedPtr<FJsonObject> AllParams = MakeShared<FJsonObject>();
		AllParams->SetStringField(TEXT("filter"), TEXT("get_"));
		AllParams->SetNumberField(TEXT("limit"), 0);

		const FMonolithActionResult AllR = Discover(AllParams);
		TestTrue(TEXT("limit:0 cross-namespace succeeds"), AllR.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* AllArr = GetActionsArray(AllR);
		TestNotNull(TEXT("actions array present"), AllArr);
		if (AllArr)
		{
			TestEqual(TEXT("explicit limit:0 returns ALL matches"), AllArr->Num(), Total);
		}
	}

	// A whitespace-only filter must NOT flip into cross-namespace mode.
	{
		TSharedPtr<FJsonObject> WsParams = MakeShared<FJsonObject>();
		WsParams->SetStringField(TEXT("filter"), TEXT("   "));

		const FMonolithActionResult WsR = Discover(WsParams);
		TestTrue(TEXT("whitespace filter succeeds"), WsR.bSuccess);
		if (WsR.Result.IsValid())
		{
			TestTrue(TEXT("whitespace filter returns the namespace inventory"),
				WsR.Result->HasField(TEXT("namespaces")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDiscoverInventoryTest, "Monolith.Discover.Terse.NamespaceInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithDiscoverInventoryTest::RunTest(const FString& Parameters)
{
	using namespace MonolithDiscoverTerseTestDetail;
	auto& Registry = FMonolithToolRegistry::Get();
	const FString Namespace = TEXT("__discover_inventory_fixture");
	ON_SCOPE_EXIT { Registry.UnregisterNamespace(Namespace); };
	const auto Handler = FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
	{
		return FMonolithActionResult::Success(MakeShared<FJsonObject>());
	});
	Registry.RegisterAction(Namespace, TEXT("first"), TEXT("Fixture"), Handler, nullptr, TEXT("Zebra"));
	Registry.RegisterAction(Namespace, TEXT("second"), TEXT("Fixture"), Handler, nullptr, TEXT("Alpha"));
	Registry.RegisterAction(Namespace, TEXT("third"), TEXT("Fixture"), Handler, nullptr, TEXT("Alpha"));
	Registry.RegisterAction(Namespace, TEXT("uncategorized"), TEXT("Fixture"), Handler);
	auto Params = MakeShared<FJsonObject>();
	for (const bool bIncludeNames : {false, true})
	{
		if (bIncludeNames) Params->SetBoolField(TEXT("include_action_names"), true);
		const auto R = Discover(Params);
		if (!TestTrue(TEXT("Inventory succeeds"), R.bSuccess && R.Result.IsValid())) return false;
		TestFalse(TEXT("Inventory flag is registered without warnings"), R.Result->HasField(TEXT("warnings")));
		bool bFoundFixture = false;
		for (const auto& Value : R.Result->GetArrayField(TEXT("namespaces")))
		{
			const auto Row = Value->AsObject();
			TestEqual(TEXT("Action names require explicit opt-in"), Row->HasField(TEXT("actions")), bIncludeNames);
			TestFalse(TEXT("Every namespace has a short description"), Row->GetStringField(TEXT("description")).IsEmpty());
			TestTrue(TEXT("Every namespace has categories array"), Row->HasTypedField<EJson::Array>(TEXT("categories")));
			if (Row->GetStringField(TEXT("namespace")) != Namespace) continue;
			bFoundFixture = true;
			TestEqual(TEXT("Fixture count unchanged"), Row->GetIntegerField(TEXT("action_count")), 4);
			const auto& Categories = Row->GetArrayField(TEXT("categories"));
			if (TestEqual(TEXT("Categories are unique and omit empty"), Categories.Num(), 2))
			{
				TestEqual(TEXT("Categories sort first"), Categories[0]->AsString(), FString(TEXT("Alpha")));
				TestEqual(TEXT("Categories sort last"), Categories[1]->AsString(), FString(TEXT("Zebra")));
			}
			if (bIncludeNames)
			{
				TArray<FString> Names;
				for (const auto& Name : Row->GetArrayField(TEXT("actions"))) Names.Add(Name->AsString());
				TestEqual(TEXT("Opt-in restores every action name"), Names.Num(), 4);
				for (const TCHAR* Name : {TEXT("first"), TEXT("second"), TEXT("third"), TEXT("uncategorized")})
					TestTrue(TEXT("Registered name restored"), Names.Contains(Name));
			}
		}
		TestTrue(TEXT("Fixture included in inventory"), bFoundFixture);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
