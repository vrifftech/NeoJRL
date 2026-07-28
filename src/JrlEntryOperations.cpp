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

GffStruct& requireEntry(GffFile& journal,
                        std::size_t questIndex,
                        std::size_t entryIndex) {
    GffStruct& quest = requireQuest(journal, questIndex);
    GffList* entries = findEntryList(quest);
    if (entries == nullptr || entryIndex >= entries->count()) {
        throw std::out_of_range("Entry index " + std::to_string(entryIndex) +
                                " is outside the selected quest's EntryList.");
    }
    GffStruct* entry = entries->GetStruct(entryIndex);
    if (entry == nullptr) throw std::runtime_error("The selected journal entry is missing its GFF struct.");
    return *entry;
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
        if (idField == nullptr) continue;
        if (idField->fieldtype != FIELD_TYPE_DWORD) {
            throw std::runtime_error("EntryList entry " + std::to_string(index) +
                                     " has an ID field with the wrong GFF type.");
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

JournalFlavor combineFlavor(JournalFlavor left, JournalFlavor right) {
    if (left == JournalFlavor::Unknown) return right;
    if (right == JournalFlavor::Unknown) return left;
    if (left == right) return left;
    return JournalFlavor::Mixed;
}

JournalFlavor flavorFromQuest(const GffStruct& quest) {
    bool kotor = quest.GetFieldByLabel("PlotIndex") != nullptr ||
                 quest.GetFieldByLabel("PlanetID") != nullptr;
    bool nwn = quest.GetFieldByLabel("XP") != nullptr;
    const GffList* entries = findEntryList(quest);
    if (entries != nullptr) {
        for (std::size_t index = 0; index < entries->count(); ++index) {
            const GffStruct* entry = entries->GetStruct(index);
            if (entry != nullptr && entry->GetFieldByLabel("XP_Percentage") != nullptr) {
                kotor = true;
                break;
            }
        }
    }
    if (kotor && nwn) return JournalFlavor::Mixed;
    if (kotor) return JournalFlavor::Kotor;
    if (nwn) return JournalFlavor::NeverwinterNights;
    return JournalFlavor::Unknown;
}

template <typename FieldType, typename ValueType>
void setOptionalScalar(GffStruct& structure,
                       const std::string& label,
                       std::optional<ValueType> value,
                       std::uint32_t expectedType) {
    GffField* field = structure.GetFieldByLabel(label);
    if (!value) {
        if (field != nullptr) structure.DeleteField(label);
        return;
    }
    if (field == nullptr) {
        structure.AddField(std::make_unique<FieldType>(label, *value));
        return;
    }
    if (field->fieldtype != expectedType) {
        throw std::runtime_error("Journal field " + label + " has the wrong GFF type.");
    }
    static_cast<FieldType&>(*field).value = *value;
}

} // namespace

JournalFlavor detectJournalQuestFlavor(const GffFile& journal, std::size_t questIndex) {
    return flavorFromQuest(requireQuest(journal, questIndex));
}

JournalFlavor detectJournalFlavor(const GffFile& journal) {
    const GffList& categories = requireCategories(journal);
    JournalFlavor result = JournalFlavor::Unknown;
    for (std::size_t index = 0; index < categories.count(); ++index) {
        const GffStruct* quest = categories.GetStruct(index);
        if (quest == nullptr) continue;
        result = combineFlavor(result, flavorFromQuest(*quest));
        if (result == JournalFlavor::Mixed) break;
    }
    return result;
}

const char* journalFlavorDisplayName(JournalFlavor flavor) {
    switch (flavor) {
    case JournalFlavor::Kotor: return "KotOR / KotOR II";
    case JournalFlavor::NeverwinterNights: return "Neverwinter Nights / NWN2";
    case JournalFlavor::Mixed: return "Mixed journal schema";
    case JournalFlavor::Unknown: return "Unknown journal schema";
    }
    return "Unknown journal schema";
}

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

    JournalFlavor flavor = flavorFromQuest(quest);
    if (flavor == JournalFlavor::Unknown) flavor = detectJournalFlavor(journal);
    if (flavor == JournalFlavor::Kotor || flavor == JournalFlavor::Mixed) {
        entry->AddField(std::make_unique<GffFloatField>("XP_Percentage", 0.0f));
    }

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
    if (field == nullptr) throw std::runtime_error("The selected quest has no entries to delete.");
    if (field->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("The selected quest has an EntryList field with the wrong GFF type.");
    }
    auto& entries = static_cast<GffList&>(*field);
    if (entryIndex >= entries.count()) {
        throw std::out_of_range("Entry index " + std::to_string(entryIndex) +
                                " is outside the selected quest's EntryList.");
    }
    entries.DeleteStruct(static_cast<UInt32>(entryIndex));
    journal.dirty(true);
    if (entries.count() == 0) return std::nullopt;
    return std::min(entryIndex, entries.count() - 1u);
}

void setJournalQuestString(GffFile& journal,
                           std::size_t questIndex,
                           const std::string& label,
                           const std::string& value) {
    GffStruct& quest = requireQuest(journal, questIndex);
    GffField* field = quest.GetFieldByLabel(label);
    if (field == nullptr) {
        quest.AddField(std::make_unique<GffExoStringField>(label, value));
    } else {
        if (field->fieldtype != FIELD_TYPE_CEXOSTRING) {
            throw std::runtime_error("Journal field " + label + " has the wrong GFF type.");
        }
        static_cast<GffExoStringField&>(*field).SetString(value);
    }
    journal.dirty(true);
}

void setJournalQuestOptionalString(GffFile& journal,
                                   std::size_t questIndex,
                                   const std::string& label,
                                   const std::optional<std::string>& value) {
    GffStruct& quest = requireQuest(journal, questIndex);
    GffField* field = quest.GetFieldByLabel(label);
    if (!value) {
        if (field != nullptr) quest.DeleteField(label);
        journal.dirty(true);
        return;
    }
    if (field == nullptr) {
        quest.AddField(std::make_unique<GffExoStringField>(label, *value));
    } else {
        if (field->fieldtype != FIELD_TYPE_CEXOSTRING) {
            throw std::runtime_error("Journal field " + label + " has the wrong GFF type.");
        }
        static_cast<GffExoStringField&>(*field).SetString(*value);
    }
    journal.dirty(true);
}

void setJournalQuestLocalizedString(GffFile& journal,
                                    std::size_t questIndex,
                                    const std::string& label,
                                    UInt32 strref,
                                    const std::optional<std::string>& language0Text) {
    GffStruct& quest = requireQuest(journal, questIndex);
    GffField* field = quest.GetFieldByLabel(label);
    if (field == nullptr) {
        auto localized = std::make_unique<GffLocalizedStringField>(label, strref);
        if (language0Text) localized->SetStringByID(0, *language0Text);
        quest.AddField(std::move(localized));
    } else {
        if (field->fieldtype != FIELD_TYPE_CEXOLOCSTRING) {
            throw std::runtime_error("Journal field " + label + " has the wrong GFF type.");
        }
        auto& localized = static_cast<GffLocalizedStringField&>(*field);
        localized.strref = strref;
        if (language0Text) localized.SetStringByID(0, *language0Text);
    }
    journal.dirty(true);
}

void setJournalQuestOptionalInt(GffFile& journal,
                                std::size_t questIndex,
                                const std::string& label,
                                std::optional<std::int32_t> value) {
    setOptionalScalar<GffIntField>(requireQuest(journal, questIndex), label, value, FIELD_TYPE_INT);
    journal.dirty(true);
}

void setJournalQuestOptionalDword(GffFile& journal,
                                  std::size_t questIndex,
                                  const std::string& label,
                                  std::optional<UInt32> value) {
    setOptionalScalar<GffUInt32Field>(requireQuest(journal, questIndex), label, value, FIELD_TYPE_DWORD);
    journal.dirty(true);
}

void setJournalQuestOptionalWord(GffFile& journal,
                                 std::size_t questIndex,
                                 const std::string& label,
                                 std::optional<std::uint16_t> value) {
    setOptionalScalar<GffWordField>(requireQuest(journal, questIndex), label, value, FIELD_TYPE_WORD);
    journal.dirty(true);
}

void changeJournalEntryId(GffFile& journal,
                          std::size_t questIndex,
                          std::size_t entryIndex,
                          UInt32 newEntryId) {
    GffStruct& quest = requireQuest(journal, questIndex);
    const GffList* entries = findEntryList(quest);
    if (entries == nullptr || entryIndex >= entries->count()) {
        throw std::out_of_range("Entry index is outside the selected quest's EntryList.");
    }
    for (std::size_t index = 0; index < entries->count(); ++index) {
        if (index == entryIndex) continue;
        const GffStruct* entry = entries->GetStruct(index);
        if (entry == nullptr) continue;
        const GffField* field = entry->GetFieldByLabel("ID");
        if (field != nullptr && field->fieldtype == FIELD_TYPE_DWORD &&
            static_cast<const GffUInt32Field&>(*field).value == newEntryId) {
            throw std::runtime_error("Entry ID " + std::to_string(newEntryId) +
                                     " is already used in the selected quest.");
        }
    }
    setOptionalScalar<GffUInt32Field>(requireEntry(journal, questIndex, entryIndex), "ID",
                                      std::optional<UInt32>{newEntryId}, FIELD_TYPE_DWORD);
    journal.dirty(true);
}

void setJournalEntryLocalizedString(GffFile& journal,
                                    std::size_t questIndex,
                                    std::size_t entryIndex,
                                    const std::string& label,
                                    UInt32 strref,
                                    const std::optional<std::string>& language0Text) {
    GffStruct& entry = requireEntry(journal, questIndex, entryIndex);
    GffField* field = entry.GetFieldByLabel(label);
    if (field == nullptr) {
        auto localized = std::make_unique<GffLocalizedStringField>(label, strref);
        if (language0Text) localized->SetStringByID(0, *language0Text);
        entry.AddField(std::move(localized));
    } else {
        if (field->fieldtype != FIELD_TYPE_CEXOLOCSTRING) {
            throw std::runtime_error("Journal entry field " + label + " has the wrong GFF type.");
        }
        auto& localized = static_cast<GffLocalizedStringField&>(*field);
        localized.strref = strref;
        if (language0Text) localized.SetStringByID(0, *language0Text);
    }
    journal.dirty(true);
}

void setJournalEntryDword(GffFile& journal,
                          std::size_t questIndex,
                          std::size_t entryIndex,
                          const std::string& label,
                          UInt32 value) {
    setOptionalScalar<GffUInt32Field>(requireEntry(journal, questIndex, entryIndex), label,
                                      std::optional<UInt32>{value}, FIELD_TYPE_DWORD);
    journal.dirty(true);
}

void setJournalEntryWord(GffFile& journal,
                         std::size_t questIndex,
                         std::size_t entryIndex,
                         const std::string& label,
                         std::uint16_t value) {
    setOptionalScalar<GffWordField>(requireEntry(journal, questIndex, entryIndex), label,
                                    std::optional<std::uint16_t>{value}, FIELD_TYPE_WORD);
    journal.dirty(true);
}

void setJournalEntryOptionalFloat(GffFile& journal,
                                  std::size_t questIndex,
                                  std::size_t entryIndex,
                                  const std::string& label,
                                  std::optional<float> value) {
    setOptionalScalar<GffFloatField>(requireEntry(journal, questIndex, entryIndex), label, value, FIELD_TYPE_FLOAT);
    journal.dirty(true);
}

} // namespace neojrl
