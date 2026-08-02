// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai_provider.h"

#include "kiosk_controller.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QListWidget;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QVBoxLayout;
class claude_provider;
class download_manager;
class ollama_provider;
class player_launcher;
class torrent_download_source;

// Settings (architecture doc §11.3, §11.4).
//
// Everything here already existed as API and could only be reached by editing
// code — the external-player choice §11.3 specifies in detail, and the
// BitTorrent knobs §11.4's whole connection-scaling argument rests on. A
// setting nobody can change is a setting that does not exist, which is why the
// caps in particular belong here: the argument for rasterbar was that the
// ceiling can be raised, and that claim is only true if someone can raise it.
//
// Values are read and written through `settings_store` below, so the dialog
// owns presentation and the store owns persistence and application.
class policy_engine;
class filter_list;
class QTreeWidget;
class QPushButton;

class settings_dialog : public QDialog {
	Q_OBJECT
public:
	settings_dialog(player_launcher *players, download_manager *downloads,
	                 torrent_download_source *torrents, ollama_provider *local_ai,
	                 claude_provider *external_ai, policy_engine *policy = nullptr,
	                 filter_list *filters = nullptr,
	                 const QString &filters_path = QString(),
	                 QWidget *parent = nullptr);

	// Saving belongs to *accepting*, not to a particular button. It hung off the
	// OK button's signal, so any other route to acceptance -- code calling
	// accept(), a future default-button change, a shortcut -- would have closed
	// the window and silently discarded everything typed into it. Found by a
	// test that accepted the dialog the obvious programmatic way.
	void accept() override;

private:
	void build_privacy_page(QWidget *page);
	void build_kiosk_page(QWidget *page);
	void build_filter_page(QWidget *page);
	void rebuild_filter_list();
	void load_kiosk();
	void apply_kiosk();
	void build_player_page(QWidget *page);
	void build_download_page(QWidget *page);
	void build_ai_page(QWidget *page);
	void update_ai_state();
	// Probing in a settings window is an explicit act, never something that
	// happens because the window opened. Both of these are wired to a button.
	void check_local_model();
	void rescan_players();
	void populate_players();
	void load();
	void apply();
	void update_custom_state();

	policy_engine           *m_policy   = nullptr;
	filter_list             *m_filters  = nullptr;
	QString                  m_filters_path;
	QTreeWidget             *m_filter_view   = nullptr;
	QPushButton             *m_filter_remove = nullptr;
	QLabel                  *m_filter_note   = nullptr;
	player_launcher         *m_players  = nullptr;
	download_manager        *m_downloads = nullptr;
	torrent_download_source *m_torrents = nullptr;
	ollama_provider         *m_local_ai = nullptr;
	claude_provider         *m_external_ai = nullptr;

	// One combo per policy feature, indexed by the enum, so a feature added to
	// the model turns up here without anyone remembering to add a row.
	QListWidget          *m_categories = nullptr;
	QLineEdit            *m_kiosk_home   = nullptr;
	QSpinBox             *m_kiosk_w      = nullptr;
	QSpinBox             *m_kiosk_h      = nullptr;
	QComboBox            *m_kiosk_scale  = nullptr;
	QComboBox            *m_kiosk_fit    = nullptr;
	QCheckBox            *m_kiosk_cursor = nullptr;
	QSpinBox             *m_kiosk_idle   = nullptr;
	QCheckBox            *m_kiosk_dog    = nullptr;
	QCheckBox            *m_kiosk_escape = nullptr;
	QList<QComboBox *>    m_feature_combos;
	QList<QRadioButton *> m_player_buttons;
	QVBoxLayout    *m_player_group_layout = nullptr;
	QLineEdit      *m_custom_cmd   = nullptr;
	QLabel         *m_player_note  = nullptr;

	QLineEdit      *m_dir          = nullptr;
	QSpinBox       *m_conn_global  = nullptr;
	QSpinBox       *m_conn_torrent = nullptr;
	QDoubleSpinBox *m_seed_ratio   = nullptr;
	QLineEdit      *m_interfaces   = nullptr;
	QCheckBox      *m_sequential   = nullptr;

	QRadioButton *m_ai_auto     = nullptr;
	QRadioButton *m_ai_local    = nullptr;
	QRadioButton *m_ai_external = nullptr;
	QLineEdit    *m_ollama_url   = nullptr;
	QLineEdit    *m_ollama_model = nullptr;
	QSpinBox     *m_probe_timeout = nullptr;
	QPushButton  *m_check_local   = nullptr;
	// Nothing is claimed about the local model until it has been checked.
	enum class probe_state { unknown, checking, reachable, unreachable };
	probe_state   m_probe_state = probe_state::unknown;
	QLineEdit    *m_claude_model = nullptr;
	QLineEdit    *m_claude_key   = nullptr;
	QLabel       *m_ai_status    = nullptr;
};

// Where settings live between runs, and the one place that pushes them into the
// objects that use them.
//
// Split out from the dialog so startup can apply saved values without building
// any UI — otherwise "settings persist" would quietly mean "settings persist if
// you open the dialog", which is the usual way this goes wrong.
namespace settings_store {

void load_into(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents, ollama_provider *local_ai,
                claude_provider *external_ai);

void save_from(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents, ollama_provider *local_ai,
                claude_provider *external_ai);

// Which backend the user picked. Read straight from storage rather than held
// on any one object, because it is a preference *about* providers rather than
// a property of either of them.
// Kiosk (§8). Kept here rather than on the controller for the same reason the
// AI mode is: it is a preference *about* a session that may not exist yet, and
// until now it was code-level defaults with no way to reach it from the app at
// all — a presentation mode nobody could configure.
kiosk_config kiosk();
void set_kiosk(const kiosk_config &c);

ai_choice ai_mode();
void set_ai_mode(ai_choice mode);

}  // namespace settings_store
