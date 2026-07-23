// SPDX-License-Identifier: MIT
// Faz 1 — `jobs` namespace: the MCP surface of FMonolithJobManager.
// Roadmap: Docs/GOLDENSTONE_ROADMAP.md "Faz 1" step 2.
//
// PLACEMENT DECISION (deviation from the literal extension template — justified here):
// the roadmap's "Calisma Ilkeleri" section says "yeni namespace = yeni modul"
// (copy Source/MonolithConfig/). These four actions live in MonolithCore instead, next
// to the registry they expose, because:
//
//   1. PRECEDENT — MonolithCore already owns a namespace exactly like this: `monolith`
//      (discover/status/update/reindex/guide) is implemented in Private/MonolithCoreTools.h
//      and registered from FMonolithCoreModule::StartupModule. And "one namespace = one
//      module" is not what the codebase actually does: MonolithReflectionIntel alone owns
//      eight namespaces (decision / risk / network / cppreflect / pipeline / reflect /
//      audit / sourceaudit). The template describes the common case, not an invariant.
//   2. NO ENCAPSULATION GAIN — FMonolithJobManager is a MonolithCore singleton. A separate
//      module would be a pure veneer over it with a hard MonolithCore dependency, buying a
//      module boundary that separates nothing.
//   3. LIFETIME — FMonolithCoreModule::ShutdownModule already calls
//      FMonolithJobManager::Reset(). Registering the namespace from a *different* module
//      would put namespace teardown and registry teardown in two independently-ordered
//      module shutdowns; keeping both in MonolithCore makes the ordering explicit.
//   4. AVAILABILITY — any module can create a job, so polling must be available whenever
//      the manager is. Hanging `jobs` off an optional feature module would allow a
//      configuration where work is queued but unpollable.
//   5. UPSTREAM MERGE CONFLICTS — the roadmap's real constraint is "new features in new
//      files so merges stay clean". This is satisfied better here: everything lives in two
//      NEW files, and the only shared-file edits are two lines in MonolithCoreModule.cpp
//      plus the settings toggle. A new module would additionally have to edit the
//      Monolith.uplugin Modules[] array — a single small JSON list that every upstream
//      module addition also touches, i.e. the most conflict-prone file in the repo.
//
// The rest of the template IS followed: a `bEnableJobs` toggle on UMonolithSettings,
// Registry.RegisterAction() calls, and automation tests under Private/Tests/.

#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

struct FMonolithJob;

/**
 * `jobs` namespace handlers — list / poll / cancel / clear over FMonolithJobManager.
 *
 * ERROR CONTRACT (mirrors the manager's own semantics: every mutator returns false for an
 * unknown id AND for an already-terminal job):
 *   - unknown id            -> ErrInvalidParams + error.data.error_code = "UNKNOWN_JOB"
 *   - cancel on a finished job -> ErrInvalidParams + error.data.error_code = "JOB_NOT_RUNNING"
 *   - bad `state` filter    -> ErrInvalidParams + error.data.error_code = "INVALID_STATE_FILTER"
 * The `error.data.error_code` string field follows the neighbouring convention established
 * by INVALID_CURSOR (MonolithSourceActions.cpp / RICursorCodec.cpp).
 */
class FMonolithJobActions
{
public:
	/** The namespace these actions register under. Single source of truth for the token. */
	static const TCHAR* GetNamespace();

	/** Register list/poll/cancel/clear with the tool registry. */
	static void RegisterAll();

	// --- Action handlers ---

	/** jobs_list — every known job (running + retained finished), oldest-issued first. */
	static FMonolithActionResult HandleList(const TSharedPtr<FJsonObject>& Params);

	/** jobs_poll — one job by id, including its result / error payload. */
	static FMonolithActionResult HandlePoll(const TSharedPtr<FJsonObject>& Params);

	/** jobs_cancel — cooperative cancel of a RUNNING job. */
	static FMonolithActionResult HandleCancel(const TSharedPtr<FJsonObject>& Params);

	/** jobs_clear — evict finished jobs only. Never touches running jobs. */
	static FMonolithActionResult HandleClear(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Serialise one job snapshot to the wire shape shared by list / poll / cancel.
	 * Exposed for automation tests so they assert against the real serializer.
	 *
	 * @param bIncludeResult  false drops the (potentially large) `result` payload — how
	 *                        `list` renders completed jobs. `poll` always passes true.
	 */
	static TSharedPtr<FJsonObject> JobToJson(const FMonolithJob& Job, bool bIncludeResult);
};
