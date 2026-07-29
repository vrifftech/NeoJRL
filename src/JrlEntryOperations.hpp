#pragma once

#include "GFFFile.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neojrl {

enum class JournalFlavor {
    Unknown,
    Kotor,
    NeverwinterNights,
    Mixed,
};

struct JournalQuestInsertResult {
    std::size_t index = 0;
    UInt32 structTypeId = 0;
    std::string tag;
    JournalFlavor flavor = JournalFlavor::Unknown;
};

struct JournalEntryInsertResult {
    std::size_t index = 0;
    UInt32 entryId = 0;
    UInt32 structTypeId = 0;
};

JournalFlavor detectJournalFlavor(const GffFile& journal);
JournalFlavor detectJournalQuestFlavor(const GffFile& journal, std::size_t questIndex);
const char* journalFlavorDisplayName(JournalFlavor flavor);

// Initializes a new canonical JRL document with an empty Categories list.
void initializeJournal(GffFile& journal);

// Returns a case-insensitively unique placeholder tag for a new quest.
std::string suggestJournalQuestTag(const GffFile& journal);

// Appends a new quest/category using the selected game-family schema.
JournalQuestInsertResult appendJournalQuest(
    GffFile& journal,
    JournalFlavor flavor,
    std::optional<std::string> requestedTag = std::nullopt,
    const std::string& initialName = "New Quest");

// Removes one quest/category and returns the quest index that should be
// selected afterwards, or std::nullopt when the journal becomes empty.
std::optional<std::size_t> deleteJournalQuest(
    GffFile& journal,
    std::size_t questIndex);

// Updates the script-facing quest tag while enforcing case-insensitive
// uniqueness across the complete journal.
void changeJournalQuestTag(GffFile& journal,
                           std::size_t questIndex,
                           const std::string& newTag);

// Returns a quest-local entry ID that is not currently used. IDs are not
// renumbered when entries are removed.
UInt32 suggestJournalEntryId(const GffFile& journal, std::size_t questIndex);

// Appends an EntryList struct appropriate for the selected journal dialect.
// KotOR entries receive XP_Percentage; NWN/NWN2 entries do not.
JournalEntryInsertResult appendJournalEntry(
    GffFile& journal,
    std::size_t questIndex,
    std::optional<UInt32> requestedEntryId = std::nullopt);

// Removes one EntryList struct and returns the entry index that should be
// selected afterwards, or std::nullopt when the quest no longer has entries.
std::optional<std::size_t> deleteJournalEntry(
    GffFile& journal,
    std::size_t questIndex,
    std::size_t entryIndex);

void setJournalQuestString(GffFile& journal,
                           std::size_t questIndex,
                           const std::string& label,
                           const std::string& value);
void setJournalQuestOptionalString(GffFile& journal,
                                   std::size_t questIndex,
                                   const std::string& label,
                                   const std::optional<std::string>& value);
void setJournalQuestLocalizedString(GffFile& journal,
                                    std::size_t questIndex,
                                    const std::string& label,
                                    UInt32 strref,
                                    const std::optional<std::string>& language0Text);
void setJournalQuestOptionalInt(GffFile& journal,
                                std::size_t questIndex,
                                const std::string& label,
                                std::optional<std::int32_t> value);
void setJournalQuestOptionalDword(GffFile& journal,
                                  std::size_t questIndex,
                                  const std::string& label,
                                  std::optional<UInt32> value);
void setJournalQuestOptionalWord(GffFile& journal,
                                 std::size_t questIndex,
                                 const std::string& label,
                                 std::optional<std::uint16_t> value);

void changeJournalEntryId(GffFile& journal,
                          std::size_t questIndex,
                          std::size_t entryIndex,
                          UInt32 newEntryId);
void setJournalEntryLocalizedString(GffFile& journal,
                                    std::size_t questIndex,
                                    std::size_t entryIndex,
                                    const std::string& label,
                                    UInt32 strref,
                                    const std::optional<std::string>& language0Text);

void setJournalEntryDword(GffFile& journal,
                          std::size_t questIndex,
                          std::size_t entryIndex,
                          const std::string& label,
                          UInt32 value);
void setJournalEntryWord(GffFile& journal,
                         std::size_t questIndex,
                         std::size_t entryIndex,
                         const std::string& label,
                         std::uint16_t value);
void setJournalEntryOptionalFloat(GffFile& journal,
                                  std::size_t questIndex,
                                  std::size_t entryIndex,
                                  const std::string& label,
                                  std::optional<float> value);

} // namespace neojrl
