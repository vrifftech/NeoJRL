# NeoJRL

[![CI](https://github.com/vrifftech/NeoJRL/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoJRL/actions/workflows/ci.yml)

NeoJRL is a purpose-built editor for BioWare journal files (`.jrl`). It supports the related but different journal formats used by:

- Star Wars: Knights of the Old Republic
- Star Wars: Knights of the Old Republic II
- Neverwinter Nights
- Neverwinter Nights 2

NeoJRL presents a journal as quests and journal states instead of requiring you to edit the underlying GFF structure by hand.

The simplest way to think about a JRL is:

```text
Journal file
`-- Quest or category
    |-- Entry 0: the quest begins
    |-- Entry 10: the player learns something new
    |-- Entry 20: the objective changes
    `-- Entry 30: the quest is complete
```

In KotOR and KotOR II, the file is named `global.jrl`. In Neverwinter Nights and Neverwinter Nights 2, it is commonly named `module.jrl`.

NeoJRL builds against the separate sibling [NeoShared](https://github.com/vrifftech/NeoShared) repository.

## Contents

- [Quick start](#quick-start)
- [How a journal works](#how-a-journal-works)
- [Quests, categories, entries, and IDs](#quests-categories-entries-and-ids)
- [Worked KotOR example](#worked-kotor-example)
- [KotOR and KotOR II fields](#kotor-and-kotor-ii-fields)
- [Neverwinter Nights and NWN2 fields](#neverwinter-nights-and-nwn2-fields)
- [Using the editor](#using-the-editor)
- [TLK files, StrRefs, and embedded text](#tlk-files-strrefs-and-embedded-text)
- [Adding and deleting entries](#adding-and-deleting-entries)
- [Search and filtering](#search-and-filtering)
- [Shared game directories](#shared-game-directories)
- [Import, export, and patching](#import-export-and-patching)
- [Building NeoJRL](#building-neojrl)
- [Raw field reference](#raw-field-reference)

## Quick start

1. Open `global.jrl` or `module.jrl`.
2. Optionally load the game's `dialog.tlk` so NeoJRL can show the text behind each StrRef.
3. Select a quest in the left-hand list.
4. Edit the quest information in the **Quest** panel and click **Apply Quest**.
5. Select a journal entry in the lower list.
6. Edit its ID, text, completion flag, or XP value and click **Apply Entry**.
7. Save the JRL.

If the JRL is stored inside a `.mod`, `.erf`, `.rim`, `.hak`, or another game archive, extract it with NeoERF first and put the edited file back into the archive afterward.

Make a backup before structural changes. Deleting an entry is immediate within the open document and NeoJRL does not currently provide an undo command.

## How a journal works

A journal file contains a collection of quests. BioWare's raw field name for that collection is `Categories`, so older tools and technical documentation often call each quest a **category**.

For normal editing, the terms can be read as:

```text
Category = quest
Entry    = journal state or stage within that quest
```

A quest does not have to advance through every entry in numeric order. Game scripts normally select a quest by its **Tag** and set it to a particular **Entry ID**.

For example:

```text
Quest tag: tar_missing_droid

Entry ID 0
    The player has heard that a droid is missing.

Entry ID 10
    The player discovered tracks near the landing pad.

Entry ID 20
    The player found the droid.

Entry ID 30
    The droid was returned. End = Yes.
```

A script can move directly from entry `0` to entry `20`. The list position of an entry is not its game-facing ID.

The JRL defines the possible quest states and their text. The player's save game records which state has actually been reached.

## Quests, categories, entries, and IDs

### Quest or category

A quest/category owns:

- A script-facing tag.
- A localized display name.
- Sorting and presentation information.
- Game-specific XP or grouping information.
- A list of journal entries.

### Journal entry

An entry represents one state of the quest. It normally contains:

- A unique numeric ID.
- Localized journal text.
- A completed/final-state flag.
- Game-specific XP information.

### Entry ID versus list position

These are different values.

```text
List position   Where the entry currently appears inside the file
Entry ID        The stable number used to select that journal state
```

Example:

```text
List position 0 -> Entry ID 10
List position 1 -> Entry ID 30
List position 2 -> Entry ID 100
```

Deleting the middle entry does not renumber the others. Their IDs remain `10` and `100`.

NeoJRL requires entry IDs to be unique within the selected quest.

## Example

Suppose a quest tracks a stolen artifact.

### Quest information

```text
Tag:               dan_stolen_artifact
Name:              The Stolen Artifact
Sort priority:     50
PlotXP.2da row:    12
Planet ID:         2
```

### Entry 0 — quest received

```text
Entry ID:          0
End:               No
Plot XP multiplier: 0
Text:              A merchant asked me to recover a stolen artifact.
```

### Entry 10 — clue found

```text
Entry ID:          10
End:               No
Plot XP multiplier: 0.25
Text:              I found evidence that the thieves went into the ruins.
```

### Entry 20 — artifact recovered

```text
Entry ID:          20
End:               No
Plot XP multiplier: 0.5
Text:              I recovered the artifact and should return it to the merchant.
```

### Entry 30 — quest complete

```text
Entry ID:          30
End:               Yes
Plot XP multiplier: 1.0
Text:              I returned the artifact and received my reward.
```

The exact XP outcome depends on the quest's `PlotIndex` row in `PlotXP.2da`. The multiplier is applied to that row's XP value.

## KotOR and KotOR II fields

NeoJRL detects KotOR-style quests and shows the fields used by both games.

### KotOR quest fields

#### Detected profile

Shows the schema NeoJRL detected for the selected quest. For these games it reads:

```text
KotOR / KotOR II
```

This field is informational and cannot be edited.

#### Name StrRef

The string number used to look up the quest title in `dialog.tlk`.

Use `-1` when the title should be stored directly in the JRL as embedded text.

#### Name text

Shows the quest title.

- When **Name StrRef** is `-1`, this box edits the embedded language-0 title stored in the JRL.
- When **Name StrRef** points to the TLK, this box is a resolved preview. Changes typed into the preview are not written to the JRL or `dialog.tlk`; edit the TLK with NeoTLK or choose a different StrRef.

#### Tag

The script-facing quest key. KotOR's runtime compares quest tags without regard to letter case.

Scripts use this value to find and update the quest, so changing it can break existing scripts or dialogue references.

#### Comment

Optional authoring notes. This is useful for modders and tool authors but is not the player's journal text.

#### Sort priority

An unsigned 32-bit value used to order quests in the journal. Higher values are sorted ahead of lower values.

#### Journal picture ID

An unsigned 16-bit, data-driven image or portrait identifier. NeoJRL validates the storage type but does not invent a smaller maximum than the format permits.

#### Planet ID

A signed grouping identifier used by KotOR's journal organization. `-1` is commonly used when no specific planet grouping is intended.

The exact meaning of each nonnegative value depends on the game data and mod.

#### PlotXP.2da row

A signed row number used by the plot-XP system. `-1` is a valid unset/default convention.

The actual XP value comes from the selected row in `PlotXP.2da`.

### KotOR entry fields

#### Entry ID

The unsigned 32-bit state number used within this quest.

It must be unique among the quest's entries. It does not have to match the row number, and IDs do not have to be consecutive.

#### Plot XP multiplier

A finite, nonnegative floating-point multiplier applied to the XP associated with the quest's `PlotXP.2da` row.

Examples:

```text
0      No plot XP from this journal state
0.25   25 percent
0.5    50 percent
1.0    100 percent
1.5    150 percent
```

This is a multiplier, not an integer percentage box.

#### End

Marks the entry as a final or completed journal state.

NeoJRL writes this as `0` or `1` even though the underlying GFF stores it in a `WORD` field.

#### StrRef

The string number used to look up the entry text in `dialog.tlk`.

Use `-1` to store the text directly in the JRL.

#### Entry Text

Shows the selected entry's journal text.

- With StrRef `-1`, this edits the embedded language-0 text.
- With a valid TLK StrRef, this is a resolved preview. Changes typed into the preview are not written to the JRL or TLK; edit the TLK with NeoTLK or choose a different StrRef.

## Neverwinter Nights and NWN2 fields

The supplied NWN and NWN2 journal resources use a related but different schema. NeoJRL detects that schema per quest and changes the visible controls.

No symbolized NWN or NWN2 executable was used for the current semantic audit, so the behavior described here is based on real `module.jrl` samples and their observed field structure.

### NWN/NWN2 quest fields

#### Name StrRef and Name text

These work like the KotOR controls: the StrRef points to a TLK entry, while `-1` allows embedded language-0 text.

#### Tag

The script-facing category key.

#### Comment

Authoring notes stored with the quest/category.

#### Sort priority

An unsigned 32-bit ordering value.

#### Journal picture ID

An unsigned 16-bit, data-driven picture identifier.

#### Quest XP

An unsigned 32-bit XP value stored on the quest/category itself.

This is the key schema difference from KotOR:

```text
KotOR / KotOR II
    XP information is stored per entry as XP_Percentage.

NWN / NWN2
    XP is stored on the quest/category as XP.
```

NeoJRL therefore hides **Plot XP multiplier**, **PlotXP.2da row**, and **Planet ID** for NWN-family quests.

### NWN/NWN2 entry fields

NWN-family entries contain:

- Entry ID.
- Entry text.
- End/final-state flag.

They do not contain KotOR's `XP_Percentage` field. When you create a new NWN or NWN2 entry, NeoJRL does not add that KotOR-only field.

## Using the editor

### Document tabs

Each open JRL appears in its own tab. You can open several journal files at once and switch between them with the tab strip or the keyboard shortcuts in the **File** menu.

An asterisk in a tab title indicates unsaved changes.

### Journal area

#### JRL file

Shows the path of the active journal. Use **Open**, **Save**, or **Save As** to manage the file.

#### dialog.tlk

Shows the optional TLK currently used for resolving StrRefs.

Loading a TLK changes previews and search results. It does not modify the JRL or the TLK.

#### Search/filter

Lets you search by:

- Quest Tag.
- Quest Name.
- Quest Stage Text.
- Planet ID.
- Priority.
- Plot Index.
- Picture.
- Quest XP.

**Search** moves to the next matching quest or entry and wraps around to the beginning.

For quest-level modes, **Tools > Apply Search as Quest Filter** hides quests that do not match the current field. **Quest Stage Text** is intended for Find Next-style searching because the matching text belongs to entries rather than the quest row itself. Clear filters from the **Tools** menu when you want the complete list again.

### Quest list

The left-hand list shows the quests/categories in the active JRL.

The first column is the physical list position. The second is the quest tag.

Selecting a quest loads its fields and its entry list on the right.

### Quest panel

Edit the selected quest and click **Apply Quest**. Applying changes updates the in-memory document; it does not save the file to disk.

The visible fields change according to the detected profile:

```text
KotOR profile
    Planet ID
    PlotXP.2da row
    Entry-level Plot XP multiplier

NWN profile
    Quest XP
```

### Entries list

The lower-left list shows the selected quest's entries.

The `#` column is the list position. The `ID` column is the game-facing entry ID.

Use **New Entry** to append an entry and **Delete Entry** to remove the selected one.

### Selected Entry panel

Edit the selected state and click **Apply Entry**. As with quest changes, use **Save** afterward to write the document to disk.

## TLK files, StrRefs, and embedded text

BioWare journals normally store text in a `CExoLocString`. That value can contain:

- A StrRef pointing to the game's TLK.
- Embedded language-specific text.
- Both, depending on the resource.

### StrRef-backed text

Example:

```text
StrRef: 12345
```

With `dialog.tlk` loaded, NeoJRL can display string 12345. The text belongs to the TLK, not to the JRL.

To change that text globally, edit the TLK with NeoTLK or point the JRL at a different StrRef.

### Embedded text

Example:

```text
StrRef: -1
```

NeoJRL stores the entered language-0 text directly in the JRL.

This is convenient for a self-contained mod but does not automatically provide translations for other languages.

### Loading the TLK is optional

Without a TLK, NeoJRL can still:

- Open and save JRL files.
- Edit StrRef numbers.
- Edit embedded text.
- Add and delete entries.

The TLK is only needed for resolved previews and text-based searching of TLK-backed strings.

## Adding and deleting entries

### New Entry

NeoJRL appends a new entry to the selected quest and chooses an unused entry ID. Normally it uses one greater than the highest existing ID.

For example, if the quest contains IDs:

```text
0, 1, 3
```

NeoJRL normally suggests:

```text
4
```

If the highest 32-bit ID cannot be incremented, NeoJRL searches for another free value.

The new entry uses the selected quest's detected schema:

- KotOR/KotOR II entries receive `XP_Percentage`.
- NWN/NWN2 entries do not.

You may change the ID before saving, but it must remain unique within that quest.

### Delete Entry

Deleting removes the selected entry structure from the quest.

NeoJRL does not renumber the remaining entry IDs. Scripts that refer to those IDs continue to use their existing values.

Deletion cannot currently be undone. Save a backup or use version control before restructuring a journal.

### Creating or deleting entire quests

The current semantic interface edits existing quests and manages their entries. It does not yet provide **New Quest** or **Delete Quest** commands.

For structural category-level work, use NeoGFF or a reviewed XML/JSON workflow, then reopen the result in NeoJRL for semantic editing.

## Search and filtering

NeoJRL provides two related tools.

### Search

Search selects the next match without hiding anything. Repeating the command moves to the next result and wraps around.

When searching **Quest Stage Text**, NeoJRL checks every entry in every quest and selects both the matching quest and entry.

### Filters

Filters temporarily hide rows that do not match.

You can:

- Apply the current search as a quest filter.
- Right-click a quest-list or entry-list column header to filter that column.
- Clear one column filter.
- Clear all filters.

A column label ending with `*` has an active filter.

Filtering does not delete or modify journal data.

## Shared game directories

**File > Open Game Directory** shows the installations saved by NeoShared and opens NeoJRL's file chooser at the selected installation directory.

The installation registry is shared with the other Neo tools.

## Import, export, and patching

### XML and JSON

NeoJRL supports hierarchical XML and JSON interchange that preserves the complete JRL/GFF structure.

Use these formats for:

- Reviewing a journal in text form.
- Version-control diffs.
- Scripted transformations.
- Moving values between tools that understand the same hierarchy.

### Clipboard copy and paste

**Copy Selection** copies the selected quest and, when applicable, its selected entry as tab-separated field rows.

**Paste Values** applies compatible field values back to the quest and entry indexes named in the pasted table.

This is an advanced operation. Review pasted indexes and fields carefully, especially after structural changes.

### TSLPatcher and HoloPatcher output

NeoJRL can compare an original JRL with the modified document and export either:

- A complete patch package.
- A patch fragment.


## Building NeoJRL

Place NeoJRL and NeoShared beside one another. The default lookup expects the shared checkout to be named `neoshared`:

```text
workspace/
|-- neoshared/
`-- NeoJRL/
```

A checkout with another name or location can be supplied explicitly.

### Linux GUI build

Install CMake, a C++17 compiler, and the wxWidgets GTK development package, then run:

```sh
bash ./scripts/build.sh \
  --neoshared-root ../neoshared \
  --wx ON \
  --require-wx ON \
  --jobs "$(nproc)" \
  --clean
```

### Linux CLI/core-only build

```sh
bash ./scripts/build.sh \
  --neoshared-root ../neoshared \
  --wx OFF \
  --jobs "$(nproc)" \
  --clean
```

### Windows GUI build

Install the pinned wxWidgets dependency through NeoShared:

```powershell
& ..\NeoShared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild
```

Then build NeoJRL:

```powershell
.\scripts\build.ps1 `
  -Clean `
  -Wx ON `
  -RequireWx ON `
  -NeoSharedRoot ..\NeoShared `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

### Direct CMake configuration

```sh
cmake -S . -B build \
  -DNEOSHARED_ROOT=../neoshared \
  -DNEOJRL_BUILD_WX_GUI=ON \
  -DNEOJRL_REQUIRE_WX_GUI=ON

cmake --build build --parallel
```

## Raw field reference

This section maps the editor's controls to the underlying GFF labels.

### KotOR / KotOR II quest structure

```text
JRL V3.2
`-- Categories
    `-- Category
        |-- Tag             CExoString
        |-- Name            CExoLocString
        |-- Priority        DWORD
        |-- Picture         WORD
        |-- PlotIndex       INT
        |-- PlanetID        INT
        `-- EntryList
            `-- Entry
                |-- ID              DWORD
                |-- Text            CExoLocString
                |-- XP_Percentage   FLOAT
                `-- End             WORD used as Boolean
```

| NeoJRL label | Raw GFF field | Meaning |
|---|---|---|
| Name StrRef / Name text | `Name` | Localized quest title |
| Tag | `Tag` | Script-facing quest key |
| Sort priority | `Priority` | Higher values sort first |
| Journal picture ID | `Picture` | Data-driven picture identifier |
| PlotXP.2da row | `PlotIndex` | Plot-XP table row |
| Planet ID | `PlanetID` | Journal grouping identifier |
| Entry ID | `ID` | Quest-local journal state number |
| StrRef / Entry Text | `Text` | Localized entry text |
| Plot XP multiplier | `XP_Percentage` | Multiplier applied to plot XP |
| End | `End` | Final/completed state flag |

### Neverwinter Nights / NWN2 quest structure

```text
JRL V3.2
`-- Categories
    `-- Category
        |-- Name       CExoLocString
        |-- Tag        CExoString
        |-- Comment    CExoString
        |-- Priority   DWORD
        |-- Picture    WORD
        |-- XP         DWORD
        `-- EntryList
            `-- Entry
                |-- ID      DWORD
                |-- Text    CExoLocString
                `-- End     WORD used as Boolean
```

| NeoJRL label | Raw GFF field | Meaning |
|---|---|---|
| Name StrRef / Name text | `Name` | Localized quest/category title |
| Tag | `Tag` | Script-facing category key |
| Comment | `Comment` | Authoring note |
| Sort priority | `Priority` | Ordering value |
| Journal picture ID | `Picture` | Data-driven picture identifier |
| Quest XP | `XP` | Category-level XP value |
| Entry ID | `ID` | Category-local journal state number |
| StrRef / Entry Text | `Text` | Localized entry text |
| End | `End` | Final/completed state flag |
