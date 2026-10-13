#include "maintenance/main_frame.hpp"

#include <wx/statline.h>
#include <wx/msgdlg.h>
#include <wx/filedlg.h>
#include <thread>

namespace maintenance {

namespace {

std::filesystem::path get_gateway_exe_path() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
    return p.parent_path() / "llm-gateway.exe";
}

std::string trim_ascii(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

MainFrame::MainFrame(std::optional<std::filesystem::path> config_path)
    : wxFrame(nullptr, wxID_ANY, "LLM Gateway Maintenance Tool (v2.0)", wxDefaultPosition, wxSize(980, 760)),
      config_store_(std::move(config_path)),
      current_config_(config_store_.load()),
      saved_config_(current_config_),
      config_exists_on_disk_(config_store_.exists_on_disk()),
      cred_store_(current_config_.credentials.service_name, current_config_.credentials.environment_fallback),
      cred_editor_(cred_store_),
      process_controller_(get_gateway_exe_path(), cred_store_),
      log_reader_(gateway::path_from_utf8(gateway::expand_environment_variables(current_config_.logging.dir)),
          current_config_.logging.file ? get_gateway_exe_path().parent_path() / gateway::path_from_utf8(gateway::expand_environment_variables(*current_config_.logging.file)) : std::filesystem::path{}) {

    init_ui();
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);

    // Start background timers
    health_timer_.Bind(wxEVT_TIMER, &MainFrame::on_health_timer, this);
    health_timer_.Start(500);

    log_timer_.Bind(wxEVT_TIMER, &MainFrame::on_log_timer, this);
    log_timer_.Start(500);

    // Initial status and logs
    process_controller_.set_active_admin_credential_name(current_config_.security.admin_credential_name);
    if (process_controller_.reattach_gateway(current_config_.listen.port) ||
        health_client_.check_health(current_config_.listen.port, 200).has_value()) {
        applied_config_ = current_config_;
        process_controller_.set_active_port(current_config_.listen.port);
    }
    refresh_settings_ui();
    refresh_providers_ui();
    refresh_credentials_ui();
    refresh_status();
    update_config_state_banner();

    // Initial tail logs
    append_log_lines(log_reader_.read_initial_tail(262144)); // 256KB initial
}

MainFrame::~MainFrame() {
    ui_alive_->store(false, std::memory_order_release);
    health_timer_.Stop();
    log_timer_.Stop();
    process_controller_.request_cancel();
    join_restart_worker();
}

void MainFrame::join_restart_worker() {
    if (restart_worker_.joinable()) {
        restart_worker_.join();
    }
}

bool MainFrame::has_unsaved_changes() const {
    if (!config_exists_on_disk_) {
        return true;
    }
    return !ConfigStore::configs_equal(current_config_, saved_config_);
}

bool MainFrame::has_unapplied_changes() const {
    if (!applied_config_.has_value()) {
        return false;
    }
    return !ConfigStore::configs_equal(current_config_, *applied_config_);
}

uint16_t MainFrame::effective_running_port() const {
    uint16_t act = process_controller_.active_port();
    if (act != 0) return act;
    if (applied_config_.has_value()) return applied_config_->listen.port;
    return current_config_.listen.port;
}

void MainFrame::update_config_state_banner() {
    if (!config_path_text_) return;

    std::string state_desc;
    if (!config_exists_on_disk_) {
        state_desc = "[New Config - Not Saved Yet]";
    } else if (has_unsaved_changes()) {
        state_desc = current_config_.loaded_from_legacy
            ? "[Unsaved Changes | Legacy Format (Converts to v2.0 on Save)]"
            : "[Unsaved Changes]";
    } else if (current_config_.loaded_from_legacy) {
        state_desc = "[Loaded from Legacy Format (Converts to v2.0 on Save)]";
    } else if (has_unapplied_changes()) {
        state_desc = "[Saved on Disk | Restart Gateway to Apply]";
    } else if (applied_config_.has_value()) {
        state_desc = "[Saved & Applied]";
    } else {
        state_desc = "[Saved]";
    }

    std::string cfg_display = "Config Path: " + gateway::path_to_utf8(config_store_.current_path()) + "   " + state_desc;
    config_path_text_->SetLabel(wxString::FromUTF8(cfg_display.c_str()));

    std::string title = "LLM Gateway Maintenance Tool (v2.0)";
    if (has_unsaved_changes()) {
        title += " *";
    }
    SetTitle(wxString::FromUTF8(title.c_str()));

    if (port_text_) {
        uint16_t run_port = effective_running_port();
        if (applied_config_.has_value() && run_port != current_config_.listen.port) {
            port_text_->SetLabel("Port: " + std::to_string(run_port) + " (Next: " + std::to_string(current_config_.listen.port) + ")");
        } else {
            port_text_->SetLabel("Port: " + std::to_string(current_config_.listen.port));
        }
    }
}

void MainFrame::init_ui() {
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    main_sizer->Add(create_header_panel(this), 0, wxEXPAND | wxALL, 5);
    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    notebook_ = new wxNotebook(this, wxID_ANY);
    notebook_->AddPage(create_providers_tab(notebook_), "Providers & Routing");
    notebook_->AddPage(create_credentials_tab(notebook_), "Credentials");
    notebook_->AddPage(create_logs_tab(notebook_), "Logs");

    main_sizer->Add(notebook_, 1, wxEXPAND | wxALL, 5);
    SetSizer(main_sizer);
}

wxPanel* MainFrame::create_header_panel(wxWindow* parent) {
    wxPanel* panel = new wxPanel(parent);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* top_row = new wxBoxSizer(wxHORIZONTAL);
    status_indicator_ = new wxStaticText(panel, wxID_ANY, "[ Stopped ]");
    status_indicator_->SetFont(status_indicator_->GetFont().Bold());

    status_text_ = new wxStaticText(panel, wxID_ANY, "Status: Not Running");
    pid_text_ = new wxStaticText(panel, wxID_ANY, "PID: -");
    port_text_ = new wxStaticText(panel, wxID_ANY, "Port: " + std::to_string(current_config_.listen.port));
    embedded_text_ = new wxStaticText(panel, wxID_ANY, "Embedded: -");

    top_row->Add(status_indicator_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);
    top_row->Add(status_text_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);
    top_row->Add(pid_text_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);
    top_row->Add(port_text_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);
    top_row->Add(embedded_text_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);
    top_row->AddStretchSpacer();

    btn_open_config_ = new wxButton(panel, wxID_ANY, "Open Config...");
    btn_start_ = new wxButton(panel, wxID_ANY, "Start");
    btn_stop_ = new wxButton(panel, wxID_ANY, "Stop");
    btn_save_ = new wxButton(panel, wxID_ANY, "Save Config");
    btn_restart_ = new wxButton(panel, wxID_ANY, "Save & Restart");

    btn_open_config_->Bind(wxEVT_BUTTON, &MainFrame::on_open_config, this);
    btn_start_->Bind(wxEVT_BUTTON, &MainFrame::on_start, this);
    btn_stop_->Bind(wxEVT_BUTTON, &MainFrame::on_stop, this);
    btn_save_->Bind(wxEVT_BUTTON, &MainFrame::on_save, this);
    btn_restart_->Bind(wxEVT_BUTTON, &MainFrame::on_restart, this);

    top_row->Add(btn_open_config_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
    top_row->Add(btn_start_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
    top_row->Add(btn_stop_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
    top_row->Add(btn_save_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
    top_row->Add(btn_restart_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);

    sizer->Add(top_row, 0, wxEXPAND | wxALL, 5);

    std::string cfg_display = "Config Path: " + gateway::path_to_utf8(config_store_.current_path());
    config_path_text_ = new wxStaticText(panel, wxID_ANY, wxString::FromUTF8(cfg_display.c_str()));
    config_path_text_->SetForegroundColour(*wxLIGHT_GREY);
    sizer->Add(config_path_text_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::create_providers_tab(wxWindow* parent) {
    wxPanel* panel = new wxPanel(parent);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    // 1. Gateway & Embedded Settings
    wxStaticText* gw_lbl = new wxStaticText(panel, wxID_ANY, "Gateway & Embedded Engine Settings:");
    gw_lbl->SetFont(gw_lbl->GetFont().Bold());
    sizer->Add(gw_lbl, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    wxBoxSizer* gw_row1 = new wxBoxSizer(wxHORIZONTAL);
    gw_row1->Add(new wxStaticText(panel, wxID_ANY, "Listen Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    txt_listen_port_ = new wxTextCtrl(panel, wxID_ANY, std::to_string(current_config_.listen.port), wxDefaultPosition, wxSize(65, -1));
    gw_row1->Add(txt_listen_port_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    chk_client_auth_ = new wxCheckBox(panel, wxID_ANY, "Require Client Auth");
    chk_client_auth_->SetValue(current_config_.security.client_auth);
    gw_row1->Add(chk_client_auth_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    chk_cors_enabled_ = new wxCheckBox(panel, wxID_ANY, "Enable CORS");
    chk_cors_enabled_->SetValue(current_config_.security.cors_enabled);
    gw_row1->Add(chk_cors_enabled_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    gw_row1->Add(new wxStaticText(panel, wxID_ANY, "Allowed Origins (CSV):"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    txt_cors_origins_ = new wxTextCtrl(panel, wxID_ANY, "");
    gw_row1->Add(txt_cors_origins_, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(gw_row1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

    wxBoxSizer* gw_row2 = new wxBoxSizer(wxHORIZONTAL);
    chk_embedded_enabled_ = new wxCheckBox(panel, wxID_ANY, "Enable Embedded Engine");
    chk_embedded_enabled_->SetValue(current_config_.embedded_engine.enabled);
    gw_row2->Add(chk_embedded_enabled_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    gw_row2->Add(new wxStaticText(panel, wxID_ANY, "Embedding GGUF:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    txt_embedding_path_ = new wxTextCtrl(panel, wxID_ANY, "");
    gw_row2->Add(txt_embedding_path_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    btn_browse_embedding_ = new wxButton(panel, wxID_ANY, "Browse...", wxDefaultPosition, wxSize(75, -1));
    btn_browse_embedding_->Bind(wxEVT_BUTTON, &MainFrame::on_browse_embedding, this);
    gw_row2->Add(btn_browse_embedding_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    gw_row2->Add(new wxStaticText(panel, wxID_ANY, "Reranker GGUF:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    txt_reranker_path_ = new wxTextCtrl(panel, wxID_ANY, "");
    gw_row2->Add(txt_reranker_path_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    btn_browse_reranker_ = new wxButton(panel, wxID_ANY, "Browse...", wxDefaultPosition, wxSize(75, -1));
    btn_browse_reranker_->Bind(wxEVT_BUTTON, &MainFrame::on_browse_reranker, this);
    gw_row2->Add(btn_browse_reranker_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    btn_apply_settings_ = new wxButton(panel, wxID_ANY, "Apply Settings");
    btn_apply_settings_->Bind(wxEVT_BUTTON, &MainFrame::on_apply_settings, this);
    gw_row2->Add(btn_apply_settings_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(gw_row2, 0, wxEXPAND | wxALL, 5);

    chk_embedded_enabled_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (is_busy_ || suppress_field_events_) return;
        current_config_.embedded_engine.enabled = chk_embedded_enabled_->GetValue();
        update_config_state_banner();
    });
    chk_client_auth_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (is_busy_ || suppress_field_events_) return;
        current_config_.security.client_auth = chk_client_auth_->GetValue();
        update_config_state_banner();
    });
    chk_cors_enabled_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (is_busy_ || suppress_field_events_) return;
        current_config_.security.cors_enabled = chk_cors_enabled_->GetValue();
        update_config_state_banner();
    });

    sizer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    // 2. Configured Providers & Editor
    wxBoxSizer* prov_hdr = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* prov_lbl = new wxStaticText(panel, wxID_ANY, "Configured Providers:");
    prov_lbl->SetFont(prov_lbl->GetFont().Bold());
    prov_hdr->Add(prov_lbl, 0, wxALIGN_CENTER_VERTICAL);
    prov_hdr->AddStretchSpacer();

    btn_preset_openrouter_ = new wxButton(panel, wxID_ANY, "+ OpenRouter Preset");
    btn_preset_lmstudio_ = new wxButton(panel, wxID_ANY, "+ LM Studio Preset");
    btn_preset_openrouter_->Bind(wxEVT_BUTTON, &MainFrame::on_preset_openrouter, this);
    btn_preset_lmstudio_->Bind(wxEVT_BUTTON, &MainFrame::on_preset_lmstudio, this);
    prov_hdr->Add(btn_preset_openrouter_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    prov_hdr->Add(btn_preset_lmstudio_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(prov_hdr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

    provider_list_ = new wxListView(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    provider_list_->AppendColumn("Provider ID", wxLIST_FORMAT_LEFT, 110);
    provider_list_->AppendColumn("Base URL", wxLIST_FORMAT_LEFT, 240);
    provider_list_->AppendColumn("Auth Mode", wxLIST_FORMAT_LEFT, 80);
    provider_list_->AppendColumn("Key Vendor/Slot", wxLIST_FORMAT_LEFT, 130);
    provider_list_->AppendColumn("Discovery", wxLIST_FORMAT_LEFT, 75);
    provider_list_->AppendColumn("Allowlist", wxLIST_FORMAT_LEFT, 180);
    provider_list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MainFrame::on_provider_selected, this);
    sizer->Add(provider_list_, 1, wxEXPAND | wxALL, 5);

    wxBoxSizer* prov_edit_row1 = new wxBoxSizer(wxHORIZONTAL);
    prov_edit_row1->Add(new wxStaticText(panel, wxID_ANY, "ID:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_prov_id_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(100, -1));
    prov_edit_row1->Add(txt_prov_id_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    prov_edit_row1->Add(new wxStaticText(panel, wxID_ANY, "Base URL:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_prov_url_ = new wxTextCtrl(panel, wxID_ANY, "");
    prov_edit_row1->Add(txt_prov_url_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    prov_edit_row1->Add(new wxStaticText(panel, wxID_ANY, "Auth:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    wxArrayString auth_choices;
    auth_choices.Add("bearer");
    auth_choices.Add("none");
    choice_prov_auth_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxSize(80, -1), auth_choices);
    choice_prov_auth_->SetSelection(0);
    prov_edit_row1->Add(choice_prov_auth_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    chk_prov_discovery_ = new wxCheckBox(panel, wxID_ANY, "Model Discovery");
    chk_prov_discovery_->SetValue(true);
    prov_edit_row1->Add(chk_prov_discovery_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(prov_edit_row1, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    wxBoxSizer* prov_edit_row2 = new wxBoxSizer(wxHORIZONTAL);
    prov_edit_row2->Add(new wxStaticText(panel, wxID_ANY, "Vendor:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_prov_vendor_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(95, -1));
    prov_edit_row2->Add(txt_prov_vendor_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    prov_edit_row2->Add(new wxStaticText(panel, wxID_ANY, "Slot:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_prov_slot_ = new wxTextCtrl(panel, wxID_ANY, "default", wxDefaultPosition, wxSize(80, -1));
    prov_edit_row2->Add(txt_prov_slot_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    prov_edit_row2->Add(new wxStaticText(panel, wxID_ANY, "Allowlist (CSV):"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_prov_allowlist_ = new wxTextCtrl(panel, wxID_ANY, "*");
    prov_edit_row2->Add(txt_prov_allowlist_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    btn_upsert_provider_ = new wxButton(panel, wxID_ANY, "Add / Update Provider");
    btn_remove_provider_ = new wxButton(panel, wxID_ANY, "Remove Provider");
    btn_upsert_provider_->Bind(wxEVT_BUTTON, &MainFrame::on_upsert_provider, this);
    btn_remove_provider_->Bind(wxEVT_BUTTON, &MainFrame::on_remove_provider, this);
    prov_edit_row2->Add(btn_upsert_provider_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    prov_edit_row2->Add(btn_remove_provider_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(prov_edit_row2, 0, wxEXPAND | wxALL, 5);

    sizer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    // 3. Configured Model Aliases & Editor
    wxStaticText* alias_lbl = new wxStaticText(panel, wxID_ANY, "Configured Model Aliases:");
    alias_lbl->SetFont(alias_lbl->GetFont().Bold());
    sizer->Add(alias_lbl, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    alias_list_ = new wxListView(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    alias_list_->AppendColumn("Alias Name", wxLIST_FORMAT_LEFT, 150);
    alias_list_->AppendColumn("Target Provider", wxLIST_FORMAT_LEFT, 120);
    alias_list_->AppendColumn("Target Model", wxLIST_FORMAT_LEFT, 220);
    alias_list_->AppendColumn("Input Prefix", wxLIST_FORMAT_LEFT, 250);
    alias_list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MainFrame::on_alias_selected, this);
    sizer->Add(alias_list_, 1, wxEXPAND | wxALL, 5);

    wxBoxSizer* alias_edit_row = new wxBoxSizer(wxHORIZONTAL);
    alias_edit_row->Add(new wxStaticText(panel, wxID_ANY, "Alias:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_alias_name_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(110, -1));
    alias_edit_row->Add(txt_alias_name_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    alias_edit_row->Add(new wxStaticText(panel, wxID_ANY, "Provider:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_alias_provider_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(95, -1));
    alias_edit_row->Add(txt_alias_provider_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    alias_edit_row->Add(new wxStaticText(panel, wxID_ANY, "Model:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_alias_model_ = new wxTextCtrl(panel, wxID_ANY, "");
    alias_edit_row->Add(txt_alias_model_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    alias_edit_row->Add(new wxStaticText(panel, wxID_ANY, "Prefix:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
    txt_alias_prefix_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(110, -1));
    alias_edit_row->Add(txt_alias_prefix_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    btn_upsert_alias_ = new wxButton(panel, wxID_ANY, "Add / Update Alias");
    btn_remove_alias_ = new wxButton(panel, wxID_ANY, "Remove Alias");
    btn_upsert_alias_->Bind(wxEVT_BUTTON, &MainFrame::on_upsert_alias, this);
    btn_remove_alias_->Bind(wxEVT_BUTTON, &MainFrame::on_remove_alias, this);
    alias_edit_row->Add(btn_upsert_alias_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    alias_edit_row->Add(btn_remove_alias_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(alias_edit_row, 0, wxEXPAND | wxALL, 5);

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::create_credentials_tab(wxWindow* parent) {
    wxPanel* panel = new wxPanel(parent);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* cred_lbl = new wxStaticText(panel, wxID_ANY, "Registered Provider API Keys (Windows Credential Manager):");
    cred_lbl->SetFont(cred_lbl->GetFont().Bold());
    sizer->Add(cred_lbl, 0, wxALL, 5);

    cred_list_ = new wxListView(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    cred_list_->AppendColumn("Vendor", wxLIST_FORMAT_LEFT, 120);
    cred_list_->AppendColumn("Slot", wxLIST_FORMAT_LEFT, 100);
    cred_list_->AppendColumn("Target Name", wxLIST_FORMAT_LEFT, 250);
    cred_list_->AppendColumn("Status", wxLIST_FORMAT_LEFT, 120);
    sizer->Add(cred_list_, 1, wxEXPAND | wxALL, 5);

    wxBoxSizer* update_row = new wxBoxSizer(wxHORIZONTAL);
    update_row->Add(new wxStaticText(panel, wxID_ANY, "New Key / Value:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    txt_new_secret_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    update_row->Add(txt_new_secret_, 1, wxEXPAND | wxRIGHT, 5);

    btn_save_secret_ = new wxButton(panel, wxID_ANY, "Save Key");
    btn_delete_secret_ = new wxButton(panel, wxID_ANY, "Delete Key");
    btn_save_secret_->Bind(wxEVT_BUTTON, &MainFrame::on_save_secret, this);
    btn_delete_secret_->Bind(wxEVT_BUTTON, &MainFrame::on_delete_secret, this);

    update_row->Add(btn_save_secret_, 0, wxRIGHT, 5);
    update_row->Add(btn_delete_secret_, 0);
    sizer->Add(update_row, 0, wxEXPAND | wxALL, 5);

    sizer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxALL, 5);

    wxBoxSizer* token_row = new wxBoxSizer(wxHORIZONTAL);
    btn_gen_client_token_ = new wxButton(panel, wxID_ANY, "Generate & Copy Client Token");
    btn_gen_admin_token_ = new wxButton(panel, wxID_ANY, "Generate & Copy Admin Token");
    btn_gen_client_token_->Bind(wxEVT_BUTTON, &MainFrame::on_gen_client_token, this);
    btn_gen_admin_token_->Bind(wxEVT_BUTTON, &MainFrame::on_gen_admin_token, this);

    token_row->Add(btn_gen_client_token_, 0, wxRIGHT, 10);
    token_row->Add(btn_gen_admin_token_, 0);
    sizer->Add(token_row, 0, wxALL, 5);

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::create_logs_tab(wxWindow* parent) {
    wxPanel* panel = new wxPanel(parent);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    log_viewer_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
    log_viewer_->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));

    sizer->Add(log_viewer_, 1, wxEXPAND | wxALL, 5);
    panel->SetSizer(sizer);
    return panel;
}

void MainFrame::refresh_settings_ui() {
    suppress_field_events_ = true;
    if (txt_listen_port_) {
        txt_listen_port_->SetValue(std::to_string(current_config_.listen.port));
    }
    if (chk_client_auth_) {
        chk_client_auth_->SetValue(current_config_.security.client_auth);
    }
    if (chk_cors_enabled_) {
        chk_cors_enabled_->SetValue(current_config_.security.cors_enabled);
    }
    if (txt_cors_origins_) {
        std::string origins = ConfigStore::join_csv_tokens(current_config_.security.allowed_origins);
        txt_cors_origins_->SetValue(wxString::FromUTF8(origins.c_str()));
    }
    if (chk_embedded_enabled_) {
        chk_embedded_enabled_->SetValue(current_config_.embedded_engine.enabled);
    }
    if (txt_embedding_path_) {
        std::string emb_path = current_config_.embedded_engine.embedding_model
            ? current_config_.embedded_engine.embedding_model->path
            : "";
        txt_embedding_path_->SetValue(wxString::FromUTF8(emb_path.c_str()));
    }
    if (txt_reranker_path_) {
        std::string rer_path = current_config_.embedded_engine.reranker_model
            ? current_config_.embedded_engine.reranker_model->path
            : "";
        txt_reranker_path_->SetValue(wxString::FromUTF8(rer_path.c_str()));
    }
    suppress_field_events_ = false;
}

bool MainFrame::sync_ui_settings_to_config(std::string* error_out) {
    gateway::GatewayConfig candidate = current_config_;

    if (txt_listen_port_) {
        std::string port_str = trim_ascii(std::string(txt_listen_port_->GetValue().ToUTF8()));
        try {
            size_t idx = 0;
            long val = std::stol(port_str, &idx, 10);
            if (idx != port_str.size() || val < 1024 || val > 65535) {
                if (error_out) *error_out = "Listen port must be an integer between 1024 and 65535.";
                return false;
            }
            candidate.listen.port = static_cast<uint16_t>(val);
        } catch (...) {
            if (error_out) *error_out = "Listen port must be an integer between 1024 and 65535.";
            return false;
        }
    }

    if (chk_client_auth_) {
        candidate.security.client_auth = chk_client_auth_->GetValue();
    }
    if (chk_cors_enabled_) {
        candidate.security.cors_enabled = chk_cors_enabled_->GetValue();
    }
    if (txt_cors_origins_) {
        std::string origins_raw = std::string(txt_cors_origins_->GetValue().ToUTF8());
        candidate.security.allowed_origins = ConfigStore::split_csv_tokens(origins_raw);
    }
    if (chk_embedded_enabled_) {
        candidate.embedded_engine.enabled = chk_embedded_enabled_->GetValue();
    }

    if (txt_embedding_path_) {
        std::string emb_path = trim_ascii(std::string(txt_embedding_path_->GetValue().ToUTF8()));
        if (emb_path.empty()) {
            candidate.embedded_engine.embedding_model.reset();
        } else {
            gateway::ModelFileConfig mcfg = candidate.embedded_engine.embedding_model.value_or(gateway::ModelFileConfig{});
            mcfg.path = emb_path;
            candidate.embedded_engine.embedding_model = mcfg;
        }
    }

    if (txt_reranker_path_) {
        std::string rer_path = trim_ascii(std::string(txt_reranker_path_->GetValue().ToUTF8()));
        if (rer_path.empty()) {
            candidate.embedded_engine.reranker_model.reset();
        } else {
            gateway::ModelFileConfig mcfg = candidate.embedded_engine.reranker_model.value_or(gateway::ModelFileConfig{});
            mcfg.path = rer_path;
            candidate.embedded_engine.reranker_model = mcfg;
        }
    }

    if (!ConfigStore::validate_config(candidate, error_out, false)) {
        return false;
    }

    current_config_ = std::move(candidate);
    update_config_state_banner();
    return true;
}

void MainFrame::refresh_status() {
    if (is_busy_ || !ui_alive_->load(std::memory_order_acquire)) {
        return;
    }

    uint16_t check_port = effective_running_port();
    auto health = health_client_.check_health(check_port, 200);
    if (!health && check_port != current_config_.listen.port) {
        health = health_client_.check_health(current_config_.listen.port, 200);
        if (health) {
            check_port = current_config_.listen.port;
            process_controller_.set_active_port(check_port);
        }
    }

    if (health) {
        if (!applied_config_.has_value()) {
            applied_config_ = saved_config_;
        }
        if (health->embedded == "error") {
            status_indicator_->SetLabel("[ Degraded ]");
            status_indicator_->SetForegroundColour(wxColour(200, 140, 0)); // Amber
            status_text_->SetLabel("Status: Healthy (Embedded Error)");
        } else {
            status_indicator_->SetLabel("[ Running ]");
            status_indicator_->SetForegroundColour(wxColour(0, 180, 0)); // Green
            status_text_->SetLabel("Status: Healthy");
        }
        pid_text_->SetLabel("PID: " + std::to_string(health->pid));
        embedded_text_->SetLabel("Embedded: " + health->embedded);
        btn_start_->Enable(false);
        btn_stop_->Enable(true);
    } else {
        bool running = process_controller_.is_running();
        if (running) {
            status_indicator_->SetLabel("[ Initializing ]");
            status_indicator_->SetForegroundColour(wxColour(200, 160, 0)); // Yellow
            status_text_->SetLabel("Status: Initializing");
            pid_text_->SetLabel("PID: " + std::to_string(process_controller_.process_id()));
            btn_start_->Enable(false);
            btn_stop_->Enable(true);
        } else {
            applied_config_.reset();
            status_indicator_->SetLabel("[ Stopped ]");
            status_indicator_->SetForegroundColour(wxColour(200, 0, 0)); // Red
            status_text_->SetLabel("Status: Stopped");
            pid_text_->SetLabel("PID: -");
            embedded_text_->SetLabel("Embedded: -");
            btn_start_->Enable(true);
            btn_stop_->Enable(false);
        }
    }
    update_config_state_banner();
}

void MainFrame::refresh_providers_ui() {
    provider_list_->DeleteAllItems();
    for (size_t i = 0; i < current_config_.providers.size(); ++i) {
        const auto& p = current_config_.providers[i];
        long idx = provider_list_->InsertItem(static_cast<long>(i), wxString::FromUTF8(p.id.c_str()));
        provider_list_->SetItem(idx, 1, wxString::FromUTF8(p.base_url.value_or("-").c_str()));
        provider_list_->SetItem(idx, 2, (p.authentication == gateway::Authentication::Bearer) ? "bearer" : "none");
        std::string vs = p.key_vendor.empty() ? "-" : (p.key_vendor + "|" + p.key_slot);
        provider_list_->SetItem(idx, 3, wxString::FromUTF8(vs.c_str()));
        provider_list_->SetItem(idx, 4, p.model_discovery ? "true" : "false");

        std::string al_str = ConfigStore::join_csv_tokens(p.allowlist);
        provider_list_->SetItem(idx, 5, wxString::FromUTF8((al_str.empty() ? "(none)" : al_str).c_str()));
    }

    alias_list_->DeleteAllItems();
    for (size_t i = 0; i < current_config_.models.size(); ++i) {
        const auto& m = current_config_.models[i];
        long idx = alias_list_->InsertItem(static_cast<long>(i), wxString::FromUTF8(m.alias.c_str()));
        alias_list_->SetItem(idx, 1, wxString::FromUTF8(m.provider.c_str()));
        alias_list_->SetItem(idx, 2, wxString::FromUTF8(m.model.c_str()));
        alias_list_->SetItem(idx, 3, wxString::FromUTF8(m.input_prefix.value_or("-").c_str()));
    }
}

void MainFrame::refresh_credentials_ui() {
    cred_list_->DeleteAllItems();
    long row = 0;
    for (const auto& p : current_config_.providers) {
        if (p.authentication == gateway::Authentication::Bearer && !p.key_vendor.empty()) {
            auto st = cred_editor_.check_provider_key(p.key_vendor, p.key_slot);
            long idx = cred_list_->InsertItem(row++, wxString::FromUTF8(p.key_vendor.c_str()));
            cred_list_->SetItem(idx, 1, wxString::FromUTF8(p.key_slot.c_str()));
            cred_list_->SetItem(idx, 2, wxString::FromUTF8(cred_store_.format_target_name(p.key_vendor, p.key_slot).c_str()));
            cred_list_->SetItem(idx, 3, st.is_registered ? "Registered" : "Not Registered");
        }
    }
}

void MainFrame::append_log_lines(const std::vector<std::string>& lines) {
    constexpr int kMaxViewerLines = 5000;
    if (lines.empty()) return;
    wxString chunk;
    for (const auto& line : lines) {
        chunk += wxString::FromUTF8(line.c_str()) + "\n";
    }
    log_viewer_->AppendText(chunk);
    const int total = log_viewer_->GetNumberOfLines();
    if (total > kMaxViewerLines) {
        const long cut = log_viewer_->XYToPosition(0, total - kMaxViewerLines);
        if (cut > 0) log_viewer_->Remove(0, cut);
    }
}

void MainFrame::poll_logs() {
    append_log_lines(log_reader_.read_new_lines());
}

void MainFrame::set_controls_busy(bool busy) {
    is_busy_ = busy;
    if (btn_open_config_) btn_open_config_->Enable(!busy);
    btn_start_->Enable(!busy);
    btn_stop_->Enable(!busy);
    btn_save_->Enable(!busy);
    btn_restart_->Enable(!busy);
    if (chk_embedded_enabled_) chk_embedded_enabled_->Enable(!busy);
    if (chk_client_auth_) chk_client_auth_->Enable(!busy);
    if (chk_cors_enabled_) chk_cors_enabled_->Enable(!busy);
    if (btn_apply_settings_) btn_apply_settings_->Enable(!busy);
    if (btn_upsert_provider_) btn_upsert_provider_->Enable(!busy);
    if (btn_remove_provider_) btn_remove_provider_->Enable(!busy);
    if (btn_preset_openrouter_) btn_preset_openrouter_->Enable(!busy);
    if (btn_preset_lmstudio_) btn_preset_lmstudio_->Enable(!busy);
    if (btn_upsert_alias_) btn_upsert_alias_->Enable(!busy);
    if (btn_remove_alias_) btn_remove_alias_->Enable(!busy);
}

void MainFrame::on_open_config(wxCommandEvent&) {
    if (is_busy_) return;
    if (has_unsaved_changes()) {
        int res = wxMessageBox(
            "You have unsaved configuration changes. Discard changes and open another configuration file?",
            "Unsaved Changes",
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this);
        if (res != wxYES) return;
    }

    wxFileDialog dlg(
        this,
        "Open Gateway Configuration JSON",
        wxString(config_store_.current_path().parent_path().wstring()),
        wxString(config_store_.current_path().filename().wstring()),
        "JSON files (*.json)|*.json|All files (*.*)|*.*",
        wxFD_OPEN);
    if (dlg.ShowModal() != wxID_OK) return;

    std::filesystem::path chosen(dlg.GetPath().ToStdWstring());
    ConfigStore next_store(chosen);
    try {
        gateway::GatewayConfig loaded = next_store.load();
        config_store_.set_path(chosen);
        current_config_ = loaded;
        saved_config_ = loaded;
        config_exists_on_disk_ = config_store_.exists_on_disk();
        refresh_settings_ui();
        refresh_providers_ui();
        refresh_credentials_ui();
        refresh_status();
        update_config_state_banner();
    } catch (const std::exception& e) {
        wxMessageBox(wxString::FromUTF8(("Failed to load configuration: " + std::string(e.what())).c_str()),
                     "Load Error", wxOK | wxICON_ERROR, this);
    }
}

void MainFrame::on_apply_settings(wxCommandEvent&) {
    if (is_busy_) return;
    std::string err;
    if (!sync_ui_settings_to_config(&err)) {
        wxMessageBox(wxString::FromUTF8(("Invalid settings: " + err).c_str()),
                     "Validation Error", wxOK | wxICON_WARNING, this);
        return;
    }
    refresh_settings_ui();
    update_config_state_banner();
}

void MainFrame::on_browse_embedding(wxCommandEvent&) {
    if (is_busy_) return;
    wxFileDialog dlg(this, "Select Embedding GGUF Model", "", "",
                     "GGUF Model Files (*.gguf)|*.gguf|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        std::filesystem::path p(dlg.GetPath().ToStdWstring());
        txt_embedding_path_->SetValue(wxString::FromUTF8(gateway::path_to_utf8(p).c_str()));
        std::string err;
        (void)sync_ui_settings_to_config(&err);
    }
}

void MainFrame::on_browse_reranker(wxCommandEvent&) {
    if (is_busy_) return;
    wxFileDialog dlg(this, "Select Reranker GGUF Model", "", "",
                     "GGUF Model Files (*.gguf)|*.gguf|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        std::filesystem::path p(dlg.GetPath().ToStdWstring());
        txt_reranker_path_->SetValue(wxString::FromUTF8(gateway::path_to_utf8(p).c_str()));
        std::string err;
        (void)sync_ui_settings_to_config(&err);
    }
}

void MainFrame::on_provider_selected(wxListEvent& evt) {
    long idx = evt.GetIndex();
    if (idx < 0 || static_cast<size_t>(idx) >= current_config_.providers.size()) return;
    const auto& p = current_config_.providers[static_cast<size_t>(idx)];
    txt_prov_id_->SetValue(wxString::FromUTF8(p.id.c_str()));
    txt_prov_url_->SetValue(wxString::FromUTF8(p.base_url.value_or("").c_str()));
    choice_prov_auth_->SetSelection(p.authentication == gateway::Authentication::Bearer ? 0 : 1);
    txt_prov_vendor_->SetValue(wxString::FromUTF8(p.key_vendor.c_str()));
    txt_prov_slot_->SetValue(wxString::FromUTF8((p.key_slot.empty() ? "default" : p.key_slot).c_str()));
    chk_prov_discovery_->SetValue(p.model_discovery);
    txt_prov_allowlist_->SetValue(wxString::FromUTF8(ConfigStore::join_csv_tokens(p.allowlist).c_str()));
}

void MainFrame::on_upsert_provider(wxCommandEvent&) {
    if (is_busy_) return;
    gateway::ProviderConfig p;
    p.id = trim_ascii(std::string(txt_prov_id_->GetValue().ToUTF8()));
    std::string url = trim_ascii(std::string(txt_prov_url_->GetValue().ToUTF8()));
    if (!url.empty()) {
        p.base_url = url;
    }
    p.authentication = (choice_prov_auth_->GetSelection() == 0)
        ? gateway::Authentication::Bearer
        : gateway::Authentication::None;
    p.key_vendor = trim_ascii(std::string(txt_prov_vendor_->GetValue().ToUTF8()));
    p.key_slot = trim_ascii(std::string(txt_prov_slot_->GetValue().ToUTF8()));
    if (p.authentication == gateway::Authentication::Bearer && p.key_slot.empty()) {
        p.key_slot = "default";
    }
    p.model_discovery = chk_prov_discovery_->GetValue();
    p.allowlist = ConfigStore::split_csv_tokens(std::string(txt_prov_allowlist_->GetValue().ToUTF8()));

    std::string err;
    if (!ConfigStore::upsert_provider(current_config_, p, &err)) {
        wxMessageBox(wxString::FromUTF8(("Provider validation failed: " + err).c_str()),
                     "Provider Error", wxOK | wxICON_WARNING, this);
        return;
    }

    refresh_providers_ui();
    refresh_credentials_ui();
    update_config_state_banner();
}

void MainFrame::on_remove_provider(wxCommandEvent&) {
    if (is_busy_) return;
    std::string id = trim_ascii(std::string(txt_prov_id_->GetValue().ToUTF8()));
    if (id.empty()) {
        long sel = provider_list_->GetFirstSelected();
        if (sel >= 0 && static_cast<size_t>(sel) < current_config_.providers.size()) {
            id = current_config_.providers[static_cast<size_t>(sel)].id;
        }
    }
    if (id.empty()) {
        wxMessageBox("Please select a provider to remove.", "Selection Required", wxOK | wxICON_WARNING, this);
        return;
    }

    std::string err;
    if (!ConfigStore::remove_provider(current_config_, id, &err)) {
        wxMessageBox(wxString::FromUTF8(err.c_str()), "Remove Provider Error", wxOK | wxICON_WARNING, this);
        return;
    }

    refresh_providers_ui();
    refresh_credentials_ui();
    update_config_state_banner();
}

void MainFrame::on_preset_openrouter(wxCommandEvent&) {
    if (is_busy_) return;
    auto preset = ConfigStore::make_openrouter_preset();
    std::string err;
    if (!ConfigStore::upsert_provider(current_config_, preset, &err)) {
        wxMessageBox(wxString::FromUTF8(err.c_str()), "Preset Error", wxOK | wxICON_WARNING, this);
        return;
    }
    refresh_providers_ui();
    refresh_credentials_ui();
    update_config_state_banner();
}

void MainFrame::on_preset_lmstudio(wxCommandEvent&) {
    if (is_busy_) return;
    auto preset = ConfigStore::make_lm_studio_preset();
    std::string err;
    if (!ConfigStore::upsert_provider(current_config_, preset, &err)) {
        wxMessageBox(wxString::FromUTF8(err.c_str()), "Preset Error", wxOK | wxICON_WARNING, this);
        return;
    }
    refresh_providers_ui();
    refresh_credentials_ui();
    update_config_state_banner();
}

void MainFrame::on_alias_selected(wxListEvent& evt) {
    long idx = evt.GetIndex();
    if (idx < 0 || static_cast<size_t>(idx) >= current_config_.models.size()) return;
    const auto& m = current_config_.models[static_cast<size_t>(idx)];
    txt_alias_name_->SetValue(wxString::FromUTF8(m.alias.c_str()));
    txt_alias_provider_->SetValue(wxString::FromUTF8(m.provider.c_str()));
    txt_alias_model_->SetValue(wxString::FromUTF8(m.model.c_str()));
    txt_alias_prefix_->SetValue(wxString::FromUTF8(m.input_prefix.value_or("").c_str()));
}

void MainFrame::on_upsert_alias(wxCommandEvent&) {
    if (is_busy_) return;
    gateway::ModelAlias m;
    m.alias = trim_ascii(std::string(txt_alias_name_->GetValue().ToUTF8()));
    m.provider = trim_ascii(std::string(txt_alias_provider_->GetValue().ToUTF8()));
    m.model = trim_ascii(std::string(txt_alias_model_->GetValue().ToUTF8()));
    std::string prefix = std::string(txt_alias_prefix_->GetValue().ToUTF8());
    if (!prefix.empty()) {
        m.input_prefix = prefix;
    }

    std::string err;
    if (!ConfigStore::upsert_model_alias(current_config_, m, &err)) {
        wxMessageBox(wxString::FromUTF8(("Model alias validation failed: " + err).c_str()),
                     "Alias Error", wxOK | wxICON_WARNING, this);
        return;
    }

    refresh_providers_ui();
    update_config_state_banner();
}

void MainFrame::on_remove_alias(wxCommandEvent&) {
    if (is_busy_) return;
    std::string alias = trim_ascii(std::string(txt_alias_name_->GetValue().ToUTF8()));
    if (alias.empty()) {
        long sel = alias_list_->GetFirstSelected();
        if (sel >= 0 && static_cast<size_t>(sel) < current_config_.models.size()) {
            alias = current_config_.models[static_cast<size_t>(sel)].alias;
        }
    }
    if (alias.empty()) {
        wxMessageBox("Please select a model alias to remove.", "Selection Required", wxOK | wxICON_WARNING, this);
        return;
    }

    std::string err;
    if (!ConfigStore::remove_model_alias(current_config_, alias, &err)) {
        wxMessageBox(wxString::FromUTF8(err.c_str()), "Remove Alias Error", wxOK | wxICON_WARNING, this);
        return;
    }

    refresh_providers_ui();
    update_config_state_banner();
}

bool MainFrame::confirm_config_format_change() {
    if (!current_config_.loaded_from_legacy) {
        return true;
    }

    wxMessageDialog dlg(
        this,
        "Saving will convert this legacy configuration to version 2.0 format.\n"
        "Older Gateway versions may no longer read it.\n\n"
        "Choose 'Overwrite in v2.0' to replace the current file, or 'Save Copy As...' to preserve the legacy file and save v2.0 to a new path.",
        "Configuration Format Change",
        wxYES_NO | wxCANCEL | wxNO_DEFAULT | wxICON_WARNING);
    dlg.SetYesNoCancelLabels("Overwrite in v2.0", "Save Copy As...", "Cancel");

    int res = dlg.ShowModal();
    if (res == wxID_YES) {
        return true;
    }
    if (res == wxID_NO) {
        wxFileDialog save_dlg(
            this,
            "Save Version 2.0 Configuration Copy As",
            wxString(config_store_.current_path().parent_path().wstring()),
            "llm-gateway.v2.json",
            "JSON files (*.json)|*.json|All files (*.*)|*.*",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (save_dlg.ShowModal() != wxID_OK) {
            return false;
        }
        config_store_.set_path(std::filesystem::path(save_dlg.GetPath().ToStdWstring()));
        return true;
    }
    return false;
}

bool MainFrame::perform_save_config(bool update_ui_feedback) {
    std::string err;
    if (!sync_ui_settings_to_config(&err)) {
        if (update_ui_feedback) {
            wxMessageBox(wxString::FromUTF8(("Invalid settings: " + err).c_str()),
                         "Validation Error", wxOK | wxICON_ERROR, this);
        }
        return false;
    }
    if (!confirm_config_format_change()) {
        return false;
    }
    if (config_store_.save_atomic(current_config_, &err)) {
        current_config_.loaded_from_legacy = false;
        saved_config_ = current_config_;
        config_exists_on_disk_ = true;
        update_config_state_banner();
        if (update_ui_feedback) {
            wxMessageBox("Configuration saved successfully.", "Save Config", wxOK | wxICON_INFORMATION, this);
        }
        return true;
    } else {
        if (update_ui_feedback) {
            wxMessageBox(wxString::FromUTF8(("Failed to save configuration: " + err).c_str()),
                         "Save Error", wxOK | wxICON_ERROR, this);
        }
        return false;
    }
}

void MainFrame::on_start(wxCommandEvent&) {
    if (is_busy_) return;

    std::string sync_err;
    if (!sync_ui_settings_to_config(&sync_err)) {
        wxMessageBox(wxString::FromUTF8(("Cannot start with invalid settings: " + sync_err).c_str()),
                     "Validation Error", wxOK | wxICON_ERROR, this);
        return;
    }

    if (!config_exists_on_disk_ || has_unsaved_changes()) {
        wxString prompt_msg = !config_exists_on_disk_
            ? "The configuration file does not exist on disk yet.\nSave configuration now before starting Gateway?"
            : "You have unsaved configuration changes in the UI.\nSave configuration before starting Gateway so your edits take effect?";
        long style = !config_exists_on_disk_
            ? (wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION)
            : (wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_QUESTION);
        int ans = wxMessageBox(prompt_msg, "Unsaved Configuration", style, this);
        if (ans == wxCANCEL || (!config_exists_on_disk_ && ans != wxYES)) {
            return;
        }
        if (ans == wxYES) {
            if (!perform_save_config(false)) {
                return;
            }
        }
    }

    set_controls_busy(true);
    std::string err;
    process_controller_.set_active_admin_credential_name(saved_config_.security.admin_credential_name);
    if (!process_controller_.start_gateway(config_store_.current_path(), &err)) {
        wxMessageBox(wxString::FromUTF8(("Failed to start Gateway: " + err).c_str()), "Start Error", wxOK | wxICON_ERROR, this);
    } else {
        applied_config_ = saved_config_;
        process_controller_.set_active_port(saved_config_.listen.port);
    }
    set_controls_busy(false);
    refresh_status();
}

void MainFrame::on_stop(wxCommandEvent&) {
    if (is_busy_) return;
    set_controls_busy(true);
    process_controller_.reset_cancel();
    std::string err;
    uint16_t stop_port = effective_running_port();
    std::string stop_admin = applied_config_.has_value()
        ? applied_config_->security.admin_credential_name
        : current_config_.security.admin_credential_name;
    if (!process_controller_.stop_gateway(stop_port, &err, stop_admin)) {
        wxMessageBox(wxString::FromUTF8(("Failed to stop Gateway: " + err).c_str()), "Stop Error", wxOK | wxICON_ERROR, this);
    } else {
        applied_config_.reset();
    }
    set_controls_busy(false);
    refresh_status();
}

void MainFrame::on_save(wxCommandEvent&) {
    if (is_busy_) return;
    (void)perform_save_config(true);
}

void MainFrame::on_restart(wxCommandEvent&) {
    if (is_busy_) return;

    std::string sync_err;
    if (!sync_ui_settings_to_config(&sync_err)) {
        wxMessageBox(wxString::FromUTF8(("Invalid settings: " + sync_err).c_str()),
                     "Validation Error", wxOK | wxICON_ERROR, this);
        return;
    }
    if (!confirm_config_format_change()) return;

    join_restart_worker();
    process_controller_.reset_cancel();

    set_controls_busy(true);
    status_indicator_->SetLabel("[ Restarting ]");
    status_indicator_->SetForegroundColour(wxColour(200, 160, 0));

    const gateway::GatewayConfig config_snapshot = current_config_;
    const std::filesystem::path config_path_snapshot = config_store_.current_path();
    auto ui_alive = ui_alive_;

    restart_worker_ = std::thread([this, config_snapshot, config_path_snapshot, ui_alive]() {
        std::string err;
        bool saved = false;
        bool ok = process_controller_.restart_pipeline(
            config_snapshot,
            config_path_snapshot,
            [this, ui_alive, &saved](RestartStage stage, const std::string& msg) {
                if (stage == RestartStage::Stopping ||
                    stage == RestartStage::Starting ||
                    stage == RestartStage::WaitingHealth ||
                    stage == RestartStage::Completed) {
                    saved = true;
                }
                if (!ui_alive->load(std::memory_order_acquire) || wxTheApp == nullptr) {
                    return;
                }
                wxTheApp->CallAfter([this, ui_alive, msg]() {
                    if (!ui_alive->load(std::memory_order_acquire)) {
                        return;
                    }
                    status_text_->SetLabel(wxString::FromUTF8(("Status: " + msg).c_str()));
                });
            },
            &err
        );

        if (!ui_alive->load(std::memory_order_acquire) || wxTheApp == nullptr) {
            return;
        }
        wxTheApp->CallAfter([this, ui_alive, ok, saved, config_snapshot, err]() {
            if (!ui_alive->load(std::memory_order_acquire)) {
                return;
            }
            if (saved || ok) {
                current_config_.loaded_from_legacy = false;
                saved_config_ = config_snapshot;
                saved_config_.loaded_from_legacy = false;
                config_exists_on_disk_ = true;
            }
            if (ok) {
                applied_config_ = saved_config_;
                process_controller_.set_active_port(saved_config_.listen.port);
            }
            set_controls_busy(false);
            refresh_status();
            update_config_state_banner();
            if (!ok) {
                wxMessageBox(wxString::FromUTF8(("Restart pipeline failed: " + err).c_str()), "Restart Error", wxOK | wxICON_ERROR, this);
            }
        });
    });
}

void MainFrame::on_close(wxCloseEvent& evt) {
    if (evt.CanVeto() && !is_busy_) {
        std::string ignore_err;
        (void)sync_ui_settings_to_config(&ignore_err);
        if (has_unsaved_changes()) {
            int ans = wxMessageBox(
                "You have unsaved configuration changes.\nSave configuration before closing?",
                "Unsaved Changes",
                wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_WARNING,
                this);
            if (ans == wxCANCEL) {
                evt.Veto();
                return;
            }
            if (ans == wxYES) {
                if (!perform_save_config(false)) {
                    evt.Veto();
                    return;
                }
            }
        }
    }
    evt.Skip();
}

void MainFrame::on_health_timer(wxTimerEvent&) {
    refresh_status();
}

void MainFrame::on_log_timer(wxTimerEvent&) {
    poll_logs();
}

void MainFrame::on_gen_client_token(wxCommandEvent&) {
    std::string token = cred_editor_.generate_and_store_token(current_config_.security.credential_name);
    if (token.empty()) {
        wxMessageBox("Failed to generate or store Client token in Windows Credential Manager.",
                     "Token Error", wxOK | wxICON_ERROR);
        return;
    }
    bool copied = CredentialEditor::copy_to_clipboard_temporary(token, 30);
    SecureZeroMemory(token.data(), token.size());
    if (copied) {
        wxMessageBox("Client token generated, stored in Windows Credential Manager, and copied to clipboard.\n"
                     "(Clipboard will clear automatically in 30 seconds while MaintenanceTool remains open).",
                     "Token Generated", wxOK | wxICON_INFORMATION);
    } else {
        wxMessageBox("Client token was generated and stored in Windows Credential Manager, "
                     "but copying to the clipboard failed.",
                     "Clipboard Warning", wxOK | wxICON_WARNING);
    }
}

void MainFrame::on_gen_admin_token(wxCommandEvent&) {
    std::string token = cred_editor_.generate_and_store_token(current_config_.security.admin_credential_name);
    if (token.empty()) {
        wxMessageBox("Failed to generate or store Admin token in Windows Credential Manager.",
                     "Token Error", wxOK | wxICON_ERROR);
        return;
    }
    bool copied = CredentialEditor::copy_to_clipboard_temporary(token, 30);
    SecureZeroMemory(token.data(), token.size());
    if (copied) {
        wxMessageBox("Admin token generated, stored in Windows Credential Manager, and copied to clipboard.\n"
                     "(Clipboard will clear automatically in 30 seconds while MaintenanceTool remains open).",
                     "Token Generated", wxOK | wxICON_INFORMATION);
    } else {
        wxMessageBox("Admin token was generated and stored in Windows Credential Manager, "
                     "but copying to the clipboard failed.",
                     "Clipboard Warning", wxOK | wxICON_WARNING);
    }
}

void MainFrame::on_save_secret(wxCommandEvent&) {
    long sel = cred_list_->GetFirstSelected();
    if (sel == -1) {
        wxMessageBox("Please select a provider row in the table first.", "Selection Required", wxOK | wxICON_WARNING);
        return;
    }

    std::string vendor = cred_list_->GetItemText(sel, 0).ToStdString();
    std::string slot = cred_list_->GetItemText(sel, 1).ToStdString();
    std::string secret = txt_new_secret_->GetValue().ToStdString();

    if (secret.empty()) {
        wxMessageBox("Secret value cannot be empty.", "Validation Error", wxOK | wxICON_WARNING);
        return;
    }

    bool ok = cred_editor_.set_provider_key(vendor, slot, secret);
    txt_new_secret_->Clear();
    SecureZeroMemory(secret.data(), secret.size());

    if (ok) {
        wxMessageBox("Key registered in Windows Credential Manager successfully.", "Success", wxOK | wxICON_INFORMATION);
        refresh_credentials_ui();
    } else {
        wxMessageBox("Failed to save key in Windows Credential Manager.", "Error", wxOK | wxICON_ERROR);
    }
}

void MainFrame::on_delete_secret(wxCommandEvent&) {
    long sel = cred_list_->GetFirstSelected();
    if (sel == -1) {
        wxMessageBox("Please select a provider row in the table first.", "Selection Required", wxOK | wxICON_WARNING);
        return;
    }

    std::string vendor = cred_list_->GetItemText(sel, 0).ToStdString();
    std::string slot = cred_list_->GetItemText(sel, 1).ToStdString();

    int answer = wxMessageBox("Are you sure you want to delete credential for " + vendor + "|" + slot + "?",
                              "Confirm Delete", wxYES_NO | wxICON_QUESTION);
    if (answer == wxYES) {
        bool ok = cred_editor_.delete_provider_key(vendor, slot);
        refresh_credentials_ui();
        if (!ok) {
            wxMessageBox("Failed to delete key from Windows Credential Manager (it may not exist or access was denied).",
                         "Delete Error", wxOK | wxICON_ERROR);
        }
    }
}

} // namespace maintenance
