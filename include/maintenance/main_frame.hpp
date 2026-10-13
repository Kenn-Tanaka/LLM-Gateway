#pragma once

#include "maintenance/config_store.hpp"
#include "maintenance/credential_editor.hpp"
#include "maintenance/process_controller.hpp"
#include "maintenance/health_client.hpp"
#include "maintenance/log_tail_reader.hpp"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/choice.h>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

namespace maintenance {

class MainFrame : public wxFrame {
public:
    explicit MainFrame(std::optional<std::filesystem::path> config_path = std::nullopt);
    ~MainFrame() override;

private:
    ConfigStore config_store_;
    gateway::GatewayConfig current_config_;
    gateway::GatewayConfig saved_config_;
    std::optional<gateway::GatewayConfig> applied_config_;
    bool config_exists_on_disk_ = false;
    bool suppress_field_events_ = false;

    gateway::CredentialStore cred_store_;
    CredentialEditor cred_editor_;
    ProcessController process_controller_;
    HealthClient health_client_;
    LogTailReader log_reader_;

    wxTimer health_timer_;
    wxTimer log_timer_;
    bool is_busy_ = false;
    std::shared_ptr<std::atomic<bool>> ui_alive_ = std::make_shared<std::atomic<bool>>(true);
    std::thread restart_worker_;

    // UI Elements - Header
    wxStaticText* status_indicator_ = nullptr;
    wxStaticText* status_text_ = nullptr;
    wxStaticText* pid_text_ = nullptr;
    wxStaticText* port_text_ = nullptr;
    wxStaticText* embedded_text_ = nullptr;
    wxStaticText* config_path_text_ = nullptr;

    wxButton* btn_open_config_ = nullptr;
    wxButton* btn_start_ = nullptr;
    wxButton* btn_stop_ = nullptr;
    wxButton* btn_save_ = nullptr;
    wxButton* btn_restart_ = nullptr;

    // Tabs
    wxNotebook* notebook_ = nullptr;

    // Tab 1: Providers, Routing & Gateway Settings
    wxTextCtrl* txt_listen_port_ = nullptr;
    wxCheckBox* chk_client_auth_ = nullptr;
    wxCheckBox* chk_cors_enabled_ = nullptr;
    wxTextCtrl* txt_cors_origins_ = nullptr;
    wxCheckBox* chk_embedded_enabled_ = nullptr;
    wxTextCtrl* txt_embedding_path_ = nullptr;
    wxButton* btn_browse_embedding_ = nullptr;
    wxTextCtrl* txt_reranker_path_ = nullptr;
    wxButton* btn_browse_reranker_ = nullptr;
    wxButton* btn_apply_settings_ = nullptr;

    wxListView* provider_list_ = nullptr;
    wxTextCtrl* txt_prov_id_ = nullptr;
    wxTextCtrl* txt_prov_url_ = nullptr;
    wxChoice* choice_prov_auth_ = nullptr;
    wxTextCtrl* txt_prov_vendor_ = nullptr;
    wxTextCtrl* txt_prov_slot_ = nullptr;
    wxCheckBox* chk_prov_discovery_ = nullptr;
    wxTextCtrl* txt_prov_allowlist_ = nullptr;
    wxButton* btn_upsert_provider_ = nullptr;
    wxButton* btn_remove_provider_ = nullptr;
    wxButton* btn_preset_openrouter_ = nullptr;
    wxButton* btn_preset_lmstudio_ = nullptr;

    wxListView* alias_list_ = nullptr;
    wxTextCtrl* txt_alias_name_ = nullptr;
    wxTextCtrl* txt_alias_provider_ = nullptr;
    wxTextCtrl* txt_alias_model_ = nullptr;
    wxTextCtrl* txt_alias_prefix_ = nullptr;
    wxButton* btn_upsert_alias_ = nullptr;
    wxButton* btn_remove_alias_ = nullptr;

    // Tab 2: Credentials
    wxListView* cred_list_ = nullptr;
    wxTextCtrl* txt_new_secret_ = nullptr;
    wxButton* btn_save_secret_ = nullptr;
    wxButton* btn_delete_secret_ = nullptr;
    wxButton* btn_gen_client_token_ = nullptr;
    wxButton* btn_gen_admin_token_ = nullptr;

    // Tab 3: Logs
    wxTextCtrl* log_viewer_ = nullptr;

    void init_ui();
    wxPanel* create_header_panel(wxWindow* parent);
    wxPanel* create_providers_tab(wxWindow* parent);
    wxPanel* create_credentials_tab(wxWindow* parent);
    wxPanel* create_logs_tab(wxWindow* parent);

    void refresh_status();
    void refresh_settings_ui();
    void refresh_providers_ui();
    void refresh_credentials_ui();
    void update_config_state_banner();
    void poll_logs();
    void append_log_lines(const std::vector<std::string>& lines);

    [[nodiscard]] bool has_unsaved_changes() const;
    [[nodiscard]] bool has_unapplied_changes() const;
    [[nodiscard]] uint16_t effective_running_port() const;
    bool sync_ui_settings_to_config(std::string* error_out = nullptr);
    bool perform_save_config(bool update_ui_feedback = true);

    // Event Handlers
    void on_open_config(wxCommandEvent& evt);
    void on_start(wxCommandEvent& evt);
    void on_stop(wxCommandEvent& evt);
    void on_save(wxCommandEvent& evt);
    void on_restart(wxCommandEvent& evt);
    void on_close(wxCloseEvent& evt);

    void on_apply_settings(wxCommandEvent& evt);
    void on_browse_embedding(wxCommandEvent& evt);
    void on_browse_reranker(wxCommandEvent& evt);

    void on_provider_selected(wxListEvent& evt);
    void on_upsert_provider(wxCommandEvent& evt);
    void on_remove_provider(wxCommandEvent& evt);
    void on_preset_openrouter(wxCommandEvent& evt);
    void on_preset_lmstudio(wxCommandEvent& evt);

    void on_alias_selected(wxListEvent& evt);
    void on_upsert_alias(wxCommandEvent& evt);
    void on_remove_alias(wxCommandEvent& evt);

    void on_health_timer(wxTimerEvent& evt);
    void on_log_timer(wxTimerEvent& evt);

    void on_gen_client_token(wxCommandEvent& evt);
    void on_gen_admin_token(wxCommandEvent& evt);
    void on_save_secret(wxCommandEvent& evt);
    void on_delete_secret(wxCommandEvent& evt);

    void set_controls_busy(bool busy);
    void join_restart_worker();
    bool confirm_config_format_change();
};

} // namespace maintenance
