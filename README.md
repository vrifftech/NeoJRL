# NeoJRL

[![CI](https://github.com/vrifftech/NeoJRL/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoJRL/actions/workflows/ci.yml)

NeoJRL is a purpose-aware editor for BioWare `JRL V3.2` journal resources. It supports the related but different journal schemas used by KotOR/KotOR II and Neverwinter Nights/Neverwinter Nights 2.

## Journal profiles

NeoJRL detects the selected category's schema from its fields and adapts the editor accordingly.

### KotOR / KotOR II

Category fields:

```text
Name            CExoLocString
Tag             CExoString
Priority        DWORD
Picture         WORD
PlotIndex       INT
PlanetID        INT
EntryList       List
```

Entry fields:

```text
ID              DWORD
Text            CExoLocString
XP_Percentage   FLOAT
End             WORD used as a Boolean
```

`PlotIndex` selects a row from `PlotXP.2da`; `XP_Percentage` is a direct multiplier applied to that row's XP (`0.5` means 50%, `1.0` means 100%). `PlanetID`, `PlotIndex`, and `Picture` are data-driven identifiers, so NeoJRL validates their storage types without inventing unsupported maximum values.

### Neverwinter Nights / Neverwinter Nights 2

Category fields:

```text
Name            CExoLocString
Tag             CExoString
Comment         CExoString, authoring metadata
Priority        DWORD
Picture         WORD
XP              DWORD
EntryList       List
```

NWN-family entries contain `ID`, `Text`, and `End`; they do not contain KotOR's `XP_Percentage`. NeoJRL no longer adds that field when creating an NWN/NWN2 entry.

Entry IDs are unsigned 32-bit values and must be unique within their category. Deleting an entry does not renumber the remaining IDs.

## TLK behavior

NeoJRL can open and edit journal files without a TLK. Loading `dialog.tlk` is optional and resolves StrRef-backed names and entry text for preview and search. The TLK parser is supplied by NeoShared.

## Build

Clone NeoJRL and NeoShared as siblings:

```text
workspace/
  NeoShared/
  NeoJRL/
```

CMake automatically detects `../NeoShared` or `../neoshared`. A different checkout can be supplied with `--neoshared-root`, `-NeoSharedRoot`, or `-DNEOSHARED_ROOT=`.

Linux GUI build:

```sh
bash ./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)" --clean
```

Linux CLI/core-only build:

```sh
bash ./scripts/build.sh --wx OFF --jobs "$(nproc)" --clean
```

Windows GUI build:

```powershell
& ..\NeoShared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Clean `
  -Wx ON `
  -RequireWx ON `
  -NeoSharedRoot ..\NeoShared `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

## Import, export, and patch generation

The GUI and CLI support full hierarchical XML and JSON interchange. CSV/TSV flattened import is intentionally not exposed because it cannot preserve JRL/GFF structure.

TSLPatcher/HoloPatcher-oriented comparison output is available for representable scalar and field changes. Generic list-struct additions and structural deletions are reported as unsupported rather than emitted with fixed indexes that would be unsafe during installation.

## Shared game directories

**File > Open Game Directory** opens NeoJRL's file chooser at a saved game installation. The installation registry is shared through NeoShared.
