# Contributing to Monolith

Thanks for your interest in contributing. This guide covers everything you need to get started.

## Dev Environment Setup

### Prerequisites

- **Unreal Engine 5.7** (source or launcher build) is the compile floor. Contributions must build on **both UE 5.7 and UE 5.8** -- both are shipped and supported. Any engine API newer than 5.7 must sit behind an `ENGINE_MINOR_VERSION` gate, e.g. `#if ENGINE_MINOR_VERSION >= 8`, with a 5.7 code path alongside it. A change that only compiles on 5.8 will be sent back.
- **Windows, macOS, or Linux** — see [README Installation](README.md#installation) for per-platform proxy setup
- **Python 3.10+** (only needed for engine source indexing and for the cross-platform MCP proxy on macOS/Linux)
- **Git**

### Clone & Build

```bash
# Clone into your project's Plugins directory
cd YourProject/Plugins
git clone https://github.com/tumourlove/monolith.git Monolith

# Or clone the standalone development repo
git clone https://github.com/tumourlove/monolith.git C:\Projects\Monolith
```

Generate project files and build from your UE project as usual. Monolith is an editor-facing plugin: every module is `Type: "Editor"` except the small `MonolithAudioRuntime` helper, which is `Type: "Runtime"`.

### Development Workflow

Clone the repo into your UE project's `Plugins/` folder and develop in-place:

```
YourProject/Plugins/Monolith/   — edit, build, commit, push from here
```

---

## Code Structure

Monolith ships **~1,400+ actions across 25+ namespaces** (an approximate, rounded-down figure -- run `monolith_discover()` for the live count; per-module counts are deliberately not listed here because they go stale the moment an action is added).

Each module owns a specific domain:

| Module | Namespace | What It Does |
|--------|-----------|--------------|
| **MonolithCore** | `monolith` | HTTP server, tool registry, discovery, bulk-fill/describe framework, settings, auto-updater |
| **MonolithBlueprint** | `blueprint` | Blueprint read/write, variable/component/graph CRUD, node operations, compile, auto-layout |
| **MonolithMaterial** | `material` | Material graph editing, inspection, CRUD, instances, functions, HLSL |
| **MonolithAnimation** | `animation` | Sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, IKRig, Control Rig |
| **MonolithNiagara** | `niagara` | Particle systems, emitters, modules, renderers, HLSL, dynamic inputs, event handlers, sim stages |
| **MonolithMesh** | `mesh` | Mesh inspection, scene manipulation, spatial queries, blockout, procedural geometry, lighting, experimental town gen |
| **MonolithEditor** | `editor`, `animation` | Build triggers, live compile, log capture, crash context, scene capture, texture import |
| **MonolithConfig** | `config` | INI resolution, explain, diff, search |
| **MonolithIndex** | `project` | SQLite FTS5 deep project indexer |
| **MonolithSource** | `source` | Engine source lookup, call graphs, class hierarchy |
| **MonolithUI** | `ui` | Widget Blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility |
| **MonolithGAS** | `gas` | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold (gated on `WITH_GBA`) |
| **MonolithAI** | `ai` | Behavior trees, blackboards, EQS, StateTree, SmartObjects, perception, navigation, AI controllers |
| **MonolithAudio** | `audio` | Sound cues, waves, classes, submixes, attenuation, concurrency, MetaSounds |
| **MonolithAudioRuntime** | -- | Runtime support for the audio module (registers no MCP actions) |
| **MonolithLevelSequence** | `level_sequence` | Sequencer inspection: bindings, directors, event bindings |
| **MonolithReflectionIntel** | `cppreflect`, `reflect`, `decision`, `risk`, `pipeline`, `network` (plus additions to existing namespaces) | Reflection intelligence over project C++ and assets: UCLASS/UPROPERTY/UFUNCTION queries, replication audits, decision records, churn/hotspot analysis, release readiness |
| **MonolithComboGraph** | `combograph` | Optional ComboGraph integration (gated on `WITH_COMBOGRAPH`) |
| **MonolithLogicDriver** | `logicdriver` | Optional Logic Driver Pro integration (gated on `WITH_LOGICDRIVER`) |
| **MonolithBABridge** | -- | Optional Blueprint Assist integration bridge (registers no MCP actions) |

Each module follows the same file structure:

```
Source/MonolithFoo/
  Public/
    MonolithFooModule.h
    MonolithFooActions.h
  Private/
    MonolithFooModule.cpp
    MonolithFooActions.cpp
```

---

## How to Add a New Action

Actions are the atomic units of functionality. Each domain module registers actions with the central `FMonolithToolRegistry`.

### 1. Declare the handler

In `MonolithFooActions.h`, declare the handler inside its action class:

```cpp
#pragma once
#include "MonolithToolRegistry.h"

class FMonolithFooActions
{
public:
    static FMonolithActionResult HandleMyAction(const TSharedPtr<FJsonObject>& Params);
};
```

### 2. Implement the handler

In `MonolithFooActions.cpp`, validate input and return an action result. This small example echoes a label; real handlers perform their editor work after validation.

```cpp
#include "MonolithFooActions.h"
#include "MonolithJsonUtils.h"

FMonolithActionResult FMonolithFooActions::HandleMyAction(const TSharedPtr<FJsonObject>& Params)
{
    FString Label;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("label must be a nonempty string"), FMonolithJsonUtils::ErrInvalidParams);
    }

    // Registry handlers execute on the game thread.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("label"), Label);
    return FMonolithActionResult::Success(Result);
}
```

### 3. Register in StartupModule

In your existing `MonolithFooModule.cpp`, whose module class declares `void StartupModule() override` in `MonolithFooModule.h`:

```cpp
#include "MonolithFooModule.h"
#include "MonolithFooActions.h"
#include "MonolithParamSchema.h"

void FMonolithFooModule::StartupModule()
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

    Registry.RegisterAction(
        TEXT("foo"),                   // namespace
        TEXT("my_action"),             // action name
        TEXT("Validate and echo a label"),
        FMonolithActionHandler::CreateStatic(&FMonolithFooActions::HandleMyAction),
        FParamSchemaBuilder()
            .Required(TEXT("label"), TEXT("string"), TEXT("Nonempty label to echo"))
            .Build(),                  // TSharedPtr<FJsonObject> schema
        TEXT("Examples")               // optional category
    );
}
```

The argument order and `CreateStatic` delegate follow the existing `editor.get_crash_context` registration in `MonolithEditorActions.cpp`. Add `MonolithCore` to your module's Build.cs dependencies if it is not already present. Unregister the namespace during shutdown when your module owns the whole namespace.

Every production registration needs an explicit, non-null parameter schema;
use `FParamSchemaBuilder().Build()` when it takes no inputs. Declare all literal
top-level input keys read by the handler, including legacy fallback names. Nested
descriptor keys belong in the containing array/object description, not at the
action's top level. Describe the handler's actual types and defaults: schema
defaults are documentation, and are not injected into incoming parameters.

Run `python Scripts/check_schema_drift.py` before committing. It checks production
registrations in headers and source files, resolves static/lambda handlers and
schema builders/getters, and fails on unresolved syntax, missing schemas, or
undeclared literal input reads. It checks all compile-gated implementations;
Automation fixture registrations under `Tests` are excluded. The check also runs
through repository lint and the CI lint job.

Optional-plugin actions retain their registrations and schemas in all builds.
Keep plugin types and implementation bodies behind `WITH_*`; absent handlers
return `OptionalDepUnavailable` with the same plugin name used in discovery.
Owner modules call `SetOptionalDependencyAvailability` with their actual compile
flag. Set `bWholeNamespace=false` for an optional feature inside a usable base
namespace (CommonUI in `ui`, Metasound in `audio`). Discovery emits those records
as `optional_dependencies`, without marking the base namespace unavailable.

When a handler forwards its original input to a helper, record the reviewed
helper, argument position, consumed keys, and reason in
`Scripts/schema_drift_forwarding.json`. These entries add helper inputs to the
check; they never exempt direct reads. Keep them current when the helper changes.
This source lint does not infer arbitrary dynamic keys or general C++ data flow.
For existing fallback fields, adding an explicit optional field preserves the
handler's precedence; declaring a schema alias also enables alias rewriting and
canonical/alias collision checks, so that is a separate behavior decision.

### 4. Update the related files

For every new action, update its handler declaration and implementation, registration, relevant `Docs/specs/SPEC_<Module>.md` action table, `Docs/API_REFERENCE.md`, domain skill action table, and `CHANGELOG.md`. Add appropriate verification for the behavior.

For a new namespace, also update both proxy seed catalogs (`Scripts/monolith_proxy.py` and `Tools/MonolithProxy/monolith_proxy.cpp`) and the `Monolith.uplugin` description, alongside its module registration and Build.cs dependencies. Run `python Scripts/check_skill_actions.py` and the Python suite to catch catalog drift.

---

## How to Add a New Indexer

MonolithIndex uses a plugin-style indexer system. Each indexer implements `IMonolithIndexer`.

### 1. Create the indexer class

```cpp
class FMyIndexer : public IMonolithIndexer
{
public:
    virtual TArray<UClass*> GetSupportedClasses() const override
    {
        return { UMyAssetClass::StaticClass() };
    }

    virtual void IndexAsset(
        FMonolithIndexDatabase& DB,
        const FAssetData& AssetData,
        UObject* LoadedAsset) override
    {
        // Extract data and write to DB using prepared statements
        DB.InsertNode(AssetId, NodeName, NodeClass, NodeType);
    }

    virtual FString GetName() const override { return TEXT("MyIndexer"); }
};
```

### 2. Register in the subsystem

Add your indexer to `UMonolithIndexSubsystem::Initialize()`:

```cpp
Indexers.Add(MakeUnique<FMyIndexer>());
```

### 3. Add DB tables if needed

If your indexer needs new tables, add the schema in `FMonolithIndexDatabase::CreateSchema()`. Follow the existing pattern with `CREATE TABLE IF NOT EXISTS`.

---

## Coding Conventions

### General

- **UE coding standard** — `F` prefix for structs, `U` for UObjects, `T` for templates, `b` prefix for bools
- **Static action handlers** — All action classes use static methods, no instance state
- **Game thread execution** — Handlers execute on the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`

### Logging

Use the `LogMonolith` category for all log output:

```cpp
UE_LOG(LogMonolith, Log, TEXT("Something happened: %s"), *Value);
UE_LOG(LogMonolith, Warning, TEXT("Something unexpected: %s"), *Value);
UE_LOG(LogMonolith, Error, TEXT("Something failed: %s"), *Error);
```

Do **not** use `LogTemp`.

### Database Access

All SQL must use prepared statements to prevent injection:

```cpp
FSQLitePreparedStatement Stmt;
Stmt.Create(*Database, TEXT("INSERT INTO nodes (asset_id, name) VALUES (?, ?)"));
Stmt.SetBindingValueByIndex(1, AssetId);
Stmt.SetBindingValueByIndex(2, NodeName);
Stmt.Execute();
```

Never use string formatting to build SQL queries.

### Error Handling

Prefer the typed `FMonolithActionResult` helpers for new action errors:

| Helper | Class | Additional context |
|---|---|---|
| `NotFound(kind, needle, candidates)` | `not_found` | Kind and missing value; optional ranked `suggestions` (at most three) from supplied candidates |
| `InvalidParam(name, why)` | `invalid_param` | Parameter name and explanation |
| `PreconditionFailed(what, next_action)` | `precondition_failed` | Missing prerequisite and the next action to run |
| `NotImplemented(part)` | `not_implemented` | `reason:not_implemented`, `implemented:false`, and missing `part` |
| `EngineError(message)` | `engine_error` | Engine execution failure |
| `OptionalDepUnavailable(dep)` | `optional_dep_unavailable` | Required plugin in `dep_name` |

These constructors include `class`, `executed:false`, and `retryable:false` in
error data. They describe rejection before side effects. If an error follows a
mutation, override `executed` with `true` (or `"unknown"` when uncertain), retain
the affected paths, and report partial results. `WithErrorData` merges an object
over existing helper data, with caller fields taking precedence; passing null
clears it. HTTP serialization preserves existing fields and adds a missing
class to legacy errors without inventing execution evidence. Scalar legacy
error data is retained under `value`.

The complete class vocabulary is `invalid_param`, `not_found`,
`precondition_failed`, `not_implemented`, `optional_dep_unavailable`,
`engine_error`, `lease_busy`, `invalid_lease`, `not_sent`, and `unknown_outcome`.
Classify by failure semantics, not by parsing the human-readable message.

When migrating an existing error, `.WithErrorMessage(original)` preserves its
detailed diagnostic while adopting the helper's code and structured fields.
`python Scripts/codemod_error_classes.py --report errors.json` previews mechanical
not-found and missing-parameter migrations; `--write` applies them. Review the
reported action context, especially errors after mutations: the script skips
recognized side effects but is not a C++ data-flow analyzer.

Use `Error(message, code)` when preserving an existing protocol code. Include
`MonolithJsonUtils.h` and use its named constants:

```cpp
return FMonolithActionResult::Error(
    TEXT("label must be a nonempty string"), FMonolithJsonUtils::ErrInvalidParams);
```

| Constant | Code | Use |
|----------|------|-----|
| `ErrInvalidParams` | -32602 | Missing or invalid action parameters |
| `ErrInternalError` | -32603 | Internal execution failure; the default if no code is supplied |
| `ErrNotFound` | -32002 | Asset, graph, object, or other requested target was not found |
| `ErrPreconditionFailed` | -32003 | Required state is missing; provide a next action |
| `ErrOptionalDepUnavailable` | -32010 | Registered action requires an unavailable optional dependency |
| `ErrNotImplemented` | -32004 | Registered action, or a requested part of it, is not implemented; attach `reason`, `implemented=false` and the missing `part` in error data |
| `ErrParseError` | -32700 | Malformed JSON; transport layer |
| `ErrInvalidRequest` | -32600 | Invalid JSON-RPC envelope; transport layer |
| `ErrMethodNotFound` | -32601 | Unknown method, namespace, or dispatched action |
| `ErrCoordinationBusy` | -32020 | Editor lease or execution busy; coordination layer |
| `ErrInvalidLease` | -32021 | Invalid or stale lease; coordination layer |

Use `.WithErrorData(Data)` to attach structured context. The HTTP server serializes
the action result into the MCP response, including `structuredContent` for tool
errors; handlers should not construct JSON-RPC envelopes themselves.

### Asset Loading

For a failed asset lookup, use `FMonolithAssetUtils::AssetNotFound(kind, path,
ExpectedClass)` to suggest nearby assets of the expected class. It reads registry
metadata without loading candidates or scanning the disk. Suggestions use at
most 256 package names from the same folder, with a bounded nearby-folder fallback
when that folder has no candidates. An empty suggestion list is valid when no
matching metadata is available.

Before creating or saving an asset, call `MonolithCore::EnsureWritablePackagePath`
from `MonolithPackagePathValidator.h` and propagate a rejected path through
`MonolithCore::WritablePathError`. Validate the entire destination set before a
batch mutates anything, and check the resolved package when loading can follow a
redirect. `ValidatePackagePath` normalizes and checks shape; it grants no write
permission. `/Game` (including `/Game/Tests/Monolith`) is writable. Plugin content
requires an explicit `WritablePluginContentRoots` setting; indexing content via
`AdditionalContentPaths` alone grants no plugin write permission. `/Engine` and
`/Script` cannot be made writable through either setting.

Use the 4-tier fallback in `FMonolithAssetUtils`:

```cpp
UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
```

This handles: StaticLoadObject -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage.

---

## Testing

Monolith exposes a Streamable HTTP MCP server. You can test with curl or any MCP client.

### curl Examples

**Discover available tools:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

**Call an action:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"blueprint_query","arguments":{"action":"list_graphs","asset_path":"/Game/MyBlueprint.MyBlueprint"}}}'
```

**Check server status:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"monolith_status","arguments":{}}}'
```

### MCP Client

Configure your `.mcp.json` (see `Templates/.mcp.json.example`):

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

Then use Claude Code or any MCP-compatible client to interact with the tools.

### What to Verify

- Your action appears in `monolith_discover` output
- Valid params return correct results
- Missing/invalid params return clear error JSON (not crashes)
- Asset paths with various formats work (the 4-tier fallback)

---

## Pull Request Process

1. **Branch from `master`** — Use descriptive branch names: `feature/niagara-scalability`, `fix/material-connection-crash`

2. **Test in-editor** — Build and run in your UE project. Verify with curl or an MCP client that your changes work

4. **Update docs** — If you add actions, update:
   - The relevant skill in `Skills/`
   - `Docs/specs/SPEC_<Module>.md` action tables (per-module spec for the namespace you touched)
   - `CHANGELOG.md` under `## [Unreleased]`
   - `README.md` only if the approximate total crosses a rounded threshold. Public counts stay rounded down with a `+` (for example `~1,400+`); do not introduce exact integers.

5. **Commit messages** — Use conventional format: `feat:`, `fix:`, `docs:`, `refactor:`

6. **One concern per PR** — Don't mix unrelated changes

---

## Architecture Notes

- **Discovery/dispatch pattern** — Each domain exposes one `{namespace}_query(action, params)` MCP tool. The registry dispatches to the correct handler. This keeps AI context lean (a couple of dozen dispatch tools instead of one endpoint per action).
- **Thread safety** — `FMonolithToolRegistry` releases its lock before executing handlers. DB access uses `FCriticalSection`.
- **Stateless server** — No session tracking. Every request is independent.
- **MCP protocol version** — 2025-03-26, Streamable HTTP transport.

---

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
