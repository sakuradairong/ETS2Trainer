# NATIVE TRAINER

## OVERVIEW
Windows C++17 trainer and save editor; accepted local-inventory score 8, a distinct native domain with its own MSVC build and application manifest.

## WHERE TO LOOK
| Task | Location | Boundary |
| --- | --- | --- |
| Process entry, CLI, diagnostics | `src/main.cpp::wWinMain` | Owns D3D11/ImGui lifecycle and suite dispatch |
| UI state and actions | `src/ui.h`, `src/ui.cpp`, `src/fleet_ui.inc` | Tracked baseline: `AppState`, write policy, settings, dialogs and fleet UI |
| LOCAL/UNCOMMITTED workspace additions, if present | `src/workspace_catalog.h`, `src/scanner_ui.inc`, `src/save_workspace_ui.inc`, `src/workspace_shell_ui.inc`, `src/workbench_ui.inc` | Absent at tracked HEAD; local catalog routes require explicit action invocation |
| Process and multiplayer detection | `src/gameio.*`, `src/gameplay.*` | Selected-game descriptors and fail-closed detection |
| Versioned structured memory | `src/layout.*`, `src/vehicle.*`, `src/economy.*` | Fingerprints, telemetry checks, verified writes |
| Engine recovery | `src/engine.*` | `EngineTuner`, original baselines and recovery records |
| Save decoding and backup | `src/saves.*`, `src/bsii.*` | ScsC decryption and bounded BSII v3 conversion |
| New save publication | `src/savecopy.*`, `src/profilesplit.*` | Staged copies with publication guards |
| Truck, trailer and fleet edits | `src/trucktransfer.*`, `src/trucktransferio.*`, `src/fleet*` | Pure transforms separated from filesystem wrappers |
| Capacity and Home resources | `src/fueltank*`, `src/homepreset*`, `src/*_assets.rc` | Locally generated catalogs and optional assets |
| Separate overlay DLL | `hook/hook_main.cpp`, `build_hook.bat` | Independent build; several menu actions remain TODO |

## CONVENTIONS
- APIs live in `namespace ets2`; fallible operations commonly return `bool` plus `std::string* error`.
- Windows paths use wide strings; Chinese source/UI text is UTF-8 and the build passes `/utf-8`.
- At tracked HEAD, `ui.cpp` includes `fleet_ui.inc`; `trucktransfer.cpp` includes `fleet_impl.inc`. Additional workspace page includes are LOCAL/UNCOMMITTED.
- Worker results land through mutex-protected pending structs consumed by the UI thread.
- Worker write decisions read an atomic policy snapshot instead of touching `AppState`.
- Tests are compiled into the executable as functions returning `std::vector<TunerTestItem>`.
- Save-transform tests use explicit synthetic paths and injected cancellation/guard/fault callbacks.
- Settings remain compatible line-oriented `key=value` text, published through a same-directory temporary file.
- `build.bat` compiles every `src\*.cpp`; compile and link stay separate, with `/MP4`.
- Keep `build.bat` ASCII; do not expand `%VCVARS%` inside parenthesized blocks because its path contains `(x86)`.
- `app.manifest` configures elevation, DPI and long-path behavior; the test build omits that manifest.
- The tracked fuel-catalog bridge tolerates absent ignored `build/fueltank_definitions.generated.h`; capacity features then require local definitions.
- LOCAL/UNCOMMITTED, if present: the accessory-name bridge and ignored `build/accessory_names.generated.h`; neither is guaranteed at tracked HEAD.
- Tracked generator: `../tools/dev/generate_fuel_catalog.py`. LOCAL/UNCOMMITTED, if present: `../tools/dev/generate_accessory_names.py`.
- LOCAL/UNCOMMITTED, if present: `../tools/build_man_home_assets.py` regenerates local `src/man_gps_manifest.h` and `src/man_gps_assets.rc`; all three are absent at tracked HEAD.
- Resource embedding is conditional on local files under `assets/home_presets/`; missing assets do not prevent a normal build.
- `build.bat clean` removes `build/`, including catalogs, and the main executable; regenerate catalogs afterward.

## ANTI-PATTERNS
- Do not bypass the memory write gate at the actual write operation; button-disabled state is insufficient.
- The automatic fuel/battery policy exception does not authorize other writers.
- Do not reuse offsets across game/version fingerprints or accept ambiguous/truncated structured candidates.
- Do not overwrite externally changed fields during rollback or report unconfirmed restoration as success.
- Do not discard unresolved engine recovery records when switching games.
- Do not let background workers assign UI-owned result/status fields directly.
- Do not keep freezer locks after clearing their corresponding located address.
- UI save-copy operations use `copySlotToNewSave`; the lower-level core does not replace its game/process gates.
- Copy/transform publication via `copySlotToNewSave` preserves original saves, refuses existing destinations and validates staged results. Explicit plaintext edits and backup restores write original targets under their existing exited-game, backup and identity guards: plaintext edits require a successful `before_text_edit` backup; restores bind to `backup.originalDir`, require `before_restore` for an existing recognized target, and may recreate a missing target without a current-state backup.
- Do not follow reparse points or escaping paths in profile-copy/split operations.
- Capacity Mods and new saves are paired: failed copies remove only this operation's new Mod; conflicting existing Mods are refused.
- Do not distribute local game definitions, user models, personal saves or generated Mods with source.
- If LOCAL/UNCOMMITTED `../docs/design/` mockups are present, do not treat them as native functionality or connect mock actions to real writes; UI review recommendations remain proposals, not approved conventions.
- Do not treat offline regression success as game-load or real-memory-write acceptance.
- `--selftest` touches real save/backup paths; `--powertest` writes real game memory.
- The overlay's visual controls do not establish implemented fuel, damage or engine control behavior.
