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

struct JournalEntryInsertResult {
    std::size_t index = 0;
    UInt32 entryId = 0;
    UInt32 structTypeId = 0;
};

JournalFlavor detectJournalFlavor(const GffFile& journal);
JournalFlavor detectJournalQuestFlavor(const GffFile& journal, std::size_t questIndex);
const char* journalFlavorDisplayName(JournalFlavor flavor);

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
