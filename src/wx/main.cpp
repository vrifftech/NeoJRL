#include "GFFFile.hpp"
#include "GffXml.hpp"
#include "JrlEntryOperations.hpp"
#include <neotlk/TlkLookup.hpp>
#include "TabularData.hpp"
#include "TslPatcher.hpp"
#include "AppModel.hpp"
#include "GffJson.hpp"
#include "wx_ui.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "neojrl_icon.xpm"

#include <wx/aui/auibook.h>
#include <wx/checkbox.h>
#include <wx/config.h>
#include <wx/clipbrd.h>
#include <wx/choice.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statbox.h>
#include <wx/wx.h>
#include <wx/version.h>

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace neojrl;

constexpr const char* kAppName = "NeoJRL";
constexpr const char* kJRLWildcard = "Journal files (*.jrl)|*.jrl|All files (*.*)|*.*";
constexpr const char* kTlkWildcard = "TLK files (*.tlk)|*.tlk|All files (*.*)|*.*";
constexpr const char* kXmlTableWildcard = "XML files (*.xml)|*.xml|All files (*.*)|*.*";
constexpr const char* kJsonTableWildcard = "JSON files (*.json)|*.json|All files (*.*)|*.*";

const char* tableWildcardForFormat(neotabular::Format format) {
    switch (format) {
    case neotabular::Format::Xml: return kXmlTableWildcard;
    case neotabular::Format::Json: return kJsonTableWildcard;
    default: throw std::invalid_argument("NeoJRL supports semantic XML and JSON table import/export only.");
    }
}

std::string exportExtensionForFormat(neotabular::Format format) {
    switch (format) {
    case neotabular::Format::Csv: return "csv";
    case neotabular::Format::Tsv: return "tsv";
    case neotabular::Format::Xml: return "xml";
    case neotabular::Format::Json: return "json";
    }
    return "txt";
}

std::string exportDefaultFilename(const std::filesystem::path& source,
                                  neotabular::Format format,
                                  const std::string& fallbackStem) {
    std::string stem = source.empty() ? fallbackStem : source.stem().string();
    if (stem.empty()) stem = fallbackStem.empty() ? std::string("export") : fallbackStem;
    return stem + "." + exportExtensionForFormat(format);
}

constexpr std::uint32_t kNoStrRef = 0xFFFFFFFFu;

std::string questColumnLabel(std::size_t column) {
    switch (column) {
        case 0: return "#";
        case 1: return "Tag";
        default: return "Column " + std::to_string(column);
    }
}

std::string entryColumnLabel(std::size_t column) {
    switch (column) {
        case 0: return "#";
        case 1: return "ID";
        default: return "Column " + std::to_string(column);
    }
}

enum : int {
    ID_Open = wxID_HIGHEST + 1,
    ID_LoadTlk,
    ID_Save,
    ID_SaveAs,
    ID_CloseTab,
    ID_CloseOtherTabs,
    ID_NextTab,
    ID_PreviousTab,
    ID_DocumentTabs,
    ID_Search,
    ID_ApplySearchFilter,
    ID_ClearFilter,
    ID_FilterColumn,
    ID_ClearColumnFilter,
    ID_ClearAllFilters,
    ID_ResetColumnOrder,
    ID_ResetRowOrder,
    ID_ApplyQuest,
    ID_ApplyEntry,
    ID_NewEntry,
    ID_DeleteEntry,
    ID_CopyCells,
    ID_PasteCells,
    ID_ImportXml,
    ID_ImportJson,
    ID_ExportXml,
    ID_ExportJson,
    ID_ExportPatcherPackage,
    ID_ExportPatcherFragment,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
};

constexpr int kRecentFileBaseId = wxID_HIGHEST + 1000;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

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
        throw std::runtime_error("The selected file is not a valid canonical global.jrl file.");
    }
    return static_cast<const GffList&>(*categoriesField);
}

std::string normalizedJrlPatcherType(std::string type) {
    type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), type.end());
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return type;
}

void requireJrlPatcherDocument(const GffFile& gff, const std::string& role) {
    if (!gff.loaded()) throw std::runtime_error(role + " is not loaded.");
    if (gff.isGff4()) {
        throw std::runtime_error(
            role + " is GFF4. TSLPatcher/HoloPatcher [GFFList] journal output supports canonical GFF3 JRL files only.");
    }
    if (normalizedJrlPatcherType(gff.filetype()) != "JRL") {
        throw std::runtime_error(role + " is not a JRL file.");
    }
    (void)requireCategories(gff);
}

void requireMatchingJrlPatcherDocuments(const GffFile& original, const GffFile& modified) {
    requireJrlPatcherDocument(original, "The original patch baseline");
    requireJrlPatcherDocument(modified, "The modified patch document");
    if (original.version() != modified.version()) {
        throw std::runtime_error("The original and modified JRL versions do not match.");
    }
}

neotabular::Table jrlTableForPatcher(const GffFile& gff) {
    neogff::GffModel model;
    model.importXml(ToGffXml(gff));
    return model.toTable();
}

const GffList* questEntries(const GffStruct& quest) {
    const GffField* listField = quest.GetFieldByLabel("EntryList");
    if (listField == nullptr || listField->fieldtype != FIELD_TYPE_LIST) {
        return nullptr;
    }
    return &static_cast<const GffList&>(*listField);
}

std::string locStringText(const GffLocalizedStringField& loc, const neotlk::TlkLookup* tlk) {
    if (loc.strref == kNoStrRef) {
        return loc.GetStringById(0);
    }
    if (tlk == nullptr) {
        return "Bad StrRef";
    }
    const auto resolved = tlk->resolve(loc.strref);
    return resolved.value_or(std::string("Bad StrRef"));
}

std::string locStrRefText(const GffStruct& structure, const std::string& label) {
    const GffField* f = field(structure, label);
    if (f != nullptr && f->fieldtype == FIELD_TYPE_CEXOLOCSTRING) {
        const auto& loc = static_cast<const GffLocalizedStringField&>(*f);
        return loc.strref == kNoStrRef ? std::string("-1") : std::to_string(loc.strref);
    }
    return std::string();
}

std::string locResolvedText(const GffStruct& structure, const std::string& label, const neotlk::TlkLookup* tlk) {
    const GffField* f = field(structure, label);
    if (f != nullptr && f->fieldtype == FIELD_TYPE_CEXOLOCSTRING) {
        return locStringText(static_cast<const GffLocalizedStringField&>(*f), tlk);
    }
    return std::string();
}

std::string pathForQuest(std::size_t questIndex, const std::string& fieldName) {
    return "Categories\\" + std::to_string(questIndex) + "\\" + fieldName;
}

std::string pathForStage(std::size_t questIndex, std::size_t stageIndex, const std::string& fieldName) {
    return "Categories\\" + std::to_string(questIndex) + "\\EntryList\\" + std::to_string(stageIndex) + "\\" + fieldName;
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

std::string trimAscii(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::optional<std::int32_t> parseOptionalInt32(const wxTextCtrl& control, const char* label) {
    const std::string text = trimAscii(wxui::toStd(control.GetValue()));
    if (text.empty()) return std::nullopt;
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed, 10);
    if (consumed != text.size() || value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(label) + " must be a signed 32-bit integer.");
    }
    return static_cast<std::int32_t>(value);
}

std::optional<UInt32> parseOptionalDword(const wxTextCtrl& control, const char* label) {
    const std::string text = trimAscii(wxui::toStd(control.GetValue()));
    if (text.empty()) return std::nullopt;
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size() || value > std::numeric_limits<UInt32>::max()) {
        throw std::runtime_error(std::string(label) + " must be an unsigned 32-bit integer.");
    }
    return static_cast<UInt32>(value);
}

UInt32 parseRequiredDword(const wxTextCtrl& control, const char* label) {
    const auto value = parseOptionalDword(control, label);
    if (!value) throw std::runtime_error(std::string(label) + " is required.");
    return *value;
}

UInt32 parseRequiredStrRef(const wxTextCtrl& control, const char* label) {
    const std::string text = trimAscii(wxui::toStd(control.GetValue()));
    if (text == "-1") return kNoStrRef;
    if (text.empty()) throw std::runtime_error(std::string(label) + " is required and must be -1 or an unsigned 32-bit value.");
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size() || value > std::numeric_limits<UInt32>::max()) {
        throw std::runtime_error(std::string(label) + " must be -1 or an unsigned 32-bit value.");
    }
    return static_cast<UInt32>(value);
}

std::optional<std::string> optionalText(const wxTextCtrl& control) {
    const std::string value = wxui::toStd(control.GetValue());
    return trimAscii(value).empty() ? std::nullopt : std::optional<std::string>{value};
}

std::optional<std::uint16_t> parseOptionalWord(const wxTextCtrl& control, const char* label) {
    const std::string text = trimAscii(wxui::toStd(control.GetValue()));
    if (text.empty()) return std::nullopt;
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size() || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(std::string(label) + " must be between 0 and 65535.");
    }
    return static_cast<std::uint16_t>(value);
}

std::optional<float> parseOptionalFloat(const wxTextCtrl& control, const char* label) {
    const std::string text = trimAscii(wxui::toStd(control.GetValue()));
    if (text.empty()) return std::nullopt;
    std::size_t consumed = 0;
    const float value = std::stof(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value)) {
        throw std::runtime_error(std::string(label) + " must be a finite decimal number.");
    }
    if (value < 0.0f) {
        throw std::runtime_error(std::string(label) + " cannot be negative.");
    }
    return value;
}


std::size_t optionalColumn(const neotabular::Table& table, const std::string& name) {
    const std::string want = lowerAscii(name);
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (lowerAscii(table.columns[i]) == want) return i;
    }
    return table.columns.size();
}

std::size_t requireColumn(const neotabular::Table& table, const std::string& name) {
    const auto index = optionalColumn(table, name);
    if (index == table.columns.size()) throw std::runtime_error("Imported table is missing required column: " + name);
    return index;
}

std::string tableCell(const std::vector<std::string>& row, std::size_t index) {
    return index < row.size() ? row[index] : std::string();
}

std::optional<std::filesystem::path> readCachedTlkPath() {
    return neosettings::AppSettings(kAppName).lastTlkPath();
}

void writeCachedTlkPath(const std::filesystem::path& path) {
    neosettings::AppSettings(kAppName).setLastTlkPath(path);
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

class NeoJRLFrame final : public wxFrame {
public:
    NeoJRLFrame()
        : wxFrame(nullptr, wxID_ANY, "NeoJRL v1.0.0 (JRL file editor)", wxDefaultPosition, wxDefaultSize) {
        setApplicationIcon();
        buildMenus();
        buildLayout();
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        bindEvents();
        createDocumentTab(true);
        tryLoadCachedTlk();
        refreshQuests();
        clearQuestPanel();
        clearEntryPanel();
        wxui::setStatusText(*this, tlk().has_value() ? wxString("NeoJRL v1.0.0 ready. Open global.jrl.") : wxString("NeoJRL v1.0.0 ready. Open global.jrl; loading dialog.tlk is optional for resolved text."));
    }

private:

    struct DocumentTab {
        std::unique_ptr<GffFile> gff = std::make_unique<GffFile>();
        std::optional<neotlk::TlkLookup> tlk;
        std::filesystem::path tlkPath;
        std::string tlkAutoLoadWarning;
        neoview::DocumentViewState viewState;
        neoview::DocumentViewState entryViewState;
        std::string untitledName = "Untitled JRL";
        wxWindow* tabPage = nullptr;
    };

    bool hasActiveDocument() const {
        return activeDocumentIndex_ != neotabs::npos && activeDocumentIndex_ < documents_.size();
    }

    DocumentTab& activeDocument() { return documents_.at(activeDocumentIndex_); }
    const DocumentTab& activeDocument() const { return documents_.at(activeDocumentIndex_); }
    GffFile& gff() { return *activeDocument().gff; }
    const GffFile& gff() const { return *activeDocument().gff; }
    std::optional<neotlk::TlkLookup>& tlk() { return activeDocument().tlk; }
    const std::optional<neotlk::TlkLookup>& tlk() const { return activeDocument().tlk; }
    std::filesystem::path& tlkPath() { return activeDocument().tlkPath; }
    const std::filesystem::path& tlkPath() const { return activeDocument().tlkPath; }
    std::string& tlkAutoLoadWarning() { return activeDocument().tlkAutoLoadWarning; }
    const std::string& tlkAutoLoadWarning() const { return activeDocument().tlkAutoLoadWarning; }
    neoview::DocumentViewState& viewState() { return activeDocument().viewState; }
    const neoview::DocumentViewState& viewState() const { return activeDocument().viewState; }
    neoview::DocumentViewState& entryViewState() { return activeDocument().entryViewState; }
    const neoview::DocumentViewState& entryViewState() const { return activeDocument().entryViewState; }

    bool tabDirty(const DocumentTab& tab) const { return tab.gff && tab.gff->dirty(); }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(tab.gff ? tab.gff->filename() : std::filesystem::path{}, tab.untitledName);
    }

    void updateActiveTabTitle() {
        if (!hasActiveDocument()) return;
        neotabs::setTabLabel(documentTabs_, activeDocument().tabPage, tabDisplayName(activeDocument()), tabDirty(activeDocument()));
    }

    void createDocumentTab(bool select = true) {
        DocumentTab tab;
        tab.gff = std::make_unique<GffFile>();
        tab.viewState.resetForNewDocument();
        tab.entryViewState.resetForNewDocument();
        const std::size_t previousActiveIndex = activeDocumentIndex_;
        documents_.push_back(std::move(tab));
        const std::size_t index = documents_.size() - 1;

        tabSwitchInProgress_ = true;
        wxWindow* const page = neotabs::addTabPage(
            documentTabs_, tabDisplayName(documents_.back()), tabDirty(documents_.back()), select);
        if (page != nullptr) documents_.back().tabPage = page;
        tabSwitchInProgress_ = false;

        if (page == nullptr) {
            documents_.pop_back();
            activeDocumentIndex_ = previousActiveIndex;
            throw std::runtime_error("Unable to create a document tab.");
        }

        if (select) {
            activeDocumentIndex_ = index;
            tabSwitchInProgress_ = true;
            neotabs::changeSelectionToPage(documentTabs_, page);
            tabSwitchInProgress_ = false;
            refreshQuests();
            clearQuestPanel();
            clearEntryPanel();
        }
    }

    bool activeTabIsReusableForOpen() const {
        return hasActiveDocument() && documents_.size() == 1 && !tabDirty(activeDocument()) && !gff().loaded();
    }

    void ensureDocumentTabForOpen() {
        if (!hasActiveDocument()) { createDocumentTab(true); return; }
        if (!activeTabIsReusableForOpen()) createDocumentTab(true);
    }

    void selectDocumentTab(std::size_t index) {
        if (documentTabs_ == nullptr || index >= documents_.size()) return;
        tabSwitchInProgress_ = true;
        const bool selected = neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        if (!selected) return;
        activeDocumentIndex_ = index;
        refreshQuests();
        loadSelectedQuest();
        updateActiveTabTitle();
    }

    bool confirmCloseDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return true;
        if (!tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocumentTab(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocumentTab(index)) return false;

        wxWindow* const page = documents_[index].tabPage;
        tabSwitchInProgress_ = true;
        const bool deleted = neotabs::deleteTabPage(documentTabs_, page);
        tabSwitchInProgress_ = false;
        if (!deleted) return false;

        documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
        if (documents_.empty()) {
            activeDocumentIndex_ = neotabs::npos;
            createDocumentTab(true);
            return true;
        }

        std::size_t selectedIndex = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (selectedIndex == neotabs::npos) selectedIndex = std::min(index, documents_.size() - 1);
        selectDocumentTab(selectedIndex);
        return true;
    }

    bool confirmCloseAllTabs() {
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!confirmCloseDocumentTab(i)) return false;
        }
        return true;
    }

    void onDocumentTabChanged(wxAuiNotebookEvent& event) {
        if (tabSwitchInProgress_) { event.Skip(); return; }
        const int selection = event.GetSelection();
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) selectDocumentTab(index);
        event.Skip();
    }

    void onDocumentTabCloseRequested(wxAuiNotebookEvent& event) {
        event.Veto();
        const int selection = event.GetSelection();
        if (selection < 0) return;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) closeDocumentTab(index);
    }

    void rebuildRecentFilesMenu() {
        if (recentFilesMenu_ != nullptr) {
            neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, kRecentFileBaseId, kClearRecentFilesId);
        }
    }

    void rememberRecentFile(const std::filesystem::path& path) {
        settings_.addRecentFile(path);
        rebuildRecentFilesMenu();
    }

    void tryLoadResolvedTlkForPath(const std::filesystem::path& path) {
        if (tlk().has_value()) return;
        const auto resolvedTlkPath = neogames::resolver().bestTlkForPath(path);
        if (!resolvedTlkPath || resolvedTlkPath->empty()) return;
        try {
            tlk().emplace();
            tlk()->load(*resolvedTlkPath);
            tlkPath() = *resolvedTlkPath;
            writeCachedTlkPath(*resolvedTlkPath);
            tlkAutoLoadWarning().clear();
        } catch (const std::exception& ex) {
            tlkAutoLoadWarning() = std::string("Unable to auto-load resolved TLK: ") + ex.what();
        }
    }

    void openJrlPath(const std::filesystem::path& path) {
        if (path.empty()) return;
        ensureDocumentTabForOpen();
        gff().LoadFile(path);
        viewState().resetForNewDocument();
        entryViewState().resetForNewDocument();
        if (searchText_) searchText_->ChangeValue("");
        tryLoadResolvedTlkForPath(path);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        refreshQuests();
    }

    void onOpenRecent(wxCommandEvent& event) {
        const int index = event.GetId() - kRecentFileBaseId;
        const auto files = settings_.recentFiles();
        if (index < 0 || static_cast<std::size_t>(index) >= files.size()) return;
        try {
            if (!std::filesystem::exists(files[static_cast<std::size_t>(index)])) {
                settings_.removeRecentFile(files[static_cast<std::size_t>(index)]);
                rebuildRecentFilesMenu();
                throw std::runtime_error("Recent file no longer exists: " + files[static_cast<std::size_t>(index)].string());
            }
            openJrlPath(files[static_cast<std::size_t>(index)]);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onClearRecentFiles(wxCommandEvent&) {
        settings_.clearRecentFiles();
        rebuildRecentFilesMenu();
    }

    void setApplicationIcon() {
        wxIconBundle bundle;
#if defined(__WXMSW__)
        wxIcon windowsIcon("neojrl", wxBITMAP_TYPE_ICO_RESOURCE);
        if (windowsIcon.IsOk()) {
            bundle.AddIcon(windowsIcon);
        }
#endif
        wxIcon fallbackIcon(neojrl_icon_xpm);
        if (fallbackIcon.IsOk()) {
            bundle.AddIcon(fallbackIcon);
        }
        if (bundle.GetIconCount() > 0) {
            SetIcons(bundle);
        }
    }

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_Open, "&Open...\tCtrl+O");
        recentFilesMenu_ = new wxMenu;
        rebuildRecentFilesMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->Append(ID_LoadTlk, "Load optional &dialog.tlk...");
        file->Append(ID_Save, "&Save\tCtrl+S");
        file->Append(ID_SaveAs, "Save &As...");
        file->AppendSeparator();
        file->Append(ID_CloseTab, "&Close Tab\tCtrl-W");
        file->Append(ID_CloseOtherTabs, "Close &Other Tabs");
        file->Append(ID_NextTab, "Next Tab\tCtrl-Tab");
        file->Append(ID_PreviousTab, "Previous Tab\tCtrl-Shift-Tab");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const std::filesystem::path& directory) {
                chooseAndOpenJrl(directory);
            });
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit");

        auto* import = new wxMenu;
        import->Append(ID_ImportXml, "Import &XML...");
        import->Append(ID_ImportJson, "Import &JSON...");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportXml, "Export as &XML...");
        exportMenu->Append(ID_ExportJson, "Export as &JSON...");
        exportMenu->AppendSeparator();
        exportMenu->Append(ID_ExportPatcherPackage, "Export TSL/HoloPatcher &Package...");
        exportMenu->Append(ID_ExportPatcherFragment, "Export TSL/HoloPatcher &Fragment...");

        auto* edit = new wxMenu;
        edit->Append(ID_CopyCells, "&Copy Selection	Ctrl-C");
        edit->Append(ID_PasteCells, "&Paste Values	Ctrl-V");
        edit->AppendSeparator();
        edit->Append(ID_ApplyQuest, "Apply &Quest");
        edit->Append(ID_ApplyEntry, "Apply &Entry");
        edit->AppendSeparator();
        edit->Append(ID_NewEntry, "&New Entry");
        edit->Append(ID_DeleteEntry, "&Delete Entry");

        auto* tools = new wxMenu;
        tools->Append(ID_ApplySearchFilter, "Apply Search as Quest &Filter");
        tools->Append(ID_FilterColumn, "Filter Selected &Column...");
        tools->Append(ID_ClearColumnFilter, "Clear Filter on Selected Column");
        tools->Append(ID_ClearAllFilters, "Clear All Filters");

        auto* view = new wxMenu;
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        view->AppendSeparator();
        view->Append(ID_ResetColumnOrder, "Reset Column Order");
        view->Append(ID_ResetRowOrder, "Reset Row Order");

        auto* help = new wxMenu;
        help->Append(wxID_ABOUT, "&About");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(import, "&Import");
        bar->Append(exportMenu, "&Export");
        bar->Append(edit, "&Edit");
        bar->Append(tools, "&Tools");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        SetMenuBar(bar);
    }

    void buildLayout() {
        auto* panel = new wxPanel(this);
        auto* root = new wxBoxSizer(wxVERTICAL);

        documentTabs_ = new wxAuiNotebook(panel, ID_DocumentTabs, wxDefaultPosition, wxDefaultSize,
                                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB | wxAUI_NB_SCROLL_BUTTONS);
        root->Add(documentTabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        neotabs::configureDocumentTabStrip(documentTabs_);

        root->Add(buildSearchBar(panel), 0, wxEXPAND | wxALL, 6);

        auto* main = new wxSplitterWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
        auto* left = new wxPanel(main);
        auto* right = new wxPanel(main);

        auto* leftSizer = new wxBoxSizer(wxVERTICAL);
        leftSizer->Add(new wxStaticText(left, wxID_ANY, "Quests"), 0, wxLEFT | wxTOP, 4);
        questList_ = new wxListCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
        wxui::setColumns(*questList_, {{"#", 55}, {"Tag", 205}});
        leftSizer->Add(questList_, 1, wxEXPAND | wxALL, 4);
        left->SetSizer(leftSizer);

        auto* rightSizer = new wxBoxSizer(wxVERTICAL);
        rightSizer->Add(buildQuestPanel(right), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
        rightSizer->Add(buildEntryListAndPanel(right), 1, wxEXPAND | wxALL, 4);
        right->SetSizer(rightSizer);

        main->SplitVertically(left, right);
        main->SetSashGravity(0.30);
        main->SetMinimumPaneSize(FromDIP(220));
        root->Add(main, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        panel->SetSizer(root);

        wxui::createStatusBar(*this, 2);
        int widths[] = {-2, -3};
        SetStatusWidths(2, widths);
        wxui::configureResponsiveWindow(*this, wxSize(980, 640), wxSize(620, 400));
        settings_.restoreWindowPlacement(*this);
    }

    wxSizer* buildSearchBar(wxWindow* parent) {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, parent, "Journal");

        auto* jrlRow = new wxBoxSizer(wxHORIZONTAL);
        jrlRow->Add(new wxStaticText(parent, wxID_ANY, "JRL file:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filePath_ = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        jrlRow->Add(filePath_, 1, wxEXPAND | wxRIGHT, FromDIP(6));
        jrlRow->Add(new wxButton(parent, ID_Open, "Open..."), 0, wxRIGHT, FromDIP(4));
        jrlRow->Add(new wxButton(parent, ID_Save, "Save"), 0, wxRIGHT, FromDIP(4));
        jrlRow->Add(new wxButton(parent, ID_SaveAs, "Save As..."), 0);
        box->Add(jrlRow, 0, wxEXPAND | wxALL, FromDIP(8));

        auto* tlkRow = new wxBoxSizer(wxHORIZONTAL);
        tlkRow->Add(new wxStaticText(parent, wxID_ANY, "dialog.tlk:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        tlkPathText_ = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        tlkRow->Add(tlkPathText_, 1, wxEXPAND | wxRIGHT, FromDIP(6));
        tlkRow->Add(new wxButton(parent, ID_LoadTlk, "Load optional TLK..."), 0);
        box->Add(tlkRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* searchRow = new wxBoxSizer(wxHORIZONTAL);
        searchRow->Add(new wxStaticText(parent, wxID_ANY, "Search/filter:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        searchMode_ = new wxChoice(parent, wxID_ANY);
        searchMode_->Append("Quest Tag");
        searchMode_->Append("Quest Name");
        searchMode_->Append("Quest Stage Text");
        searchMode_->Append("PlanetID");
        searchMode_->Append("Priority");
        searchMode_->Append("PlotIndex");
        searchMode_->Append("Picture");
        searchMode_->Append("Quest XP");
        searchMode_->SetSelection(0);
        searchRow->Add(searchMode_, 0, wxRIGHT, FromDIP(6));
        searchText_ = new wxTextCtrl(parent, wxID_ANY);
        searchRow->Add(searchText_, 1, wxEXPAND | wxRIGHT, FromDIP(6));
        searchRow->Add(new wxButton(parent, ID_Search, "Search"), 0);
        box->Add(searchRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        return box;
    }

    wxSizer* buildQuestPanel(wxWindow* parent) {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, parent, "Quest");
        auto* form = new wxFlexGridSizer(2, 2, 5, 8);
        form->AddGrowableCol(1, 1);

        profileLabel_ = new wxStaticText(parent, wxID_ANY, "Detected profile");
        form->Add(profileLabel_, 0, wxALIGN_CENTER_VERTICAL);
        profile_ = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        form->Add(profile_, 1, wxEXPAND);

        nameStrRef_ = addText(parent, form, "Name StrRef");
        name_ = addText(parent, form, "Name text");
        name_->SetToolTip("When Name StrRef is -1, this edits the embedded language-0 name. Otherwise it shows the resolved TLK text.");
        tag_ = addText(parent, form, "Tag");
        comment_ = addText(parent, form, "Comment");
        priority_ = addText(parent, form, "Sort priority");
        picture_ = addText(parent, form, "Journal picture ID");
        planet_ = addText(parent, form, "Planet ID", &planetLabel_);
        plotIndex_ = addText(parent, form, "PlotXP.2da row", &plotIndexLabel_);
        questXp_ = addText(parent, form, "Quest XP", &questXpLabel_);

        box->Add(form, 0, wxEXPAND | wxALL, 6);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer(1);
        row->Add(new wxButton(parent, ID_ApplyQuest, "Apply Quest"), 0, wxRIGHT, 6);
        box->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        return box;
    }

    wxWindow* buildEntryListAndPanel(wxWindow* parent) {
        auto* splitter = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
        auto* entriesPanel = new wxPanel(splitter);
        auto* detailPanel = new wxPanel(splitter);

        auto* entriesSizer = new wxStaticBoxSizer(wxVERTICAL, entriesPanel, "Entries");
        entryList_ = new wxListCtrl(entriesPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
        wxui::setColumns(*entryList_, {{"#", 55}, {"ID", 95}});
        entriesSizer->Add(entryList_, 1, wxEXPAND | wxALL, 4);
        auto* entryButtons = new wxBoxSizer(wxHORIZONTAL);
        entryButtons->Add(new wxButton(entriesPanel, ID_NewEntry, "New Entry"), 0, wxRIGHT, 4);
        entryButtons->Add(new wxButton(entriesPanel, ID_DeleteEntry, "Delete Entry"), 0);
        entriesSizer->Add(entryButtons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
        entriesPanel->SetSizer(entriesSizer);

        auto* detailSizer = new wxStaticBoxSizer(wxVERTICAL, detailPanel, "Selected Entry");
        auto* form = new wxFlexGridSizer(4, 2, 5, 8);
        form->AddGrowableCol(1, 1);
        entryId_ = addText(detailPanel, form, "Entry ID");
        entryXp_ = addText(detailPanel, form, "Plot XP multiplier", &entryXpLabel_);
        entryXp_->SetToolTip("KotOR multiplies PlotXP.2da XP by this value: 0.5 = 50%, 1.0 = 100%.");
        form->Add(new wxStaticText(detailPanel, wxID_ANY, "End"), 0, wxALIGN_CENTER_VERTICAL);
        entryEnd_ = new wxCheckBox(detailPanel, wxID_ANY, wxEmptyString);
        form->Add(entryEnd_, 0, wxALIGN_CENTER_VERTICAL);
        entryStrRef_ = addText(detailPanel, form, "StrRef");
        detailSizer->Add(form, 0, wxEXPAND | wxALL, 6);
        detailSizer->Add(new wxStaticText(detailPanel, wxID_ANY, "Entry Text"), 0, wxLEFT | wxRIGHT, 6);
        entryText_ = new wxTextCtrl(detailPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxTE_MULTILINE | wxTE_RICH2);
        detailSizer->Add(entryText_, 1, wxEXPAND | wxALL, 6);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer(1);
        row->Add(new wxButton(detailPanel, ID_ApplyEntry, "Apply Entry"), 0, wxRIGHT, 6);
        detailSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        detailPanel->SetSizer(detailSizer);

        splitter->SplitVertically(entriesPanel, detailPanel);
        splitter->SetSashGravity(0.35);
        splitter->SetMinimumPaneSize(FromDIP(140));
        return splitter;
    }

    wxTextCtrl* addText(wxWindow* parent,
                        wxSizer* sizer,
                        const char* label,
                        wxStaticText** labelOut = nullptr,
                        long style = 0) {
        auto* caption = new wxStaticText(parent, wxID_ANY, label);
        if (labelOut != nullptr) *labelOut = caption;
        sizer->Add(caption, 0, wxALIGN_CENTER_VERTICAL);
        auto* ctrl = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, style);
        sizer->Add(ctrl, 1, wxEXPAND);
        return ctrl;
    }

    void bindEvents() {
        Bind(wxEVT_MENU, &NeoJRLFrame::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &NeoJRLFrame::onOpenRecent, this, kRecentFileBaseId, kRecentFileBaseId + neosettings::kMaxRecentFiles - 1);
        Bind(wxEVT_MENU, &NeoJRLFrame::onClearRecentFiles, this, kClearRecentFilesId);
        Bind(wxEVT_MENU, &NeoJRLFrame::onLoadTlk, this, ID_LoadTlk);
        Bind(wxEVT_MENU, &NeoJRLFrame::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &NeoJRLFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &NeoJRLFrame::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &NeoJRLFrame::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &NeoJRLFrame::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &NeoJRLFrame::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &NeoJRLFrame::onSearch, this, ID_Search);
        Bind(wxEVT_MENU, &NeoJRLFrame::onApplySearchFilter, this, ID_ApplySearchFilter);
        Bind(wxEVT_MENU, &NeoJRLFrame::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_MENU, &NeoJRLFrame::onFilterSelectedColumn, this, ID_FilterColumn);
        Bind(wxEVT_MENU, &NeoJRLFrame::onClearSelectedColumnFilter, this, ID_ClearColumnFilter);
        Bind(wxEVT_MENU, &NeoJRLFrame::onClearAllFilters, this, ID_ClearAllFilters);
        Bind(wxEVT_MENU, &NeoJRLFrame::onResetColumnOrder, this, ID_ResetColumnOrder);
        Bind(wxEVT_MENU, &NeoJRLFrame::onResetRowOrder, this, ID_ResetRowOrder);
        Bind(wxEVT_MENU, &NeoJRLFrame::onApplyQuest, this, ID_ApplyQuest);
        Bind(wxEVT_MENU, &NeoJRLFrame::onApplyEntry, this, ID_ApplyEntry);
        Bind(wxEVT_MENU, &NeoJRLFrame::onNewEntry, this, ID_NewEntry);
        Bind(wxEVT_MENU, &NeoJRLFrame::onDeleteEntry, this, ID_DeleteEntry);
        Bind(wxEVT_MENU, &NeoJRLFrame::onCopyCells, this, ID_CopyCells);
        Bind(wxEVT_MENU, &NeoJRLFrame::onPasteCells, this, ID_PasteCells);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Xml); }, ID_ImportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Json); }, ID_ImportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Xml); }, ID_ExportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Json); }, ID_ExportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExportPatcher(true); }, ID_ExportPatcherPackage);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExportPatcher(false); }, ID_ExportPatcherFragment);
        Bind(wxEVT_MENU, &NeoJRLFrame::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoJRLFrame::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoJRLFrame::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoJRLFrame::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About NeoJRL", "NeoJRL v1.0.0\nNative wxWidgets journal editor\n\nA special thanks to everyone in the KOTOR modding community that has contributed their work, knowledge, and creativity to making tools, mods, and guides over the last 20+ years");
        }, wxID_ABOUT);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &NeoJRLFrame::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &NeoJRLFrame::onDocumentTabCloseRequested, this);
        Bind(wxEVT_CLOSE_WINDOW, &NeoJRLFrame::onClose, this);

        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_Open);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_LoadTlk);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_Save);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_Search);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_ApplyQuest);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_ApplyEntry);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_NewEntry);
        Bind(wxEVT_BUTTON, &NeoJRLFrame::dispatchButton, this, ID_DeleteEntry);
        questList_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { loadSelectedQuest(); });
        entryList_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { loadSelectedEntry(); });
        questList_->Bind(wxEVT_LIST_COL_RIGHT_CLICK, &NeoJRLFrame::onQuestColumnRightClick, this);
        entryList_->Bind(wxEVT_LIST_COL_RIGHT_CLICK, &NeoJRLFrame::onEntryColumnRightClick, this);
    }

    void dispatchButton(wxCommandEvent& event) {
        wxCommandEvent menuEvent(wxEVT_MENU, event.GetId());
        ProcessWindowEvent(menuEvent);
    }

    void ensureLoaded() const {
        if (!gff().loaded()) {
            throw std::runtime_error("No journal file is loaded.");
        }
    }

    void updateHeaderPaths() {
        if (filePath_ != nullptr) {
            const std::string path = gff().loaded() ? gff().filename().string() : std::string();
            if (wxui::toStd(filePath_->GetValue()) != path) filePath_->ChangeValue(wxui::toWx(path));
        }
        if (tlkPathText_ != nullptr) {
            const std::string path = tlk().has_value() ? tlkPath().string() : std::string();
            if (wxui::toStd(tlkPathText_->GetValue()) != path) tlkPathText_->ChangeValue(wxui::toWx(path));
        }
    }

    void updateQuestColumnLabels() {
        neoview::ensureIdentityColumns(viewState(), 2);
        for (std::size_t visualColumn = 0; visualColumn < 2; ++visualColumn) {
            const std::size_t logicalColumn = neoview::logicalColumnForVisual(viewState(), visualColumn);
            std::string label = questColumnLabel(logicalColumn);
            if (neoview::findColumnFilter(viewState(), logicalColumn) != nullptr) {
                label += " *";
            }
            wxListItem item;
            item.SetMask(wxLIST_MASK_TEXT);
            item.SetText(wxui::toWx(label));
            questList_->SetColumn(static_cast<int>(visualColumn), item);
        }
    }

    void updateEntryColumnLabels() {
        neoview::ensureIdentityColumns(entryViewState(), 2);
        for (std::size_t visualColumn = 0; visualColumn < 2; ++visualColumn) {
            const std::size_t logicalColumn = neoview::logicalColumnForVisual(entryViewState(), visualColumn);
            std::string label = entryColumnLabel(logicalColumn);
            if (neoview::findColumnFilter(entryViewState(), logicalColumn) != nullptr) {
                label += " *";
            }
            wxListItem item;
            item.SetMask(wxLIST_MASK_TEXT);
            item.SetText(wxui::toWx(label));
            entryList_->SetColumn(static_cast<int>(visualColumn), item);
        }
    }

    std::string questFilterCell(std::size_t questIndex, const GffStruct& quest, std::size_t logicalColumn) const {
        switch (logicalColumn) {
            case 0: return std::to_string(questIndex);
            case 1: return fieldText(quest, "Tag");
            default: return {};
        }
    }

    bool questPassesCurrentFilters(std::size_t questIndex, const GffStruct& quest) const {
        if (!viewState().filterTerm.empty()) {
            if (!matchesQuest(viewState().sortColumn, quest, viewState().filterTerm)) {
                return false;
            }
        }
        return neoview::rowPassesColumnFilters(viewState(), [&](std::size_t logicalColumn) {
            return questFilterCell(questIndex, quest, logicalColumn);
        });
    }

    std::string entryFilterCell(std::size_t entryIndex, const GffStruct& stage, std::size_t logicalColumn) const {
        switch (logicalColumn) {
            case 0: return std::to_string(entryIndex);
            case 1: return fieldText(stage, "ID");
            default: return {};
        }
    }

    bool entryPassesCurrentFilters(std::size_t entryIndex, const GffStruct& stage) const {
        return neoview::rowPassesColumnFilters(entryViewState(), [&](std::size_t logicalColumn) {
            return entryFilterCell(entryIndex, stage, logicalColumn);
        });
    }

    void appendMappedRow(wxListCtrl& list, const std::vector<std::string>& cells, const neoview::DocumentViewState& state, std::size_t logicalIndex) {
        const std::string first = neoview::logicalColumnForVisual(state, 0) < cells.size() ? cells[neoview::logicalColumnForVisual(state, 0)] : std::string();
        const long row = list.InsertItem(list.GetItemCount(), wxui::toWx(first));
        for (std::size_t visualColumn = 1; visualColumn < cells.size(); ++visualColumn) {
            const std::size_t logicalColumn = neoview::logicalColumnForVisual(state, visualColumn);
            list.SetItem(row, static_cast<int>(visualColumn), wxui::toWx(logicalColumn < cells.size() ? cells[logicalColumn] : std::string()));
        }
        list.SetItemData(row, static_cast<long>(logicalIndex));
    }

    void selectQuestIndex(std::size_t questIndex) {
        for (long row = 0; row < questList_->GetItemCount(); ++row) {
            if (static_cast<std::size_t>(questList_->GetItemData(row)) == questIndex) {
                wxui::selectRow(*questList_, row);
                return;
            }
        }
    }

    void selectEntryIndex(std::size_t entryIndex) {
        for (long row = 0; row < entryList_->GetItemCount(); ++row) {
            if (static_cast<std::size_t>(entryList_->GetItemData(row)) == entryIndex) {
                wxui::selectRow(*entryList_, row);
                return;
            }
        }
    }

    void refreshQuests() {
        updateHeaderPaths();
        questList_->DeleteAllItems();
        entryList_->DeleteAllItems();
        neoview::removeColumnFiltersOutsideRange(viewState(), 2);
        neoview::removeColumnFiltersOutsideRange(entryViewState(), 2);
        neoview::ensureIdentityColumns(viewState(), 2);
        neoview::ensureIdentityColumns(entryViewState(), 2);
        updateQuestColumnLabels();
        updateEntryColumnLabels();
        viewState().primarySelection.reset();
        viewState().secondarySelection.reset();
        clearQuestPanel();
        clearEntryPanel();
        if (!gff().loaded()) {
            neoview::setIdentityRows(viewState(), 0);
            neoview::setIdentityRows(entryViewState(), 0);
            wxui::setStatusText(*this, "No journal loaded", 0);
            wxui::setStatusText(*this, tlk().has_value() ? wxui::toWx("TLK: " + tlkPath().string()) : wxui::toWx(tlkAutoLoadWarning().empty() ? std::string("No TLK loaded") : tlkAutoLoadWarning()), 1);
            return;
        }
        const auto& categories = requireCategories(gff());
        std::vector<std::size_t> visibleQuests;
        for (std::size_t i = 0; i < categories.count(); ++i) {
            const auto* quest = categories.GetStruct(i);
            if (quest == nullptr || !questPassesCurrentFilters(i, *quest)) {
                continue;
            }
            visibleQuests.push_back(i);
            appendMappedRow(*questList_, {std::to_string(i), fieldText(*quest, "Tag")}, viewState(), i);
        }
        neoview::setRowsFromLogicalRows(viewState(), visibleQuests);
        wxui::applyTheme(questList_, darkMode_);
        std::string questStatus = "JRL: " + gff().filename().string() + "  Quests: " + std::to_string(visibleQuests.size()) + "/" + std::to_string(categories.count());
        const std::string summary = neoview::columnFilterSummary(viewState());
        if (!viewState().filterTerm.empty()) {
            questStatus += "  Search filter: " + viewState().filterTerm;
        }
        if (!summary.empty()) {
            questStatus += "  Column filters: " + summary;
        }
        wxui::setStatusText(*this, wxui::toWx(questStatus), 0);
        updateActiveTabTitle();
        wxui::setStatusText(*this, tlk().has_value() ? wxui::toWx("TLK: " + tlkPath().string()) : wxui::toWx(tlkAutoLoadWarning().empty() ? std::string("No TLK loaded") : tlkAutoLoadWarning()), 1);
        if (!visibleQuests.empty()) {
            wxui::selectRow(*questList_, 0);
            loadSelectedQuest();
        }
    }

    void refreshEntries(std::size_t questIndex) {
        entryList_->DeleteAllItems();
        neoview::removeColumnFiltersOutsideRange(entryViewState(), 2);
        neoview::ensureIdentityColumns(entryViewState(), 2);
        updateEntryColumnLabels();
        viewState().secondarySelection.reset();
        clearEntryPanel();
        const auto& categories = requireCategories(gff());
        const auto* quest = categories.GetStruct(questIndex);
        const auto* entries = quest ? questEntries(*quest) : nullptr;
        if (entries == nullptr) {
            neoview::setIdentityRows(entryViewState(), 0);
            return;
        }
        std::vector<std::size_t> visibleEntries;
        for (std::size_t i = 0; i < entries->count(); ++i) {
            const auto* stage = entries->GetStruct(i);
            if (stage == nullptr || !entryPassesCurrentFilters(i, *stage)) {
                continue;
            }
            visibleEntries.push_back(i);
            appendMappedRow(*entryList_, {std::to_string(i), fieldText(*stage, "ID")}, entryViewState(), i);
        }
        neoview::setRowsFromLogicalRows(entryViewState(), visibleEntries);
        wxui::applyTheme(entryList_, darkMode_);
        if (!visibleEntries.empty()) {
            wxui::selectRow(*entryList_, 0);
            loadSelectedEntry();
        }
    }

    void updateQuestFieldVisibility(JournalFlavor flavor) {
        const bool showKotor = flavor != JournalFlavor::NeverwinterNights;
        const bool showNwn = flavor != JournalFlavor::Kotor;
        for (wxWindow* window : {static_cast<wxWindow*>(planetLabel_), static_cast<wxWindow*>(planet_),
                                 static_cast<wxWindow*>(plotIndexLabel_), static_cast<wxWindow*>(plotIndex_)}) {
            if (window != nullptr) window->Show(showKotor);
        }
        for (wxWindow* window : {static_cast<wxWindow*>(questXpLabel_), static_cast<wxWindow*>(questXp_)}) {
            if (window != nullptr) window->Show(showNwn);
        }
        for (wxWindow* window : {static_cast<wxWindow*>(entryXpLabel_), static_cast<wxWindow*>(entryXp_)}) {
            if (window != nullptr) window->Show(showKotor);
        }
        Layout();
    }

    void clearQuestPanel() {
        profile_->Clear();
        nameStrRef_->Clear();
        name_->Clear();
        comment_->Clear();
        tag_->Clear();
        planet_->Clear();
        priority_->Clear();
        plotIndex_->Clear();
        picture_->Clear();
        questXp_->Clear();
        updateQuestFieldVisibility(JournalFlavor::Unknown);
    }

    void clearEntryPanel() {
        entryId_->Clear();
        entryXp_->Clear();
        entryStrRef_->Clear();
        entryText_->Clear();
        if (entryEnd_ != nullptr) {
            entryEnd_->SetValue(false);
        }
    }

    void loadSelectedQuest() {
        if (!gff().loaded()) {
            return;
        }
        const long row = wxui::selectedRow(*questList_);
        if (row < 0) {
            return;
        }
        try {
            const auto& categories = requireCategories(gff());
            const auto questIndex = static_cast<std::size_t>(questList_->GetItemData(row));
            const auto* quest = categories.GetStruct(questIndex);
            if (quest == nullptr) {
                return;
            }
            viewState().primarySelection = questIndex;
            viewState().secondarySelection.reset();
            JournalFlavor flavor = detectJournalQuestFlavor(gff(), questIndex);
            if (flavor == JournalFlavor::Unknown) flavor = detectJournalFlavor(gff());
            profile_->SetValue(wxui::toWx(journalFlavorDisplayName(flavor)));
            updateQuestFieldVisibility(flavor);
            nameStrRef_->SetValue(wxui::toWx(locStrRefText(*quest, "Name")));
            name_->SetValue(wxui::toWx(locResolvedText(*quest, "Name", tlk().has_value() ? &*tlk() : nullptr)));
            comment_->SetValue(wxui::toWx(fieldText(*quest, "Comment")));
            tag_->SetValue(wxui::toWx(fieldText(*quest, "Tag")));
            planet_->SetValue(wxui::toWx(fieldText(*quest, "PlanetID")));
            priority_->SetValue(wxui::toWx(fieldText(*quest, "Priority")));
            plotIndex_->SetValue(wxui::toWx(fieldText(*quest, "PlotIndex")));
            picture_->SetValue(wxui::toWx(fieldText(*quest, "Picture")));
            questXp_->SetValue(wxui::toWx(fieldText(*quest, "XP")));
            refreshEntries(*viewState().primarySelection);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void loadSelectedEntry() {
        if (!gff().loaded() || !viewState().primarySelection) {
            return;
        }
        const long row = wxui::selectedRow(*entryList_);
        if (row < 0) {
            return;
        }
        try {
            const auto entryIndex = static_cast<std::size_t>(entryList_->GetItemData(row));
            const GffStruct* stage = selectedEntry(entryIndex);
            if (stage == nullptr) {
                return;
            }
            viewState().secondarySelection = entryIndex;
            entryId_->SetValue(wxui::toWx(fieldText(*stage, "ID")));
            entryXp_->SetValue(wxui::toWx(fieldText(*stage, "XP_Percentage")));
            entryEnd_->SetValue(fieldText(*stage, "End") != "0");
            entryStrRef_->SetValue(wxui::toWx(locStrRefText(*stage, "Text")));
            entryText_->SetValue(wxui::toWx(locResolvedText(*stage, "Text", tlk().has_value() ? &*tlk() : nullptr)));
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    const GffStruct* selectedEntry(std::size_t entryIndex) const {
        if (!viewState().primarySelection) {
            return nullptr;
        }
        const auto& categories = requireCategories(gff());
        const auto* quest = categories.GetStruct(*viewState().primarySelection);
        const auto* entries = quest ? questEntries(*quest) : nullptr;
        return entries ? entries->GetStruct(entryIndex) : nullptr;
    }


    neotabular::Table currentTable() const {
        neotabular::Table table;
        table.columns = {"Kind", "QuestIndex", "EntryIndex", "Field", "Value"};
        if (!gff().loaded()) return table;
        const auto& categories = requireCategories(gff());
        for (std::size_t qi = 0; qi < categories.count(); ++qi) {
            const auto* quest = categories.GetStruct(qi);
            if (!quest) continue;
            for (const auto& fieldName : {"Tag", "Comment", "PlanetID", "Priority", "Picture", "PlotIndex", "XP"}) {
                table.rows.push_back({"Quest", std::to_string(qi), "", fieldName, fieldText(*quest, fieldName)});
            }
            table.rows.push_back({"Quest", std::to_string(qi), "", "Name(strref)", locStrRefText(*quest, "Name")});
            table.rows.push_back({"Quest", std::to_string(qi), "", "Name(lang0)", locResolvedText(*quest, "Name", tlk().has_value() ? &*tlk() : nullptr)});
            const auto* entries = questEntries(*quest);
            if (!entries) continue;
            for (std::size_t si = 0; si < entries->count(); ++si) {
                const auto* stage = entries->GetStruct(si);
                if (!stage) continue;
                for (const auto& fieldName : {"ID", "XP_Percentage", "End"}) {
                    table.rows.push_back({"Entry", std::to_string(qi), std::to_string(si), fieldName, fieldText(*stage, fieldName)});
                }
                table.rows.push_back({"Entry", std::to_string(qi), std::to_string(si), "Text(strref)", locStrRefText(*stage, "Text")});
                table.rows.push_back({"Entry", std::to_string(qi), std::to_string(si), "Text(lang0)", locResolvedText(*stage, "Text", tlk().has_value() ? &*tlk() : nullptr)});
            }
        }
        return table;
    }

    neotabular::Table selectedTable() const {
        neotabular::Table table;
        table.columns = {"Kind", "QuestIndex", "EntryIndex", "Field", "Value"};
        if (!gff().loaded() || !viewState().primarySelection) return table;
        const auto& categories = requireCategories(gff());
        const auto* quest = categories.GetStruct(*viewState().primarySelection);
        if (!quest) return table;
        for (const auto& fieldName : {"Tag", "Comment", "PlanetID", "Priority", "Picture", "PlotIndex", "XP"}) {
            table.rows.push_back({"Quest", std::to_string(*viewState().primarySelection), "", fieldName, fieldText(*quest, fieldName)});
        }
        if (viewState().secondarySelection) {
            const auto* stage = selectedEntry(*viewState().secondarySelection);
            if (stage) {
                for (const auto& fieldName : {"ID", "XP_Percentage", "End"}) {
                    table.rows.push_back({"Entry", std::to_string(*viewState().primarySelection), std::to_string(*viewState().secondarySelection), fieldName, fieldText(*stage, fieldName)});
                }
                table.rows.push_back({"Entry", std::to_string(*viewState().primarySelection), std::to_string(*viewState().secondarySelection), "Text(strref)", locStrRefText(*stage, "Text")});
                table.rows.push_back({"Entry", std::to_string(*viewState().primarySelection), std::to_string(*viewState().secondarySelection), "Text(lang0)", locResolvedText(*stage, "Text", tlk().has_value() ? &*tlk() : nullptr)});
            }
        }
        return table;
    }

    void applyTable(const neotabular::Table& table) {
        const auto kindCol = requireColumn(table, "Kind");
        const auto questCol = requireColumn(table, "QuestIndex");
        const auto fieldCol = requireColumn(table, "Field");
        const auto valueCol = requireColumn(table, "Value");
        const auto entryCol = optionalColumn(table, "EntryIndex");
        for (const auto& row : table.rows) {
            const auto kind = lowerAscii(tableCell(row, kindCol));
            const auto questText = tableCell(row, questCol);
            if (questText.empty()) continue;
            const auto questIndex = static_cast<std::size_t>(std::stoull(questText));
            const auto fieldName = tableCell(row, fieldCol);
            const auto value = tableCell(row, valueCol);
            if (kind == "quest") {
                gff().ChangeFieldValue(pathForQuest(questIndex, fieldName), value);
            } else if (kind == "entry") {
                const auto entryText = tableCell(row, entryCol);
                if (entryText.empty()) continue;
                const auto entryIndex = static_cast<std::size_t>(std::stoull(entryText));
                gff().ChangeFieldValue(pathForStage(questIndex, entryIndex, fieldName), value);
            }
        }
    }

    void onImport(neotabular::Format format) {
        try {
            ensureLoaded();
            const auto file = wxui::chooseOpenFile(this, "Import " + neotabular::formatName(format), tableWildcardForFormat(format));
            if (!file) return;
            if (format == neotabular::Format::Xml) {
                LoadGffXml(gff(), readTextFile(*file));
            } else if (format == neotabular::Format::Json) {
                LoadGffXml(gff(), gffJsonToXml(readTextFile(*file)));
            } else {
                throw std::runtime_error("NeoJRL imports only semantic XML or JSON. CSV/TSV flattened import is not supported for JRL/GFF files.");
            }
            viewState().resetForNewDocument();
            entryViewState().resetForNewDocument();
            if (searchText_) searchText_->ChangeValue("");
            refreshQuests();
            wxui::setStatusText(*this, wxui::toWx("Imported " + file->string()), 1);
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onExport(neotabular::Format format) {
        try {
            ensureLoaded();
            const auto file = wxui::chooseSaveFile(this, "Export " + neotabular::formatName(format), tableWildcardForFormat(format),
                                                  exportDefaultFilename(gff().filename(), format, "global"));
            if (!file) return;
            if (format == neotabular::Format::Xml || format == neotabular::Format::Json) {
                if (neoview::hasAnyFilter(viewState()) || neoview::hasAnyFilter(entryViewState())) {
                    throw std::runtime_error("Semantic JRL XML/JSON export preserves hierarchy and does not support row filtering. Clear quest and entry filters first.");
                }
                const std::string xml = ToGffXml(gff());
                writeTextFile(*file, format == neotabular::Format::Json ? gffXmlToJson(xml) : xml);
            } else {
                throw std::runtime_error("NeoJRL exports only semantic XML or JSON. CSV/TSV flattened export is not supported for JRL/GFF files.");
            }
            wxui::setStatusText(*this, wxui::toWx("Exported " + file->string()), 1);
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onExportPatcher(bool package) {
        try {
            ensureLoaded();
            requireJrlPatcherDocument(gff(), "The active document");
            const auto originalPath = wxui::chooseOpenFile(this, "Select clean/unmodified global.jrl", kJRLWildcard);
            if (!originalPath) return;

            GffFile original;
            original.LoadFile(*originalPath);
            requireMatchingJrlPatcherDocuments(original, gff());

            std::string defaultPatchName = gff().filename().filename().string();
            if (defaultPatchName.empty()) defaultPatchName = "global.jrl";
            const auto patchName = wxui::promptText(this,
                                                    "Patch Target Filename",
                                                    "JRL filename to patch in the user's install:",
                                                    defaultPatchName);
            if (!patchName || patchName->empty()) return;

            auto project = neotsl::diffGffFlatTable(
                jrlTableForPatcher(original), jrlTableForPatcher(gff()), *patchName, package, *originalPath);
            neotsl::throwIfUnsupported(project);

            if (package) {
                const auto outputDir = wxui::chooseDirectory(this, "Choose tslpatchdata package folder");
                if (!outputDir) return;
                neotsl::writePackage(project, *outputDir, true);
                wxui::showMessage(this,
                                  "TSL/HoloPatcher JRL Package",
                                  "Wrote changes.ini and staged the clean JRL baseline in:\n" + outputDir->string());
            } else {
                const auto output = wxui::chooseSaveFile(
                    this,
                    "Save TSL/HoloPatcher JRL fragment",
                    "INI files (*.ini)|*.ini|All files (*.*)|*.*",
                    "jrl_fragment.ini");
                if (!output) return;
                neotsl::writeFragment(project, *output);
                wxui::showMessage(this,
                                  "TSL/HoloPatcher JRL Fragment",
                                  "Wrote a GFFList fragment to:\n" + output->string());
            }
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCopyCells(wxCommandEvent&) {
        try {
            auto table = selectedTable();
            if (table.rows.empty()) return;
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(neotabular::serializeDelimited(table, '\t'))));
                wxTheClipboard->Close();
            }
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onPasteCells(wxCommandEvent&) {
        try {
            ensureLoaded();
            if (!wxTheClipboard->Open()) return;
            wxTextDataObject data;
            const bool ok = wxTheClipboard->GetData(data);
            wxTheClipboard->Close();
            if (!ok) return;
            applyTable(neotabular::parseDelimited(wxui::toStd(data.GetText()), '\t'));
            refreshQuests();
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void chooseAndOpenJrl(const std::filesystem::path& initialDirectory = {}) {
        try {
            const auto file = wxui::chooseOpenFile(
                this, "Please select a global.jrl file to open.", kJRLWildcard, initialDirectory);
            if (!file) {
                return;
            }
            openJrlPath(*file);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onOpen(wxCommandEvent&) {
        chooseAndOpenJrl();
    }

    void tryLoadCachedTlk() {
        if (tlk().has_value()) return;
        const auto cached = readCachedTlkPath();
        if (!cached || cached->empty()) return;
        try {
            if (!std::filesystem::exists(*cached)) {
                tlkAutoLoadWarning() = "Cached TLK not found: " + cached->string();
                return;
            }
            tlk().emplace();
            tlk()->load(*cached);
            tlkPath() = *cached;
            tlkAutoLoadWarning().clear();
        } catch (const std::exception& ex) {
            tlkAutoLoadWarning() = std::string("Unable to auto-load cached TLK: ") + ex.what();
        }
    }

    void onLoadTlk(wxCommandEvent&) {
        try {
            const auto file = wxui::chooseOpenFile(this, "Load dialog.tlk for optional resolved text", kTlkWildcard);
            if (!file) {
                return;
            }
            tlk().emplace();
            tlk()->load(*file);
            tlkPath() = *file;
            writeCachedTlkPath(*file);
            tlkAutoLoadWarning().clear();
            refreshQuests();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onSave(wxCommandEvent&) {
        try {
            ensureLoaded();
            gff().SaveFile();
            updateActiveTabTitle();
            rememberRecentFile(gff().filename());
            neogames::resolver().inferFromOpenedPath(gff().filename());
            refreshQuests();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onSaveAs(wxCommandEvent&) {
        try {
            ensureLoaded();
            const auto file = wxui::chooseSaveFile(this, "Save global.jrl as", kJRLWildcard, gff().filename().filename().string());
            if (!file) {
                return;
            }
            gff().SaveFile(*file);
            updateActiveTabTitle();
            rememberRecentFile(*file);
            neogames::resolver().inferFromOpenedPath(*file);
            refreshQuests();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onApplyQuest(wxCommandEvent&) {
        try {
            ensureLoaded();
            const long row = wxui::selectedRow(*questList_);
            if (row < 0) throw std::runtime_error("Select a quest first.");
            const auto index = static_cast<std::size_t>(questList_->GetItemData(row));
            JournalFlavor flavor = detectJournalQuestFlavor(gff(), index);
            if (flavor == JournalFlavor::Unknown) flavor = detectJournalFlavor(gff());

            const UInt32 nameStrRef = parseRequiredStrRef(*nameStrRef_, "Name StrRef");
            setJournalQuestLocalizedString(
                gff(), index, "Name", nameStrRef,
                nameStrRef == kNoStrRef ? std::optional<std::string>{wxui::toStd(name_->GetValue())}
                                        : std::nullopt);
            setJournalQuestString(gff(), index, "Tag", wxui::toStd(tag_->GetValue()));
            setJournalQuestOptionalString(gff(), index, "Comment", optionalText(*comment_));
            setJournalQuestOptionalDword(gff(), index, "Priority", parseOptionalDword(*priority_, "Sort priority"));
            setJournalQuestOptionalWord(gff(), index, "Picture", parseOptionalWord(*picture_, "Journal picture ID"));

            if (flavor != JournalFlavor::NeverwinterNights) {
                setJournalQuestOptionalInt(gff(), index, "PlanetID", parseOptionalInt32(*planet_, "Planet ID"));
                setJournalQuestOptionalInt(gff(), index, "PlotIndex", parseOptionalInt32(*plotIndex_, "PlotXP.2da row"));
            }
            if (flavor != JournalFlavor::Kotor) {
                setJournalQuestOptionalDword(gff(), index, "XP", parseOptionalDword(*questXp_, "Quest XP"));
            }

            refreshQuests();
            selectQuestIndex(index);
            loadSelectedQuest();
            updateActiveTabTitle();
            wxui::setStatusText(*this, "Quest changes applied.", 1);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onApplyEntry(wxCommandEvent&) {
        try {
            ensureLoaded();
            if (!viewState().primarySelection) throw std::runtime_error("Select a quest first.");
            const long row = wxui::selectedRow(*entryList_);
            if (row < 0) throw std::runtime_error("Select an entry first.");
            const auto questIndex = *viewState().primarySelection;
            const auto entryIndex = static_cast<std::size_t>(entryList_->GetItemData(row));
            JournalFlavor flavor = detectJournalQuestFlavor(gff(), questIndex);
            if (flavor == JournalFlavor::Unknown) flavor = detectJournalFlavor(gff());

            changeJournalEntryId(gff(), questIndex, entryIndex,
                                 parseRequiredDword(*entryId_, "Entry ID"));
            setJournalEntryWord(gff(), questIndex, entryIndex, "End", entryEnd_->GetValue() ? 1u : 0u);
            if (flavor != JournalFlavor::NeverwinterNights) {
                setJournalEntryOptionalFloat(gff(), questIndex, entryIndex, "XP_Percentage",
                                             parseOptionalFloat(*entryXp_, "Plot XP multiplier"));
            }

            const UInt32 textStrRef = parseRequiredStrRef(*entryStrRef_, "Entry StrRef");
            setJournalEntryLocalizedString(
                gff(), questIndex, entryIndex, "Text", textStrRef,
                textStrRef == kNoStrRef ? std::optional<std::string>{wxui::toStd(entryText_->GetValue())}
                                        : std::nullopt);

            refreshEntries(questIndex);
            selectEntryIndex(entryIndex);
            loadSelectedEntry();
            updateActiveTabTitle();
            wxui::setStatusText(*this, "Entry changes applied.", 1);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onNewEntry(wxCommandEvent&) {
        try {
            ensureLoaded();
            if (!viewState().primarySelection) {
                throw std::runtime_error("Select a quest before adding an entry.");
            }

            const std::size_t questIndex = *viewState().primarySelection;
            const bool clearedEntryFilters = neoview::hasAnyFilter(entryViewState());
            const auto added = appendJournalEntry(gff(), questIndex);
            if (clearedEntryFilters) {
                neoview::clearAllFilters(entryViewState());
            }
            refreshEntries(questIndex);
            selectEntryIndex(added.index);
            loadSelectedEntry();
            updateActiveTabTitle();
            if (entryId_ != nullptr) entryId_->SetFocus();

            std::string status = "Added journal entry ID " + std::to_string(added.entryId) + ".";
            if (clearedEntryFilters) status += " Entry filters were cleared so the new entry is visible.";
            wxui::setStatusText(*this, wxui::toWx(status), 1);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onDeleteEntry(wxCommandEvent&) {
        try {
            ensureLoaded();
            if (!viewState().primarySelection) {
                throw std::runtime_error("Select a quest before deleting an entry.");
            }
            const long row = wxui::selectedRow(*entryList_);
            if (row < 0) {
                throw std::runtime_error("Select an entry to delete.");
            }

            const std::size_t questIndex = *viewState().primarySelection;
            const std::size_t entryIndex = static_cast<std::size_t>(entryList_->GetItemData(row));
            const GffStruct* entry = selectedEntry(entryIndex);
            if (entry == nullptr) {
                throw std::runtime_error("The selected entry no longer exists.");
            }
            const std::string entryId = fieldText(*entry, "ID");
            const std::string description = entryId.empty()
                ? "entry at list index " + std::to_string(entryIndex)
                : "entry ID " + entryId;
            if (!wxui::confirm(this,
                               "Delete Entry",
                               "Delete " + description + " from the selected quest?\n\nThis changes the JRL structure and cannot be undone.")) {
                return;
            }

            const auto nextSelection = deleteJournalEntry(gff(), questIndex, entryIndex);
            refreshEntries(questIndex);
            if (nextSelection) {
                selectEntryIndex(*nextSelection);
                loadSelectedEntry();
            } else {
                clearEntryPanel();
            }
            updateActiveTabTitle();
            wxui::setStatusText(*this, wxui::toWx("Deleted " + description + "."), 1);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void applyDarkMode() {
        if (darkModeItem_ != nullptr) {
            darkModeItem_->Check(darkMode_);
        }
        wxui::applyTheme(this, darkMode_);
        if (questList_ != nullptr) {
            wxui::applyListTheme(*questList_, darkMode_);
        }
        if (entryList_ != nullptr) {
            wxui::applyListTheme(*entryList_, darkMode_);
        }
        applyFontScale();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
    }

    void changeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void onToggleDarkMode(wxCommandEvent& event) {
        darkMode_ = event.IsChecked();
        wxui::writeDarkMode(kAppName, darkMode_);
        applyDarkMode();
    }

    void onIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(1);
    }
    void onDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(-1);
    }
    void onResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }



    void onApplySearchFilter(wxCommandEvent&) {
        try {
            ensureLoaded();
            const std::string text = searchText_ ? wxui::toStd(searchText_->GetValue()) : std::string();
            if (text.empty()) {
                throw std::runtime_error("Enter search text before applying it as a filter.");
            }
            viewState().filterTerm = text;
            viewState().sortColumn = searchMode_ ? searchMode_->GetSelection() : 0;
            refreshQuests();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void clearAllFiltersAndRefresh() {
        neoview::clearAllFilters(viewState());
        neoview::clearAllFilters(entryViewState());
        if (gff().loaded()) {
            refreshQuests();
        } else {
            updateQuestColumnLabels();
            updateEntryColumnLabels();
        }
    }

    void onClearFilter(wxCommandEvent&) { clearAllFiltersAndRefresh(); }
    void onClearAllFilters(wxCommandEvent&) { clearAllFiltersAndRefresh(); }

    void promptColumnFilter(bool entryList, int visualColumn) {
        ensureLoaded();
        auto& state = entryList ? entryViewState() : viewState();
        neoview::ensureIdentityColumns(state, 2);
        const std::size_t logicalColumn = neoview::logicalColumnForVisual(state, static_cast<std::size_t>(std::max(0, visualColumn)));
        const auto* existing = neoview::findColumnFilter(state, logicalColumn);
        const std::string prior = existing != nullptr ? existing->term : std::string();
        const std::string label = entryList ? entryColumnLabel(logicalColumn) : questColumnLabel(logicalColumn);
        const auto term = wxui::promptText(this, entryList ? "Entry Column Filter" : "Quest Column Filter", "Show rows where " + label + " contains:", prior);
        if (!term) return;
        if (neoview::trimmedCopy(*term).empty()) {
            neoview::clearColumnFilter(state, logicalColumn);
        } else {
            neoview::setColumnFilter(state, neoview::ColumnFilter{logicalColumn, label, *term, neoview::TextFilterMode::Contains, true});
        }
        if (entryList && viewState().primarySelection) {
            refreshEntries(*viewState().primarySelection);
        } else {
            refreshQuests();
        }
    }

    void onFilterSelectedColumn(wxCommandEvent&) {
        try { promptColumnFilter(contextListIsEntry_, contextVisualColumn_); } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onClearSelectedColumnFilter(wxCommandEvent&) {
        auto& state = contextListIsEntry_ ? entryViewState() : viewState();
        neoview::ensureIdentityColumns(state, 2);
        const std::size_t logicalColumn = neoview::logicalColumnForVisual(state, static_cast<std::size_t>(std::max(0, contextVisualColumn_)));
        neoview::clearColumnFilter(state, logicalColumn);
        if (contextListIsEntry_ && viewState().primarySelection) {
            refreshEntries(*viewState().primarySelection);
        } else {
            refreshQuests();
        }
    }

    void onResetColumnOrder(wxCommandEvent&) {
        neoview::setIdentityColumns(viewState(), 2);
        neoview::setIdentityColumns(entryViewState(), 2);
        if (gff().loaded()) refreshQuests(); else { updateQuestColumnLabels(); updateEntryColumnLabels(); }
    }

    void onResetRowOrder(wxCommandEvent&) {
        if (gff().loaded()) refreshQuests();
    }

    void onQuestColumnRightClick(wxListEvent& event) {
        contextListIsEntry_ = false;
        contextVisualColumn_ = event.GetColumn();
        wxMenu menu;
        menu.Append(ID_FilterColumn, "Filter This Quest Column...");
        menu.Append(ID_ClearColumnFilter, "Clear Filter on This Quest Column");
        menu.AppendSeparator();
        menu.Append(ID_ClearAllFilters, "Clear All Filters");
        PopupMenu(&menu);
    }

    void onEntryColumnRightClick(wxListEvent& event) {
        contextListIsEntry_ = true;
        contextVisualColumn_ = event.GetColumn();
        wxMenu menu;
        menu.Append(ID_FilterColumn, "Filter This Entry Column...");
        menu.Append(ID_ClearColumnFilter, "Clear Filter on This Entry Column");
        menu.AppendSeparator();
        menu.Append(ID_ClearAllFilters, "Clear All Filters");
        PopupMenu(&menu);
    }

    void onSearch(wxCommandEvent&) {
        try {
            ensureLoaded();
            const std::string text = wxui::toStd(searchText_->GetValue());
            if (text.empty()) {
                throw std::runtime_error("Enter search text first.");
            }
            const int mode = searchMode_->GetSelection();
            const auto startQuest = viewState().primarySelection.value_or(static_cast<std::size_t>(-1));
            if (searchFrom(mode, text, startQuest + 1) || searchFrom(mode, text, 0, startQuest + 1)) {
                return;
            }
            wxui::showMessage(this, "Search", "No results matching the specified criteria were found.");
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    bool searchFrom(int mode, const std::string& text, std::size_t begin, std::optional<std::size_t> end = std::nullopt) {
        const auto& categories = requireCategories(gff());
        const std::size_t stop = std::min(end.value_or(categories.count()), categories.count());
        for (std::size_t qi = begin; qi < stop; ++qi) {
            const auto* quest = categories.GetStruct(qi);
            if (quest == nullptr) {
                continue;
            }
            if (matchesQuest(mode, *quest, text)) {
                selectQuestIndex(qi);
                loadSelectedQuest();
                return true;
            }
            if (mode == 2) {
                const auto* entries = questEntries(*quest);
                if (entries == nullptr) {
                    continue;
                }
                for (std::size_t si = 0; si < entries->count(); ++si) {
                    const auto* stage = entries->GetStruct(si);
                    if (stage != nullptr && containsInsensitive(locResolvedText(*stage, "Text", tlk().has_value() ? &*tlk() : nullptr), text)) {
                        selectQuestIndex(qi);
                        loadSelectedQuest();
                        selectEntryIndex(si);
                        loadSelectedEntry();
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool matchesQuest(int mode, const GffStruct& quest, const std::string& text) const {
        switch (mode) {
            case 0: return containsInsensitive(fieldText(quest, "Tag"), text);
            case 1: return containsInsensitive(locResolvedText(quest, "Name", tlk().has_value() ? &*tlk() : nullptr), text);
            case 3: return containsInsensitive(fieldText(quest, "PlanetID"), text);
            case 4: return containsInsensitive(fieldText(quest, "Priority"), text);
            case 5: return containsInsensitive(fieldText(quest, "PlotIndex"), text);
            case 6: return containsInsensitive(fieldText(quest, "Picture"), text);
            case 7: return containsInsensitive(fieldText(quest, "XP"), text);
            default: return false;
        }
    }

    void onCloseTab(wxCommandEvent&) { closeDocumentTab(activeDocumentIndex_); }

    void onCloseOtherTabs(wxCommandEvent&) {
        if (!hasActiveDocument()) return;
        for (std::size_t i = documents_.size(); i-- > 0;) {
            if (i != activeDocumentIndex_ && !closeDocumentTab(i)) return;
        }
    }

    void onNextTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(true);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onPreviousTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(false);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onClose(wxCloseEvent& event) {
        if (event.CanVeto() && !confirmCloseAllTabs()) {
            event.Veto();
            return;
        }
        settings_.saveWindowPlacement(*this);
        event.Skip();
    }

    neosettings::AppSettings settings_{kAppName};
    wxMenu* recentFilesMenu_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;
    wxListCtrl* questList_ = nullptr;
    wxListCtrl* entryList_ = nullptr;
    wxChoice* searchMode_ = nullptr;
    wxTextCtrl* searchText_ = nullptr;
    wxTextCtrl* filePath_ = nullptr;
    wxTextCtrl* tlkPathText_ = nullptr;
    wxStaticText* profileLabel_ = nullptr;
    wxTextCtrl* profile_ = nullptr;
    wxTextCtrl* nameStrRef_ = nullptr;
    wxTextCtrl* name_ = nullptr;
    wxTextCtrl* comment_ = nullptr;
    wxTextCtrl* tag_ = nullptr;
    wxStaticText* planetLabel_ = nullptr;
    wxTextCtrl* planet_ = nullptr;
    wxTextCtrl* priority_ = nullptr;
    wxTextCtrl* picture_ = nullptr;
    wxStaticText* plotIndexLabel_ = nullptr;
    wxTextCtrl* plotIndex_ = nullptr;
    wxStaticText* questXpLabel_ = nullptr;
    wxTextCtrl* questXp_ = nullptr;
    wxTextCtrl* entryId_ = nullptr;
    wxStaticText* entryXpLabel_ = nullptr;
    wxTextCtrl* entryXp_ = nullptr;
    wxCheckBox* entryEnd_ = nullptr;
    wxTextCtrl* entryStrRef_ = nullptr;
    wxTextCtrl* entryText_ = nullptr;
    wxAuiNotebook* documentTabs_ = nullptr;
    std::vector<DocumentTab> documents_;
    std::size_t activeDocumentIndex_ = neotabs::npos;
    bool tabSwitchInProgress_ = false;
    int contextVisualColumn_ = 0;
    bool contextListIsEntry_ = false;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};

class NeoJRLApp final : public wxApp {
public:
    bool OnInit() override {
#if wxCHECK_VERSION(3, 3, 0)
        SetAppearance(Appearance::System);
#endif
        auto* frame = new NeoJRLFrame;
        frame->Show(true);
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoJRLApp);
