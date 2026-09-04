// SPDX-License-Identifier: MIT
// Survivor C (Action + namespace did_you_mean fuzzy match) automation tests
// — plan §12 "Survivor C", plan §3.C.
// Plan: Plugins/Monolith/Docs/plans/2026-05-27-mcp-llm-ergonomics.md
//
// DEVIATION NOTE: plan §6 file-table specifies `Source/MonolithCore/Tests/...`.
// This file lives under `Source/MonolithCore/Private/Tests/...` so UBT's
// auto-include of `Private/` picks it up without a Build.cs change. Matches the
// existing precedent at `Private/Tests/MonolithResponseShapingTest.cpp` and
// `Private/Reflection/Tests/MonolithReflectionWalkerTest.cpp`.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithFuzzyMatch.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithFuzzyMatchTestDetail
{
	// Helper: pull the suggestions array (if any) out of an Error result's
	// ErrorData payload. Returns true if `suggestions[]` is present.
	static bool GetSuggestions(
		const FMonolithActionResult& Result,
		TArray<FString>& OutKeys,
		TArray<float>& OutScores)
	{
		OutKeys.Reset();
		OutScores.Reset();

		if (Result.bSuccess || !Result.ErrorData.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
		if (!Result.ErrorData->TryGetObject(DataObjPtr) || !DataObjPtr || !DataObjPtr->IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!(*DataObjPtr)->TryGetArrayField(TEXT("suggestions"), Arr) || !Arr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}
			FString Key;
			if (!(*Obj)->TryGetStringField(TEXT("action"), Key))
			{
				(*Obj)->TryGetStringField(TEXT("namespace"), Key);
			}
			double Score = 0.0;
			(*Obj)->TryGetNumberField(TEXT("score"), Score);
			OutKeys.Add(Key);
			OutScores.Add(static_cast<float>(Score));
		}
		return true;
	}
}

// ---------------------------------------------------------------------------
// Test 1: Action typo within a known namespace.
// `monolih_discover` → should suggest `discover` (and others) for the
// `monolith` namespace.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyMatchActionTypoCorpusTest,
	"Monolith.FuzzyMatch.ActionTypoCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyMatchActionTypoCorpusTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithFuzzyMatchTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Skip if the `monolith` namespace is somehow not registered — the test
	// has no signal to assert on.
	if (Registry.GetActions(TEXT("monolith")).Num() == 0)
	{
		AddWarning(TEXT("Skipping: `monolith` namespace not registered in this test run"));
		return true;
	}

	// Three typo variants of well-known monolith actions.
	const TArray<TPair<FString, FString>> Typos = {
		{ TEXT("monolih_discover"), TEXT("discover") }, // dispatched as namespace="monolith", action="monolih_discover"
		{ TEXT("statux"),           TEXT("status")   },
		{ TEXT("guid"),             TEXT("guide")    },
	};

	for (const TPair<FString, FString>& T : Typos)
	{
		FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("monolith"), T.Key, MakeShared<FJsonObject>());

		TestFalse(FString::Printf(TEXT("'%s' dispatch returns error"), *T.Key), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("'%s' carries ErrorData"), *T.Key), Result.ErrorData.IsValid());

		TArray<FString> SuggKeys;
		TArray<float> SuggScores;
		const bool bHasSuggestions = GetSuggestions(Result, SuggKeys, SuggScores);
		TestTrue(FString::Printf(TEXT("'%s' carries suggestions[] payload"), *T.Key), bHasSuggestions);

		if (bHasSuggestions)
		{
			TestTrue(FString::Printf(TEXT("'%s' returns at least one suggestion"), *T.Key),
				SuggKeys.Num() >= 1);
			TestTrue(FString::Printf(TEXT("'%s' returns at most three suggestions"), *T.Key),
				SuggKeys.Num() <= 3);
			if (SuggKeys.Num() >= 1)
			{
				// The correct answer should be position 1 (index 0).
				TestEqual(
					FString::Printf(TEXT("'%s' top suggestion is '%s'"), *T.Key, *T.Value),
					SuggKeys[0], T.Value);
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Namespace typo (unknown namespace dispatch).
// `materail_query.foo` should suggest `material` namespace etc.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyMatchNamespaceTypoCorpusTest,
	"Monolith.FuzzyMatch.NamespaceTypoCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyMatchNamespaceTypoCorpusTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithFuzzyMatchTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	const TArray<FString> AvailableNamespaces = Registry.GetNamespaces();
	if (AvailableNamespaces.Num() == 0)
	{
		AddWarning(TEXT("Skipping: registry has zero namespaces"));
		return true;
	}

	// Build typo from a known namespace (insert a swap). Falls back to
	// `materail` if `material` exists; otherwise typos the first available
	// namespace by transposing its second/third chars.
	FString GoodNs;
	if (AvailableNamespaces.Contains(TEXT("material")))
	{
		GoodNs = TEXT("material");
	}
	else
	{
		GoodNs = AvailableNamespaces[0];
	}

	// Generate a single-char-deletion typo.
	FString TypoNs = GoodNs;
	if (TypoNs.Len() >= 3)
	{
		TypoNs.RemoveAt(2, 1);
	}
	else
	{
		TypoNs.AppendChar(TEXT('x'));
	}

	FMonolithActionResult Result = Registry.ExecuteAction(
		TypoNs, TEXT("any_action"), MakeShared<FJsonObject>());

	TestFalse(TEXT("unknown namespace returns error"), Result.bSuccess);
	TestTrue(TEXT("error carries ErrorData"), Result.ErrorData.IsValid());

	TArray<FString> SuggKeys;
	TArray<float> SuggScores;
	const bool bHasSuggestions = GetSuggestions(Result, SuggKeys, SuggScores);
	TestTrue(TEXT("unknown namespace carries suggestions[]"), bHasSuggestions);

	if (bHasSuggestions)
	{
		TestTrue(TEXT("at least one namespace suggestion"), SuggKeys.Num() >= 1);
		TestTrue(TEXT("at most three namespace suggestions"), SuggKeys.Num() <= 3);
		if (SuggKeys.Num() >= 1)
		{
			TestEqual(
				FString::Printf(TEXT("top namespace suggestion equals '%s'"), *GoodNs),
				SuggKeys[0], GoodNs);
		}
	}

	// Verify the suggestion kind is "namespace", not "action".
	if (Result.ErrorData.IsValid())
	{
		const TSharedPtr<FJsonObject>* DataObj = nullptr;
		if (Result.ErrorData->TryGetObject(DataObj) && DataObj && DataObj->IsValid())
		{
			FString Kind;
			(*DataObj)->TryGetStringField(TEXT("kind"), Kind);
			TestEqual(TEXT("error.data.kind == 'namespace'"), Kind, FString(TEXT("namespace")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: No-match path — pure gibberish.
// Asserts no crash. Per plan §12, allows up to 3 low-scoring suggestions; the
// real signal is "no exception, no deadlock".
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyMatchNoMatchPathTest,
	"Monolith.FuzzyMatch.NoMatchPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyMatchNoMatchPathTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithFuzzyMatchTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("xyzzy_qwerty_fnord"), TEXT("plover"), MakeShared<FJsonObject>());

	TestFalse(TEXT("gibberish namespace returns error"), Result.bSuccess);
	TestTrue(TEXT("ErrorData still attached on no-match"), Result.ErrorData.IsValid());

	// Cap-only check: suggestions[] exists (may be empty) and is bounded.
	TArray<FString> SuggKeys;
	TArray<float> SuggScores;
	const bool bHasSuggestions = GetSuggestions(Result, SuggKeys, SuggScores);
	TestTrue(TEXT("suggestions[] field present even on no-match"), bHasSuggestions);
	TestTrue(TEXT("suggestions count <= 3 on no-match"), SuggKeys.Num() <= 3);
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Registry metadata remains accessible while a game-thread handler
// runs. Workers use metadata APIs only; UObject action dispatch is game-thread
// only. A bounded wait inside the handler detects a held registry mutex without
// relying on scheduler-sensitive latency ratios. Workers capture values only,
// so even the failure/timeout path cannot outlive a stack capture.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyMatchLockReleaseHandoverTest,
	"Monolith.FuzzyMatch.LockReleaseHandover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyMatchLockReleaseHandoverTest::RunTest(const FString& /*Parameters*/)
{
	auto& Registry = FMonolithToolRegistry::Get();
	const FString Namespace = TEXT("__fuzzy_lock_handover_test");
	ON_SCOPE_EXIT { Registry.UnregisterNamespace(Namespace); };
	Registry.RegisterAction(Namespace, TEXT("probe"), TEXT("Metadata lock probe"),
		FMonolithActionHandler::CreateLambda([Namespace](const TSharedPtr<FJsonObject>&)
		{
			auto Worker = Async(EAsyncExecution::Thread, [Namespace]
			{
				auto& WorkerRegistry = FMonolithToolRegistry::Get();
				return WorkerRegistry.GetNamespaces().Contains(Namespace)
					&& WorkerRegistry.HasAction(Namespace, TEXT("probe"));
			});
			const bool bCompleted = Worker.WaitFor(FTimespan::FromSeconds(5));
			auto Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("completed_during_handler"), bCompleted);
			Result->SetBoolField(TEXT("metadata_valid"), bCompleted && Worker.Get());
			return FMonolithActionResult::Success(Result);
		}));

	const auto Probe = Registry.ExecuteAction(Namespace, TEXT("probe"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Game-thread handler executes"), Probe.bSuccess);
	if (Probe.Result.IsValid())
	{
		TestTrue(TEXT("Worker acquires registry mutex before handler returns"), Probe.Result->GetBoolField(TEXT("completed_during_handler")));
		TestTrue(TEXT("Worker sees consistent registry metadata"), Probe.Result->GetBoolField(TEXT("metadata_valid")));
	}

	// Exercise unknown-action suggestion snapshots on the game thread while
	// a worker repeatedly reads registry metadata. No handlers run on workers.
	auto MetadataWorker = Async(EAsyncExecution::Thread, [Namespace]
	{
		for (int32 Index = 0; Index < 200; ++Index)
		{
			auto& WorkerRegistry = FMonolithToolRegistry::Get();
			if (!WorkerRegistry.GetNamespaces().Contains(Namespace)
				|| !WorkerRegistry.HasAction(Namespace, TEXT("probe"))) { return false; }
		}
		return true;
	});
	for (int32 Index = 0; Index < 50; ++Index)
	{
		const auto Missing = Registry.ExecuteAction(Namespace,
			FString::Printf(TEXT("probee_%d"), Index), MakeShared<FJsonObject>());
		TestFalse(TEXT("Unknown action rejected on game thread"), Missing.bSuccess);
		TestEqual(TEXT("Unknown action keeps method-not-found code"), Missing.ErrorCode, -32601);
		TestTrue(TEXT("Fuzzy diagnostics retained"), Missing.ErrorData.IsValid());
	}
	const bool bMetadataDone = MetadataWorker.WaitFor(FTimespan::FromSeconds(5));
	TestTrue(TEXT("Concurrent metadata reader completed"), bMetadataDone);
	if (bMetadataDone) { TestTrue(TEXT("Metadata stayed valid during fuzzy dispatch"), MetadataWorker.Get()); }
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: Scoring order — returned suggestions are monotonically descending.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyMatchScoringOrderTest,
	"Monolith.FuzzyMatch.ScoringOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyMatchScoringOrderTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithFuzzyMatchTestDetail;

	// Direct unit-test the scorer with a controlled corpus so the assertion
	// does not depend on what's registered in this editor session.
	const TArray<FString> Corpus = {
		TEXT("discover"),
		TEXT("status"),
		TEXT("update"),
		TEXT("reindex"),
		TEXT("guide"),
		TEXT("disciplines"),
	};

	const FString Needle = TEXT("discove"); // typo of "discover" — closest match
	TArray<MonolithFuzzyMatchDetail::FFuzzyCandidate> Top3 =
		MonolithFuzzyMatchDetail::ScoreFuzzyMatches(Needle, Corpus, 3);

	TestTrue(TEXT("returns at least 1 candidate"), Top3.Num() >= 1);
	TestTrue(TEXT("returns at most 3 candidates"), Top3.Num() <= 3);
	if (Top3.Num() >= 1)
	{
		TestEqual(TEXT("top candidate is 'discover'"), Top3[0].Key, FString(TEXT("discover")));
	}
	// Monotonically descending scores.
	for (int32 i = 1; i < Top3.Num(); ++i)
	{
		TestTrue(
			FString::Printf(TEXT("score[%d]=%.4f <= score[%d]=%.4f"),
				i, Top3[i].Score, i - 1, Top3[i - 1].Score),
			Top3[i].Score <= Top3[i - 1].Score);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
