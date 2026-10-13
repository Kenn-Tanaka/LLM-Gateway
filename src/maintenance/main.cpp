#include "maintenance/main_frame.hpp"
#include <wx/wx.h>
#include <wx/cmdline.h>

namespace maintenance {

class MaintenanceApp : public wxApp {
public:
    void OnInitCmdLine(wxCmdLineParser& parser) override {
        wxApp::OnInitCmdLine(parser);
        parser.AddOption("", "config", "Configuration file (legacy or version 2.0)", wxCMD_LINE_VAL_STRING);
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override {
        if (!wxApp::OnCmdLineParsed(parser)) return false;
        wxString value;
        if (parser.Found("config", &value)) config_path_ = std::filesystem::path(value.ToStdWstring());
        return true;
    }

    bool OnInit() override {
        if (!wxApp::OnInit()) return false;

        try {
            if (config_path_ && !std::filesystem::is_regular_file(*config_path_)) throw std::runtime_error("The specified configuration file does not exist.");
            MainFrame* frame = new MainFrame(config_path_);
            frame->Show(true);
            SetTopWindow(frame);
            return true;
        } catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), "Configuration Error", wxOK | wxICON_ERROR);
            return false;
        }
    }
private:
    std::optional<std::filesystem::path> config_path_;
};

} // namespace maintenance

wxIMPLEMENT_APP(maintenance::MaintenanceApp);
