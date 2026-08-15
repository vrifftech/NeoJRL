#include "GFFFile.hpp"
#include "GffTypeNames.hpp"
#include "GffXml.hpp"
#include "JrlEntryOperations.hpp"
#include <neotlk/TlkLookup.hpp>
#include "TabularData.hpp"
#include "GffJson.hpp"
#include "TslPatcher.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <string>

using namespace neojrl;

namespace {


std::string lowerAsciiLocal(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open input text file: " + file.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Unable to open output text file: " + file.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("Unable to write output text file: " + file.string());
}

void printTableTsv(const neotabular::Table& table) {
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        if (c) std::cout << '\t';
        std::cout << table.columns[c];
    }
    std::cout << '\n';
    for (const auto& row : table.rows) {
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            if (c) std::cout << '\t';
            if (c < row.size()) std::cout << row[c];
        }
        std::cout << '\n';
    }
}

std::string joinPath(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    if (child.empty()) return parent;
    return parent + "\\" + child;
}

void appendGffFieldRows(neotabular::Table& table, const GffField& field, const std::string& path);

void appendGffStructRows(neotabular::Table& table, const GffStruct& structure, const std::string& path) {
    for (const auto& field : structure.allFields()) {
        if (field) appendGffFieldRows(table, *field, joinPath(path, field->GetLabel()));
    }
}

void appendGffFieldRows(neotabular::Table& table, const GffField& field, const std::string& path) {
    const UInt32 type = field.fieldtype;
    table.rows.push_back({path, field.GetLabel(), gffFieldTypeName(type), gffFieldTypeEditable(type) ? "yes" : "no", field.GetString()});
    if (type == FIELD_TYPE_STRUCT) {
        appendGffStructRows(table, static_cast<const GffStruct&>(field), path);
    } else if (type == FIELD_TYPE_LIST) {
        const auto& list = static_cast<const GffList&>(field);
        for (std::size_t i = 0; i < list.count(); ++i) {
            const GffStruct* structure = list.GetStruct(i);
            if (!structure) continue;
            const std::string childPath = joinPath(path, std::to_string(i));
            table.rows.push_back({childPath, "[" + std::to_string(i) + "]", "Struct", "no", structure->GetString()});
            appendGffStructRows(table, *structure, childPath);
        }
    } else if (type == FIELD_TYPE_CEXOLOCSTRING) {
        const auto& loc = static_cast<const GffLocalizedStringField&>(field);
        table.rows.push_back({path + "(strref)", field.GetLabel() + " strref", "CExoLocString StrRef", "yes", loc.strref == 0xffffffffu ? std::string("-1") : std::to_string(loc.strref)});
        for (const auto& sub : loc.substrings) {
            table.rows.push_back({path + "(lang" + std::to_string(sub.stringid) + ")", field.GetLabel() + " lang" + std::to_string(sub.stringid), "CExoLocString Text", "yes", sub.GetString()});
        }
    }
}

neotabular::Table gffToTable(const GffFile& gff) {
    neotabular::Table table;
    table.columns = {"Path", "Label", "Type", "Editable", "Value"};
    const GffStruct* root = gff.root();
    if (root != nullptr) appendGffStructRows(table, *root, "");
    return table;
}


struct PatchOutputOptions {
    bool package = true;
    bool allowUnsupported = false;
    std::string patchFilename;
    std::string modifiedFormat = "auto";
    std::filesystem::path iniFilename = "changes.ini";
};

PatchOutputOptions parsePatchOutputOptions(int argc, char** argv, int begin, const std::filesystem::path& original) {
    PatchOutputOptions options;
    options.patchFilename = neotsl::basenameForPatch(original);
    for (int i = begin; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--package") options.package = true;
        else if (arg == "--fragment") options.package = false;
        else if (arg == "--filename") {
            if (i + 1 >= argc) throw std::runtime_error("--filename requires a value.");
            options.patchFilename = argv[++i];
        } else if (arg == "--ini") {
            if (i + 1 >= argc) throw std::runtime_error("--ini requires a filename.");
            options.iniFilename = argv[++i];
        } else if (arg == "--allow-unsupported") options.allowUnsupported = true;
        else if (arg == "--modified-format" || arg == "--input-format") {
            if (i + 1 >= argc) throw std::runtime_error(arg + " requires a value.");
            options.modifiedFormat = argv[++i];
        }
        else throw std::runtime_error("Unknown --diff-tslpatcher option: " + arg);
    }
    return options;
}

void writePatchOutput(const neotsl::PatchProject& project, const std::filesystem::path& output, const PatchOutputOptions& options) {
    if (!options.allowUnsupported) neotsl::throwIfUnsupported(project);
    else neotsl::printReport(project);
    if (options.package) {
        const std::filesystem::path iniPath = options.iniFilename.is_absolute()
            ? options.iniFilename
            : output / options.iniFilename;
        neotsl::writePackageToIni(project, iniPath, true);
    } else {
        neotsl::writeFragment(project, output);
    }
}

std::string extensionImportFormat(const std::filesystem::path& path) {
    std::string ext = lowerAsciiLocal(path.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    if (ext == "csv" || ext == "tsv" || ext == "xml" || ext == "json") return ext;
    return "native";
}

const GffList& requireCategories(const GffFile& gff);

bool isNativeGffImportFormat(std::string formatName) {
    formatName = lowerAsciiLocal(std::move(formatName));
    return formatName == "native" || formatName == "kotor" || formatName == "jrl" || formatName == "gff";
}

void loadGffFromImport(GffFile& gff,
                       const std::filesystem::path& originalPath,
                       const std::filesystem::path& inputPath,
                       std::string formatName) {
    (void)originalPath;
    formatName = lowerAsciiLocal(std::move(formatName));
    if (formatName.empty() || formatName == "auto") formatName = extensionImportFormat(inputPath);

    if (isNativeGffImportFormat(formatName)) {
        gff.LoadFile(inputPath);
        return;
    }

    const auto format = neotabular::parseFormat(formatName);
    if (format == neotabular::Format::Xml) {
        LoadGffXml(gff, readTextFile(inputPath));
    } else if (format == neotabular::Format::Json) {
        LoadGffXml(gff, gffJsonToXml(readTextFile(inputPath)));
    } else {
        throw std::runtime_error("NeoJRL supports XML/JSON or native JRL/GFF import for patcher generation; CSV/TSV flattened imports are not supported.");
    }
}

std::string normalizedJrlType(std::string type) {
    type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), type.end());
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return type;
}

void requireJrlPatcherInput(const GffFile& gff, const std::string& role) {
    if (!gff.loaded()) throw std::runtime_error(role + " is not loaded.");
    if (gff.isGff4() || normalizedJrlType(gff.version()) != "V3.2") {
        throw std::runtime_error(
            role + " is not a classic JRL V3.2 document supported by original TSLPatcher and HoloPatcher 1.7.");
    }
    if (normalizedJrlType(gff.filetype()) != "JRL") {
        throw std::runtime_error(role + " is not a JRL file.");
    }
    (void)requireCategories(gff);
    const auto flavor = neojrl::detectJournalFlavor(gff);
    if (flavor != neojrl::JournalFlavor::Kotor) {
        throw std::runtime_error(
            role + " uses the " + std::string(neojrl::journalFlavorDisplayName(flavor)) +
            " journal schema. NeoJRL patcher export is limited to KotOR and KotOR II global.jrl files; distribute NWN/NWN2 journals as complete files.");
    }
}

void requireMatchingJrlPatchDocuments(const GffFile& original, const GffFile& modified) {
    requireJrlPatcherInput(original, "The original patch baseline");
    requireJrlPatcherInput(modified, "The modified patch input");
    if (original.version() != modified.version()) {
        throw std::runtime_error("The original and modified JRL versions do not match.");
    }
}

int diffTslPatcher(const std::filesystem::path& originalPath, const std::filesystem::path& modifiedPath, const std::filesystem::path& output, const PatchOutputOptions& options) {
    GffFile original;
    original.LoadFile(originalPath);
    GffFile modified;
    loadGffFromImport(modified, originalPath, modifiedPath, options.modifiedFormat);
    requireMatchingJrlPatchDocuments(original, modified);
    auto project = neotsl::diffGffFlatTable(gffToTable(original), gffToTable(modified), options.patchFilename, options.package, originalPath);
    writePatchOutput(project, output, options);
    return 0;
}

int exportGff(const std::filesystem::path& input, const std::string& formatText, const std::filesystem::path& output, const std::string& filterTerm) {
    GffFile gff;
    gff.LoadFile(input);
    const auto format = neotabular::parseFormat(formatText);
    if (format == neotabular::Format::Xml) {
        if (!filterTerm.empty()) {
            throw std::runtime_error("Hierarchical GFF XML export preserves hierarchy and does not support row filtering.");
        }
        writeTextFile(output, ToGffXml(gff));
    } else if (format == neotabular::Format::Json) {
        if (!filterTerm.empty()) {
            throw std::runtime_error("Semantic GFF JSON export preserves hierarchy and does not support row filtering.");
        }
        writeTextFile(output, gffXmlToJson(ToGffXml(gff)));
    } else {
        throw std::runtime_error("NeoJRL exports only semantic XML or JSON. CSV/TSV flattened export is not supported for JRL/GFF files.");
    }
    return 0;
}

int importGff(const std::filesystem::path& input, const std::filesystem::path& output, const std::string& formatText, const std::filesystem::path& tablePath) {
    GffFile gff;
    const auto format = neotabular::parseFormat(formatText);
    if (format == neotabular::Format::Xml) {
        (void)input;
        LoadGffXml(gff, readTextFile(tablePath));
    } else if (format == neotabular::Format::Json) {
        (void)input;
        LoadGffXml(gff, gffJsonToXml(readTextFile(tablePath)));
    } else {
        throw std::runtime_error("NeoJRL imports only semantic XML or JSON. CSV/TSV flattened import is not supported for JRL/GFF files.");
    }
    gff.SaveFile(output);
    return 0;
}

int searchGff(const std::filesystem::path& input, const std::string& term) {
    GffFile gff;
    gff.LoadFile(input);
    printTableTsv(neotabular::filterRows(gffToTable(gff), term));
    return 0;
}

std::string resolveStrRef(const GffLocalizedStringField& loc, const neotlk::TlkLookup* tlk) {
    const std::string fallback = loc.GetStringById(0);
    if (loc.strref == 0xFFFFFFFFu) {
        return fallback;
    }
    if (tlk == nullptr) {
        return "Bad StrRef";
    }
    const auto resolved = tlk->resolve(loc.strref);
    return resolved.value_or(std::string("Bad StrRef"));
}

const GffField* field(const GffStruct& structure, const std::string& label) {
    return structure.GetFieldByLabel(label);
}

std::string fieldText(const GffStruct& structure, const std::string& label) {
    const GffField* f = field(structure, label);
    return f ? f->GetString() : std::string();
}

const GffList& requireCategories(const GffFile& gff) {
    const GffField* categoriesField = gff.GetFieldByLabel("Categories");
    if (categoriesField == nullptr || categoriesField->fieldtype != FIELD_TYPE_LIST) {
        throw std::runtime_error("Could not load categories list! Make sure the selected file is a valid global.jrl file!");
    }
    return static_cast<const GffList&>(*categoriesField);
}

std::size_t countEntries(const GffList& categories) {
    std::size_t total = 0;
    for (std::size_t i = 0; i < categories.count(); ++i) {
        const GffStruct* quest = categories.GetStruct(i);
        const GffField* entriesField = field(*quest, "EntryList");
        if (entriesField != nullptr && entriesField->fieldtype == FIELD_TYPE_LIST) {
            total += static_cast<const GffList&>(*entriesField).count();
        }
    }
    return total;
}

void printJournal(const GffList& categories, const neotlk::TlkLookup* tlk) {
    for (std::size_t i = 0; i < categories.count(); ++i) {
        const GffStruct* quest = categories.GetStruct(i);
        std::string name;
        const GffField* nameField = field(*quest, "Name");
        if (nameField != nullptr && nameField->fieldtype == FIELD_TYPE_CEXOLOCSTRING) {
            name = resolveStrRef(static_cast<const GffLocalizedStringField&>(*nameField), tlk);
        }
        std::cout << i << "\t" << fieldText(*quest, "Tag") << "\t" << name << "\n";

        const GffField* entriesField = field(*quest, "EntryList");
        if (entriesField != nullptr && entriesField->fieldtype == FIELD_TYPE_LIST) {
            const auto& entries = static_cast<const GffList&>(*entriesField);
            for (std::size_t j = 0; j < entries.count(); ++j) {
                const GffStruct* stage = entries.GetStruct(j);
                std::string text;
                const GffField* textField = field(*stage, "Text");
                if (textField != nullptr && textField->fieldtype == FIELD_TYPE_CEXOLOCSTRING) {
                    text = resolveStrRef(static_cast<const GffLocalizedStringField&>(*textField), tlk);
                }
                std::cout << "  " << j << "\tID=" << fieldText(*stage, "ID")
                          << "\tXP=" << fieldText(*stage, "XP_Percentage")
                          << "\tEnd=" << fieldText(*stage, "End")
                          << "\t" << text << "\n";
            }
        }
    }
}

int printInfo(const std::filesystem::path& jrl) {
    GffFile gff;
    gff.LoadFile(jrl);
    const GffList& categories = requireCategories(gff);
    std::cout << "type=" << gff.filetype() << "\n"
              << "version=" << gff.version() << "\n"
              << "categories=" << categories.count() << "\n"
              << "entries=" << countEntries(categories) << "\n";
    return 0;
}

int roundTrip(const std::filesystem::path& input, const std::filesystem::path& output) {
    GffFile gff;
    gff.LoadFile(input);
    requireCategories(gff);
    gff.SaveFile(output);
    return 0;
}

void printUsage() {
    std::cout << "Usage:\n"
              << "  neojrl-cli <global.jrl> [dialog.tlk]\n"
              << "  neojrl-cli --info <global.jrl>\n"
              << "  neojrl-cli --roundtrip <global.jrl> <output.jrl>\n"
              << "  neojrl-cli --search <global.jrl> <term>\n"
              << "  neojrl-cli --export <global.jrl> <xml|json> <output>\n"
              << "  neojrl-cli --import-values <input.jrl> <output.jrl> <xml|json> <input-document>\n"
              << "  neojrl-cli --diff-tslpatcher <original.jrl> <modified-input> <output-dir|fragment.ini> [--modified-format xml|json|jrl|gff|kotor|native|auto] [--package|--fragment] [--filename name] [--ini installer.ini] [--allow-unsupported]\n"
              << "  neojrl-cli --diff-tslpatcher-import <original.jrl> <modified-input> <xml|json|jrl|gff|kotor|native|auto> <output-dir|fragment.ini> [--package|--fragment] [--filename name] [--ini installer.ini] [--allow-unsupported]\n\n"
              << "Prints, inspects, rewrites, searches, imports, or exports a canonical GFF V3.2 journal file.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" || std::string(argv[1]) == "help")) {
            printUsage();
            return 0;
        }

        if (argc >= 2 && std::string(argv[1]) == "--info") {
            if (argc != 3) {
                printUsage();
                return 2;
            }
            return printInfo(argv[2]);
        }

        if (argc >= 2 && std::string(argv[1]) == "--roundtrip") {
            if (argc != 4) {
                printUsage();
                return 2;
            }
            return roundTrip(argv[2], argv[3]);
        }

        if (argc >= 2 && std::string(argv[1]) == "--search") {
            if (argc != 4) {
                printUsage();
                return 2;
            }
            return searchGff(argv[2], argv[3]);
        }

        if (argc >= 2 && std::string(argv[1]) == "--export") {
            if (argc != 5 && argc != 6) {
                printUsage();
                return 2;
            }
            return exportGff(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : std::string());
        }


        if (argc >= 2 && (std::string(argv[1]) == "--diff-tslpatcher" || std::string(argv[1]) == "diff-tslpatcher" ||
                          std::string(argv[1]) == "--diff-tslpatcher-import" || std::string(argv[1]) == "diff-tslpatcher-import")) {
            const std::string cmd = argv[1];
            const bool explicitImport = cmd == "--diff-tslpatcher-import" || cmd == "diff-tslpatcher-import";
            if ((!explicitImport && argc < 5) || (explicitImport && argc < 6)) {
                printUsage();
                return 2;
            }
            if (explicitImport) {
                auto options = parsePatchOutputOptions(argc, argv, 6, argv[2]);
                options.modifiedFormat = argv[4];
                return diffTslPatcher(argv[2], argv[3], argv[5], options);
            }
            const auto options = parsePatchOutputOptions(argc, argv, 5, argv[2]);
            return diffTslPatcher(argv[2], argv[3], argv[4], options);
        }

        if (argc >= 2 && std::string(argv[1]) == "--import-values") {
            if (argc != 6) {
                printUsage();
                return 2;
            }
            return importGff(argv[2], argv[3], argv[4], argv[5]);
        }

        if (argc < 2 || argc > 3) {
            printUsage();
            return argc < 2 ? 1 : 2;
        }

        GffFile gff;
        gff.LoadFile(argv[1]);
        const GffList& categories = requireCategories(gff);

        std::unique_ptr<neotlk::TlkLookup> tlk;
        if (argc == 3) {
            tlk = std::make_unique<neotlk::TlkLookup>();
            tlk->load(std::filesystem::path(argv[2]));
        }

        printJournal(categories, tlk.get());
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
