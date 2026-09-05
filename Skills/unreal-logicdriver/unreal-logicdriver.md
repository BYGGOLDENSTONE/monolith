---
name: unreal-logicdriver
description: Use when working with Logic Driver Pro plugin via Monolith MCP -- creating and editing state machines, states, transitions, runtime PIE control, JSON spec builds, scaffolding templates, and text visualization. Triggers on state machine, logic driver, SM blueprint, state graph, FSM, state transition, dialogue tree, quest system, game flow.
---

# Unreal Logic Driver Pro Workflows

Use `logicdriver_query()` and discover the installed actions with `monolith_discover({ namespace: "logicdriver" })`. Logic Driver Pro must be available; inspect live schemas before writing assets.

## Key Parameters

- `asset_path` -- existing SM Blueprint path.
- `save_path` -- destination for a new SM or Node Blueprint.
- `node_guid` / `transition_guid` -- identifiers returned by graph inspection.
- `actor` -- actor label or name in PIE; `component_name` selects its SM component.
- `spec` -- JSON object accepted by `build_sm_from_spec`; use `export_sm_spec` to inspect its format.

## Action Reference

Names, parameters and descriptions below are taken from `RegisterAction` calls in `Source/MonolithLogicDriver/Private/`. A question mark marks an optional parameter. The namespace prefix identifies the dispatcher; send the part after the dot as `action`.

### Assets

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.create_state_machine` | `save_path`, `name`?, `parent_class`? | Create a new Logic Driver State Machine Blueprint via USMBlueprintFactory |
| `logicdriver.get_state_machine` | `asset_path` | Get full JSON dump of a state machine's structure: states, transitions, conduits, nested SMs |
| `logicdriver.list_state_machines` | `path_filter`? | List all Logic Driver State Machine Blueprints in the project via AssetRegistry |
| `logicdriver.delete_state_machine` | `asset_path` | Delete a Logic Driver State Machine Blueprint asset |
| `logicdriver.duplicate_state_machine` | `source_path`, `dest_path` | Deep copy a Logic Driver State Machine Blueprint to a new path |
| `logicdriver.create_node_blueprint` | `save_path`, `name`, `node_type`, `parent_class`? | Create a new Logic Driver Node Blueprint (custom state, transition, conduit, or state machine node class) |
| `logicdriver.get_node_blueprint` | `asset_path` | Get info about a Logic Driver Node Blueprint: class hierarchy, node type, properties |
| `logicdriver.list_node_blueprints` | `path_filter`?, `node_type`? | List all Logic Driver Node Blueprints in the project |

### Graph read/write

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.get_sm_structure` | `asset_path`, `depth`? | Get hierarchical JSON structure of an entire state machine: states, transitions, conduits, nested SMs, GUIDs |
| `logicdriver.get_node_details` | `asset_path`, `node_guid` | Get detailed info for a specific node including all UPROPERTY values and connections |
| `logicdriver.get_node_connections` | `asset_path`, `node_guid` | List all inbound and outbound transitions for a node |
| `logicdriver.find_nodes_by_type` | `asset_path`, `node_type` | Find all nodes of a given type (state/transition/conduit/any_state/state_machine) in the SM |
| `logicdriver.add_state` | `asset_path`, `name`?, `position_x`?, `position_y`? | Add a state node to a Logic Driver state machine graph |
| `logicdriver.add_transition` | `asset_path`, `source_guid`, `target_guid`, `priority`? | Add a transition between two nodes in a Logic Driver state machine |
| `logicdriver.add_conduit` | `asset_path`, `name`?, `position_x`?, `position_y`? | Add a conduit node to a Logic Driver state machine graph |
| `logicdriver.add_state_machine_node` | `asset_path`, `name`?, `reference_path`?, `position_x`?, `position_y`? | Add a nested state machine node to a Logic Driver state machine graph |
| `logicdriver.add_any_state_node` | `asset_path`, `position_x`?, `position_y`? | Add an Any State node to a Logic Driver state machine graph |
| `logicdriver.remove_node` | `asset_path`, `node_guid` | Remove a node from a Logic Driver state machine graph (breaks all connections first) |
| `logicdriver.set_node_properties` | `asset_path`, `node_guid`, `properties` | Set UPROPERTY values on a Logic Driver node via reflection |
| `logicdriver.set_initial_state` | `asset_path`, `node_guid` | Set a state as the initial state by rewiring the entry node |
| `logicdriver.set_end_state` | `asset_path`, `node_guid`, `is_end_state` | Set or clear the end state flag on a state node |
| `logicdriver.set_node_class` | `asset_path`, `node_guid`, `class_path` | Set the custom node class (NodeInstanceClass) on a Logic Driver node via reflection |
| `logicdriver.rename_node` | `asset_path`, `node_guid`, `new_name` | Rename a node in a Logic Driver state machine |
| `logicdriver.compile_state_machine` | `asset_path` | Compile a Logic Driver State Machine Blueprint and return success/failure with error messages |
| `logicdriver.find_nodes_by_class` | `asset_path`, `class_name` | Find all nodes whose class name matches a given string (full or partial match) |
| `logicdriver.get_sm_statistics` | `asset_path` | Get statistics for a state machine: state/transition/conduit/nested SM counts, max depth, total nodes |
| `logicdriver.move_node` | `asset_path`, `node_guid`, `position_x`, `position_y` | Move a node to a specific position in the graph editor |
| `logicdriver.auto_arrange_graph` | `asset_path`, `formatter`? | Auto-arrange all nodes in a state machine graph. Uses Blueprint Assist formatter if available, otherwise built-in BFS layout |

### Node configuration

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.configure_state` | `asset_path`, `node_guid`, `always_update`?, `disable_tick_transition`?, `exclude_from_any_state`? | Set state node configuration flags (always_update, disable_tick_transition, exclude_from_any_state) via reflection |
| `logicdriver.configure_transition` | `asset_path`, `node_guid`, `priority`?, `color`?, `eval_mode`?, `can_eval_with_start_state`? | Set transition properties (priority, color, eval_mode, can_eval_with_start_state) via reflection |
| `logicdriver.configure_conduit` | `asset_path`, `node_guid`, `eval_with_transitions`?, `conduit_as_state`? | Set conduit properties (eval_with_transitions, conduit_as_state) via reflection |
| `logicdriver.set_transition_condition` | `asset_path`, `transition_guid`, `condition_type`, `params`? | Set transition condition type: always_true, time_delay, event_based, or tag_check. Sets properties via reflection (no graph rewiring). |
| `logicdriver.set_state_tags` | `asset_path`, `node_guid`, `gameplay_tags` | Set gameplay tags on a state node. Clears existing tags and applies the provided array. |
| `logicdriver.get_exposed_properties` | `asset_path`, `node_guid`? | Read all exposed graph properties on SM nodes — FSMGraphProperty variables visible in the graph editor |
| `logicdriver.set_exposed_property` | `asset_path`, `node_guid`, `property_name`, `value` | Set an exposed property value on an SM node by name via reflection |
| `logicdriver.configure_state_machine_node` | `asset_path`, `node_guid`, `reuse_if_not_end_state`?, `reuse_current_state`?, `allow_independent_tick`? | Configure a nested state machine node: reuse behavior, independent tick, and other settings |

### Runtime / PIE

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.runtime_get_sm_state` | `actor`, `component_name`? | Get the active state(s) of a live SM instance in PIE — state name, GUID, time in state |
| `logicdriver.runtime_start_sm` | `actor`, `component_name`? | Initialize and start a live SM instance during PIE |
| `logicdriver.runtime_stop_sm` | `actor`, `component_name`? | Stop a live SM instance during PIE |
| `logicdriver.runtime_restart_sm` | `actor`, `component_name`? | Restart a live SM instance during PIE (stop + initialize + start) |
| `logicdriver.runtime_switch_state` | `actor`, `state_guid`, `component_name`? | Force-switch to a specific state by GUID during PIE |
| `logicdriver.runtime_evaluate_transitions` | `actor`, `component_name`? | Force transition evaluation on a live SM instance during PIE |
| `logicdriver.runtime_get_state_history` | `actor`, `component_name`?, `limit`? | Get state transition history from a live SM instance during PIE |

### JSON and specs

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.export_sm_json` | `asset_path`, `output_path`? | Export a state machine's full structure as JSON. Optionally write to a file on disk |
| `logicdriver.build_sm_from_spec` | `save_path`, `spec` | Create a complete state machine from a JSON spec in one call. The crown jewel — states, transitions, conduits, nested SMs, initial/end markers, all wired and compiled |
| `logicdriver.export_sm_spec` | `asset_path` | Export a state machine as a spec JSON (same format as build_sm_from_spec input). Inverse of build_sm_from_spec |
| `logicdriver.import_sm_json` | `save_path`, `json_path_or_data` | Import a state machine from a JSON spec — either a file path or inline JSON string. Parses and delegates to build_sm_from_spec logic |
| `logicdriver.compare_state_machines` | `path_a`, `path_b` | Compare two state machines structurally: diff states, transitions, and topology by name |

### Scaffolding

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.scaffold_hello_world_sm` | `save_path`, `name`? | Create a ready-to-use SM Blueprint with 3 states (Idle->Active->Complete) and transitions — a quick-start template |
| `logicdriver.scaffold_weapon_sm` | `save_path`, `name`? | Create an FPS weapon state machine: Idle->Drawing->Ready->Firing->Cooldown->Reloading with transitions |
| `logicdriver.scaffold_horror_encounter_sm` | `save_path`, `name`? | Create a horror encounter state machine: Dormant->Lurking->Stalking->Chasing->Attacking->Retreating->Despawned |
| `logicdriver.scaffold_game_flow_sm` | `save_path`, `name`? | Create a game flow state machine: MainMenu->Loading->Gameplay->Pause->Results->Credits with loops |
| `logicdriver.scaffold_dialogue_sm` | `save_path`, `name`, `dialogue_nodes`? | Create a dialogue state machine with speaker/text states wired in sequence, with optional branching choices |
| `logicdriver.scaffold_quest_sm` | `save_path`, `name`, `objectives`? | Create a quest state machine: Inactive -> Active -> [objectives] -> Complete/Failed |
| `logicdriver.scaffold_interactable_sm` | `save_path`, `name`, `states`? | Create an interactable state machine with custom states (default: locked/unlocked/open/closed) |

### Discovery

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.get_sm_overview` | `path_filter`? | Project scan: list and count SM Blueprints and Node Blueprints; component usage is not indexed |
| `logicdriver.validate_state_machine` | `asset_path` | Validate a state machine for common issues: missing initial state, orphaned states, unreachable nodes |
| `logicdriver.find_sm_references` | `asset_path` | Find all Blueprints in the project that reference a given SM Blueprint (via dependencies) |
| `logicdriver.visualize_sm_as_text` | `asset_path`, `format` | Generate a text diagram of a state machine in ASCII, Mermaid, or DOT format |
| `logicdriver.explain_state_machine` | `asset_path` | Generate a structured explanation of a state machine: purpose, states, flow paths, key decisions, complexity rating |
| `logicdriver.find_node_class_usages` | `node_bp_path` | Search all SM Blueprints in the project for nodes that use a specific Node Blueprint class |

Project overview does not index SM component usage. Inspect a specific actor Blueprint with `logicdriver.get_sm_component_config`.

### Components

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.get_sm_component_config` | `blueprint_path`, `component_name`? | Read SM component configuration on an actor Blueprint: state machine class, auto-start, tick interval, network config, and all SM-specific properties |
| `logicdriver.add_sm_component` | `blueprint_path`, `sm_path`?, `component_name`? | Add a Logic Driver SM component to an actor Blueprint via SimpleConstructionScript |
| `logicdriver.configure_sm_component` | `blueprint_path`, `component_name`?, `auto_start`?, `tick_interval`?, `network_config`? | Set SM component properties on an actor Blueprint: auto_start, tick_interval, network_config via reflection |

### Text graphs

| Action | Parameters | Purpose |
|---|---|---|
| `logicdriver.get_text_graph_content` | `asset_path`, `node_guid`? | Read FSMTextGraphProperty content (dialogue text, speaker names) from state nodes |
| `logicdriver.get_dialogue_flow` | `asset_path` | Walk entire SM and extract dialogue flow: speakers, lines, choices, branching paths |

## Workflow Examples

### Create, compile and inspect

```javascript
logicdriver_query({ action: "build_sm_from_spec", params: {
  save_path: "/Game/StateMachines/SM_EnemyAI",
  spec: {
    states: [
      { name: "Idle", is_initial: true },
      { name: "Patrol" },
      { name: "Chase" }
    ],
    transitions: [
      { from: "Idle", to: "Patrol" },
      { from: "Patrol", to: "Chase" },
      { from: "Chase", to: "Idle" }
    ]
  }
}})
logicdriver_query({ action: "compile_state_machine", params: {
  asset_path: "/Game/StateMachines/SM_EnemyAI"
}})
logicdriver_query({ action: "get_sm_structure", params: {
  asset_path: "/Game/StateMachines/SM_EnemyAI"
}})
```

### Scaffold a horror encounter

```javascript
logicdriver_query({ action: "scaffold_horror_encounter_sm", params: {
  save_path: "/Game/StateMachines/SM_GhostEncounter"
}})
logicdriver_query({ action: "validate_state_machine", params: {
  asset_path: "/Game/StateMachines/SM_GhostEncounter"
}})
```

Wait for each result before the next call. Inspect generated state transitions and verify runtime behavior in PIE; scaffold creation alone does not prove the encounter works. Runtime actions require an actor with an SM component in the PIE world. Keep a coordination lease for the complete edit, compile and readback workflow when sharing an editor.
