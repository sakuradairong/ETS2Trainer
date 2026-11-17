# PROJECT KNOWLEDGE BASE

**Generated:** 2026-11-17
**Commit:** 481babae0d17dd890710910efa9c2c73dda265c2 (tracked baseline; local/uncommitted additions are qualified below)
**Branch:** main

## OVERVIEW
Windows C++17/MSVC trainer and save editor for Euro Truck Simulator 2 and American Truck Simulator, using Dear ImGui 1.92.9 and D3D11.
The native application, optional hook DLL, and offline tools have separate roles.

## STRUCTURE
```text
ETS2Trainer/
|-- cpp/                 # Native application, hook, embedded resources; see its AGENTS.md
|   |-- src/             # UI fragments, game access, save transformations, resource bridges
|   |-- hook/            # Separate DLL; some actions remain TODO
|   `-- build/           # Generated catalogs, test executable, reports and settings
|-- docs/                # Safety and recovery policy
|-- tools/               # Save research and asset-generation utilities
|   `-- dev/             # Catalog generation and executable inspection
`-- README.md            # User behavior, build requirements and test commands
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Native implementation guidance | `cpp/AGENTS.md` | Specialized native domain; score 8 |
| Startup, CLI and test dispatch | `cpp/src/main.cpp` | `wWinMain` dispatches application modes |
| UI state and routing | `cpp/src/ui.h`, `ui.cpp`, `fleet_ui.inc` | Native UI state and fleet controls |
| Game selection and process access | `cpp/src/gameplay.*`, `gameio.*` | Includes multiplayer selection; ATS layouts are enabled |
| Layout verification and memory scans | `cpp/src/` | Preserve fingerprint and telemetry checks |
| Save conversion and new slots | `cpp/src/bsii.*`, `savecopy.*`, `profilesplit.*` | UI publishes through `copySlotToNewSave` |
| Truck/fleet transfers | `cpp/src/trucktransfer.cpp`, `trucktransferio.*` | Pure transforms separate from I/O gates; `fleet_impl.inc` is included |
| Safety and recovery contracts | `docs/safety-and-recovery.md` | Memory rollback, engine recovery, save/mod cleanup |
| Generated catalogs and assets | `tools/dev/generate_fuel_catalog.py`, `cpp/src/homepreset_assets.rc` | Local game inputs; see resource notes below |

## CODE MAP
Structural/global reference counts are unmeasured: clangd and ast-grep were unavailable during reduction.

| Symbol | Type | Location | Refs | Role |
|--------|------|----------|------|------|
| `wWinMain` | Entry function | `cpp/src/main.cpp::wWinMain` | Unmeasured | Native startup and mode dispatch |
| `renderApp` | Function | `cpp/src/ui.cpp::renderApp` | Unmeasured | Application rendering |
| `AppState` | Type | `cpp/src/ui.h::AppState` | Unmeasured | Shared UI/application state |
| `EngineTuner` | Type | `cpp/src/engine.h::EngineTuner` | Unmeasured | Engine tuning and recovery |
| `writeGateReason` | Symbol | `cpp/src/ui.h::writeGateReason` | Unmeasured | Memory-write policy |
| `groupSaveProfiles` | Symbol | `cpp/src/saveprofiles.h::groupSaveProfiles` | Unmeasured | Save-profile grouping |
| `probeBankObject` | Symbol | `cpp/src/economy.h::probeBankObject` | Unmeasured | Bank-object probing |
| `verifyGameLayout` | Symbol | `cpp/src/layout.h::verifyGameLayout` | Unmeasured | Layout verification |

## CONVENTIONS
- UI and documentation are Chinese; source uses UTF-8, wide paths and namespace `ets2`.
- APIs commonly return `bool` with an error-out parameter; native test cases use `vector<TunerTestItem>`.
- Settings retain compatible `key=value` storage (`cpp/src/ui.cpp`).
- Worker results are mutex-protected and applied on the UI thread (`cpp/src/ui.h`).

## ANTI-PATTERNS (THIS PROJECT)
- Do not bypass memory-write policy, fingerprint/telemetry checks, ambiguity rejection, external-change-aware rollback or engine recovery credentials.
- Automatic fuel/battery handling has a narrow policy exception; do not generalize it to other writes.
- Copy/transform publication creates new slots without overwriting originals or existing destinations; reject escaping paths and reparse paths. Explicit plaintext edits and restores retain game-exit, backup and original-target checks: plaintext edits require `before_text_edit`; restores bind to `backup.originalDir`, require `before_restore` for an existing recognized target, and may recreate a missing target without a current-state backup.
- Do not distribute local game definitions, models, saves or Mods.
- Prototype review recommendations are proposals, not current native conventions or evidence of acceptance.
- Do not claim a rollover feature; the README explicitly says none exists.

## UNIQUE STYLES
- Tracked UI and fleet implementation use textual `fleet_ui.inc` and `fleet_impl.inc` composition.
- ETS2 backups use `backups/`; ATS backups use `backups/ats`; exports use the selected game's Documents/exports.
- Steam application IDs are 227300 (ETS2) and 270880 (ATS); game selection affects paths and layout handling.

## COMMANDS
Commands below are documented evidence only; none were executed during this guidance run.
Use VS2022 Build Tools and the Windows SDK. Run these offline build/test targets from `cpp/`:
```bat
build.bat check
build.bat
build.bat test
build_hook.bat
build\ETS2Trainer_test.exe --gametest
build\ETS2Trainer_test.exe --crosstest
build\ETS2Trainer_test.exe --smoketest --game=ets2
build\ETS2Trainer_test.exe --smoketest --game=ats
```
`check` compiles only; normal build produces `../ETS2Trainer.exe`; hook build produces `../ETS2TrainerHook.dll`.
The listed test modes use synthetic/self memory, a child target, or UI smoke respectively; reports/settings go in `cpp/build`.
From `cpp/`, `--selftest` touches backups; `--livetest` and `--enginetest` may attach to games; `--powertest` writes real memory. These are not interchangeable with offline checks.
From the repository root, fuel catalog generation requires local SCS extractions:
```bat
python tools/dev/generate_fuel_catalog.py --root build/fuel-def-reference --root build/fuel-volvo-reference --root build/fuel-man-reference --root build/fuel-scania-electric-reference
```
From repository root, research tools `python tools/decrypt_sii.py <src> <dst>` and `python tools/bsii_to_text.py <src> <dst>` overwrite their destination; decryption needs an AES backend and BSII conversion supports v3.
`python tools/analyze_trailers.py <text-save>` prints analysis; `tools/dev/dump_fields.ps1` inspects a disk executable, not live memory.

## NOTES
- The generated fuel catalog lives at `cpp/build/fueltank_definitions.generated.h`; its generator refuses conflicting definitions. `build.bat clean` deletes catalogs, so regenerate them afterward.
- The fuel catalog bridge tolerates its absence. Optional `cpp/assets/home_presets/` inputs embed through `cpp/src/homepreset_assets.rc`; builds tolerate their absence.
- Offline tests do not establish game acceptance. Failed mod/save pair creation must clean up its new artifacts.
- Runtime/generated data is ignored; measured HEAD does not describe all existing working-tree changes.

## LOCAL/UNCOMMITTED CONTEXT (ABSENT AT THE TRACKED BASELINE)
- If present, `cpp/src/workspace_catalog.h`, `scanner_ui.inc`, `save_workspace_ui.inc`, `workspace_shell_ui.inc` and `workbench_ui.inc` add local workspace routing/pages.
- If present, `tools/dev/generate_accessory_names.py` reads installed HashFS from repository root; optional `--game`/`--lang` select inputs. Its accessory-name bridge/catalog are local additions.
- If present, `tools/build_man_home_assets.py` regenerates local `cpp/src/man_gps_manifest.h` and `man_gps_assets.rc`; these are not clean-checkout requirements.
- If present, `docs/design/full-app-2026-11-16/` and `docs/design/workbench-2026-11-16/` are browser demos opened directly. The full-app prototype uses mock data and ordered app/workshop/live/saves/integration scripts; it has no filesystem, network, process, keyboard or localStorage APIs. Its UI review remains unapproved proposal material.
