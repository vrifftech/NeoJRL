#include "JrlEntryOperations.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace neojrl {
namespace {

std::string normalizedType(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

void requireEditableJournal(const GffFile& journal) {
    if (!journal.loaded()) {
        throw std::runtime_error("No journal file is loaded.");
    }
    if (journal.isGff4()) {
        throw std::runtime_error("NeoJRL entry editing requires a canonical GFF3 JRL file.");
    }
    if (normalizedType(journal.filetype()) != "JRL") {
        throw std::runtime_error("The loaded GFF is not a JRL document.");
    }
}

GffList& requireCategories(GffFile& journal) {
    requireEditableJournal(journal);
    GffField* field = journal.GetFieldByLabel("Categories");
    if (field == nullptr || field->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("The JRL does not contain a canonical Categories list.");
    }
    return static_cast<GffList&>(*field);
}

const GffList& requireCategories(const GffFile& journal) {
    requireEditableJournal(journal);
    const GffField* field = journal.GetFieldByLabel("Categories");
    if (field == nullptr || field->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("The JRL does not contain a canonical Categories list.");
    }
    return static_cast<const GffList&>(*field);
}

GffStruct& requireQuest(GffFile& journal, std::size_t questIndex) {
    GffList& categories = requireCategories(journal);
    if (questIndex >= categories.count()) {
        throw std::out_of_range("Quest index " + std::to_string(questIndex) + " is outside the Categories list.");
    }
    GffStruct* quest = categories.GetStruct(questIndex);
    if (quest == nullptr) {
        throw std::runtime_error("The selected quest is missing its GFF struct.");
    }
    return *quest;
}

const GffStruct& requireQuest(const GffFile& journal, std::size_t questIndex) {
    const GffList& categories = requireCategories(journal);
    if (questIndex >= categories.count()) {
        throw std::out_of_range("Quest index " + std::to_string(questIndex) + " is outside the Categories list.");
    }
    const GffStruct* quest = categories.GetStruct(questIndex);
    if (quest == nullptr) {
        throw std::runtime_error("The selected quest is missing its GFF struct.");
    }
    return *quest;
}

const GffList* findEntryList(const GffStruct& quest) {
    const GffField* field = quest.GetFieldByLabel("EntryList");
    if (field == nullptr) return nullptr;
    if (field->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("The selected quest has an EntryList field with the wrong GFF type.");
    }
    return &static_cast<const GffList&>(*field);
}

GffList* findEntryList(GffStruct& quest) {
    GffField* field = quest.GetFieldByLabel("EntryList");
    if (field == nullptr) return nullptr;
    if (field->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("The selected quest has an EntryList field with the wrong GFF type.");
    }
    return &static_cast<GffList&>(*field);
}

std::unordered_set<UInt32> collectEntryIds(const GffList* entries) {
    std::unordered_set<UInt32> ids;
    if (entries == nullptr) return ids;
    for (std::size_t index = 0; index < entries->count(); ++index) {
        const GffStruct* entry = entries->GetStruct(index);
        if (entry == nullptr) {
            throw std::runtime_error("EntryList contains a missing struct at index " + std::to_string(index) + ".");
        }
        const GffField* idField = entry->GetFieldByLabel("ID");
        if (idField == nullptr) {
            // Some hand-authored journals omit ID. Preserve those entries and
            // choose a value that does not collide with the IDs that do exist.
            continue;
        }
        if (idField->fieldtype != FIELD_TYPE_DWORD) {
            throw std::runtime_error("EntryList entry " + std::to_string(index) + " has an ID field with the wrong GFF type.");
        }
        ids.insert(static_cast<const GffUInt32Field&>(*idField).value);
    }
    return ids;
}

UInt32 firstAvailableId(const std::unordered_set<UInt32>& ids) {
    UInt32 maximum = 0;
    bool haveAny = false;
    for (const UInt32 id : ids) {
        maximum = haveAny ? std::max(maximum, id) : id;
        haveAny = true;
    }
    if (!haveAny) return 0;
    if (maximum != std::numeric_limits<UInt32>::max()) {
        const UInt32 candidate = maximum + 1u;
        if (ids.find(candidate) == ids.end()) return candidate;
    }
    for (UInt32 candidate = 0;; ++candidate) {
        if (ids.find(candidate) == ids.end()) return candidate;
        if (candidate == std::numeric_limits<UInt32>::max()) break;
    }
    throw std::runtime_error("The selected quest has no available 32-bit entry IDs.");
}


} // namespace

UInt32 suggestJournalEntryId(const GffFile& journal, std::size_t questIndex) {
    const GffStruct& quest = requireQuest(journal, questIndex);
    return firstAvailableId(collectEntryIds(findEntryList(quest)));
}

JournalEntryInsertResult appendJournalEntry(
    GffFile& journal,
    std::size_t questIndex,
    std::optional<UInt32> requestedEntryId) {
    GffStruct& quest = requireQuest(journal, questIndex);
    GffList* entries = findEntryList(quest);
    const auto existingIds = collectEntryIds(entries);
    const UInt32 entryId = requestedEntryId.value_or(firstAvailableId(existingIds));
    if (existingIds.find(entryId) != existingIds.end()) {
        throw std::runtime_error("Entry ID " + std::to_string(entryId) + " is already used in the selected quest.");
    }

    const std::size_t newIndex = entries == nullptr ? 0 : entries->count();
    if (newIndex > static_cast<std::size_t>(std::numeric_limits<UInt32>::max())) {
        throw std::runtime_error("The selected quest has too many entries for the GFF list format.");
    }
    const UInt32 structTypeId = static_cast<UInt32>(newIndex);
    auto entry = std::make_unique<GffStruct>();
    entry->typeid_ = structTypeId;
    entry->AddField(std::make_unique<GffWordField>("End", 0u));
    entry->AddField(std::make_unique<GffUInt32Field>("ID", entryId));
    entry->AddField(std::make_unique<GffLocalizedStringField>("Text", std::numeric_limits<UInt32>::max()));
    entry->AddField(std::make_unique<GffFloatField>("XP_Percentage", 0.0f));

    if (entries == nullptr) {
        auto list = std::make_unique<GffList>("EntryList");
        entries = list.get();
        quest.AddField(std::move(list));
    }
    entries->AddStruct(std::move(entry));
    journal.dirty(true);
    return JournalEntryInsertResult{newIndex, entryId, structTypeId};
}

std::optional<std::size_t> deleteJournalEntry(
    GffFile& journal,
    std::size_t questIndex,
    std::size_t entryIndex) {
    GffStruct& quest = requireQuest(journal, questIndex);
    GffField* field = quest.GetFieldByLabel("EntryList");
    if (field == nullptr) {
        throw std::runtime_error("The selected quest has no entries to delete.");
    }
    if (field->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("The selected quest has an EntryList field with the wrong GFF type.");
    }
    auto& entries = static_cast<GffList&>(*field);
    if (entryIndex >= entries.count()) {
        throw std::out_of_range("Entry index " + std::to_string(entryIndex) + " is outside the selected quest's EntryList.");
    }

    if (entryIndex > static_cast<std::size_t>(std::numeric_limits<UInt32>::max())) {
        throw std::out_of_range("Entry index cannot be represented by the GFF list API.");
    }
    entries.DeleteStruct(static_cast<UInt32>(entryIndex));
    journal.dirty(true);
    if (entries.count() == 0) return std::nullopt;
    return std::min(entryIndex, entries.count() - 1u);
}

} // namespace neojrl
