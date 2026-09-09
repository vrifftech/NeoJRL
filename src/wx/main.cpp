#include "JRLEditorPanel.hpp"
#include "core/Version.hpp"
#include "neojrl_icon.xpm"
#include "NeoSettings.hpp"
#include <wx/app.h>
#include <wx/iconbndl.h>
namespace {
class NeoJRLFrame final : public wxFrame {
public:
    NeoJRLFrame():wxFrame(nullptr,wxID_ANY,wxui::toWx(std::string("NeoJRL v")+neojrl::kVersion)) {
        wxIconBundle icons;
#if defined(__WXMSW__)
        wxIcon native("neojrl",wxBITMAP_TYPE_ICO_RESOURCE);if(native.IsOk())icons.AddIcon(native);
#endif
        wxIcon fallback(neojrl_icon_xpm);if(fallback.IsOk())icons.AddIcon(fallback);
        SetIcons(icons);
        neomodules::Context context;
        context.titleChanged=[this](const wxString& title){SetTitle(title);};
        context.closeRequested=[this]{Close();};
        panel_=neojrl::ui::createEditorPanel(this,std::move(context));
        SetMenuBar(panel_->takeMenus().release());
        auto* layout=new wxBoxSizer(wxVERTICAL);layout->Add(panel_,1,wxEXPAND);SetSizer(layout);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){if(!neomodules::routeCommand({panel_},event))event.Skip();});
        Bind(wxEVT_MENU_OPEN,[this](wxMenuEvent& event){neomodules::routeMenuOpen({panel_},event);event.Skip();});
        Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& event){
            if(!panel_->canClose()){if(event.CanVeto()){event.Veto();return;}}
            settings_.saveWindowPlacement(*this);event.Skip();
        });
        wxui::configureResponsiveWindow(*this,wxSize(1050,720),wxSize(620,420));
        settings_.restoreWindowPlacement(*this);
    }
    ~NeoJRLFrame() override {DestroyChildren();}
    void openStartup(const std::filesystem::path& path){try{panel_->openFile(path);}catch(const std::exception& ex){wxui::showError(this,ex);}}
private:
    neojrl::ui::JRLEditorPanel* panel_{};
    neosettings::AppSettings settings_{"NeoJRL"};
};
class NeoJRLApp final:public wxApp {
public:bool OnInit()override {
    SetAppName("NeoJRL");SetVendorName("Neo Tools");wxInitAllImageHandlers();
    auto* frame=new NeoJRLFrame;frame->Show();
    if(argc>1){const auto path=neosettings::pathFromWx(wxString(argv[1]));frame->CallAfter([frame,path]{frame->openStartup(path);});}
    return true;
}};
}
wxIMPLEMENT_APP(NeoJRLApp);
