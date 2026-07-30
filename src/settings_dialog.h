// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QList>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class download_manager;
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
class settings_dialog : public QDialog {
	Q_OBJECT
public:
	settings_dialog(player_launcher *players, download_manager *downloads,
	                 torrent_download_source *torrents, QWidget *parent = nullptr);

private:
	void build_player_page(QWidget *page);
	void build_download_page(QWidget *page);
	void load();
	void apply();
	void update_custom_state();

	player_launcher         *m_players  = nullptr;
	download_manager        *m_downloads = nullptr;
	torrent_download_source *m_torrents = nullptr;

	QList<QRadioButton *> m_player_buttons;
	QLineEdit      *m_custom_cmd   = nullptr;
	QLabel         *m_player_note  = nullptr;

	QLineEdit      *m_dir          = nullptr;
	QSpinBox       *m_conn_global  = nullptr;
	QSpinBox       *m_conn_torrent = nullptr;
	QDoubleSpinBox *m_seed_ratio   = nullptr;
	QLineEdit      *m_interfaces   = nullptr;
	QCheckBox      *m_sequential   = nullptr;
};

// Where settings live between runs, and the one place that pushes them into the
// objects that use them.
//
// Split out from the dialog so startup can apply saved values without building
// any UI — otherwise "settings persist" would quietly mean "settings persist if
// you open the dialog", which is the usual way this goes wrong.
namespace settings_store {

void load_into(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents);

void save_from(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents);

}  // namespace settings_store
