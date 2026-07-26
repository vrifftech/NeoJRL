# NeoJRL

[![CI](https://github.com/vrifftech/NeoJRL/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoJRL/actions/workflows/ci.yml)

C++17 JRL/GFF editor core, CLI, and optional wxWidgets desktop GUI.

## TLK behavior

NeoJRL can open and edit journal files without a TLK. Loading `dialog.tlk` is optional and only resolves StrRef-backed localized strings for preview/search. The lookup parser is supplied by `neoshared`, so NeoJRL does not maintain a separate TLK decoder.

## Build

This repository consumes shared code from the separate `neoshared` repository. Clone the repositories as siblings:

```text
workspace/
  neoshared/
  NeoJRL/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.


Linux GUI build:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

Windows GUI build with the shared, pinned wxWidgets 3.3.3 overlay:

```powershell
& ..\neoshared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Use `-Wx OFF` on Windows for a CLI/core-only build. The default build directory is `build/`.

## Journal entry editing

The wxWidgets editor can add and delete quest entries directly in the selected quest's canonical `EntryList`.

**New Entry** appends a standard JRL entry struct containing:

```text
End            WORD, default 0
ID             DWORD, next unused quest-local ID
Text           CExoLocString, default StrRef -1
XP_Percentage  FLOAT, default 0.0
```

The new row is selected immediately, and any active entry filters are cleared so it remains visible. **Delete Entry** asks for confirmation, removes only the selected `EntryList` struct, and does not renumber the remaining entry IDs.

## Tabular import/export

`neojrl-cli` supports search plus XML and JSON import/export. XML and JSON use full hierarchical typed GFF/JRL documents. CSV/TSV flattened GFF value-table import/export is intentionally not exposed because it does not preserve the semantic structure of JRL/GFF data. The GUI exposes **Import XML**, **Import JSON**, **Export XML**, and **Export JSON**, plus copy/paste for selected quest/entry fields. Quest and entry filters must be cleared before structured export so the command cannot be mistaken for a partial-document export.

## TSLPatcher/HoloPatcher output

Generate GFF/JRL patcher instructions from original and modified JRL files:

```sh
neojrl-cli --diff-tslpatcher global_original.jrl global_modified.jrl tslpatchdata --package --filename global.jrl
neojrl-cli --diff-tslpatcher global_original.jrl global_modified.jrl global_fragment.ini --fragment --filename global.jrl
```

The generator emits `[GFFList]` field assignments and `AddFieldN` sections for representable scalar/field changes. The generic GFF exporter deliberately rejects newly appended structs under a GFF `List`, including new JRL `EntryList` rows, because a safe installer needs dynamic `TypeId=ListIndex` and `2DAMEMORY` field-path wiring. NeoDLG retains its specialized dynamic implementation for DLG graph nodes. Deleted fields, deleted entries, type changes, structural reorders, and generic list-struct additions are reported as unsupported rather than emitted with brittle fixed indexes.

Native NeoJRL add/delete remains fully supported. For a mod that adds or removes journal entries, distribute a whole replacement `global.jrl` only when that is acceptable for the intended compatibility model; the semantic patch exporter will not pretend the operation is merge-safe.

Patcher generation accepts imported modified-side journal/GFF data: `--modified-format xml|json|jrl|gff|kotor|native|auto` or `--diff-tslpatcher-import`. XML/JSON are full hierarchical GFF documents; native JRL/GFF files can also be compared directly.

Patcher export requires a canonical GFF V3 `JRL ` document containing the journal category list. The GUI exposes package and fragment export under **Export**; unrelated GFF files and GFF V4 files are rejected.

## Shared game directories

The wxWidgets application exposes **File > Open Game Directory**. Its submenu lists every saved game install from the shared `neoshared` settings store; selecting an entry opens this application's supported-file dialog with that installation as the starting folder. **Manage Game Directories...** adds, renames, rescans, activates, or removes shared entries, and changes are visible in every Neo tool.

## Continuous integration

GitHub Actions checks out `vrifftech/neoshared` beside this repository, then builds the full wxWidgets application on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. Successful non-pull-request runs publish staged Linux and Windows artifacts.

The shared dependency defaults to `neoshared/main`. Set the repository Actions variable `NEOSHARED_REF` to a release tag or commit SHA to pin normal CI builds. A manual workflow run can override the ref, and the workflow accepts the `neoshared-updated` repository-dispatch event for cross-repository compatibility checks.
