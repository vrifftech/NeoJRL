#pragma once

#include "GFFFile.hpp"

#include <cstddef>
#include <optional>

namespace neojrl {

struct JournalEntryInsertResult {
    std::size_t index = 0;
    UInt32 entryId = 0;
    UInt32 structTypeId = 0;
};

// Returns a quest-local entry ID that is not currently used. IDs are not
// renumbered when entries are removed.
UInt32 suggestJournalEntryId(const GffFile& journal, std::size_t questIndex);

// Appends a canonical JRL EntryList struct. When requestedEntryId is omitted,
// the next available quest-local ID is selected automatically.
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

} // namespace neojrl
