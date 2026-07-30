// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_dialog.h"
#include "download_manager.h"
#include "player_launcher.h"
#include "torrent_download_source.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

QSettings open_settings() {
	// Explicit scope rather than relying on application metadata, so where this
	// lands does not change if the app or organisation name is ever edited.
	return QSettings(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
}

}  // namespace

// --------------------------------------------------------------- the store --

namespace settings_store {

void load_into(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents) {
	QSettings s = open_settings();

	if (players) {
		players->set_custom_command(s.value("player/custom_command").toString());
		const QString id = s.value("player/selected").toString();
		if (!id.isEmpty())
			players->set_selected(id);
		// refresh() re-resolves the default if the saved player has since been
		// uninstalled, which is the case worth handling: a machine that lost
		// mpv should fall back rather than silently fail to launch anything.
		players->refresh();
	}

	if (downloads) {
		const QString dir = s.value("downloads/directory").toString();
		if (!dir.isEmpty())
			downloads->set_directory(dir);
	}

	if (torrents) {
		torrents->set_connection_limits(
			s.value("torrent/connections_global", 800).toInt(),
			s.value("torrent/connections_per_torrent", 200).toInt());
		torrents->set_seed_ratio(s.value("torrent/seed_ratio", 1.0).toDouble());
		torrents->set_listen_interfaces(
			s.value("torrent/listen_interfaces").toString());
		torrents->set_sequential(s.value("torrent/sequential", false).toBool());
	}
}

void save_from(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents) {
	QSettings s = open_settings();

	if (players) {
		s.setValue("player/selected", players->selected());
		s.setValue("player/custom_command", players->custom_command());
	}
	if (downloads)
		s.setValue("downloads/directory", downloads->directory());
	if (torrents) {
		s.setValue("torrent/connections_global",
		            torrents->connection_limit_global());
		s.setValue("torrent/connections_per_torrent",
		            torrents->connection_limit_per_torrent());
		s.setValue("torrent/seed_ratio", torrents->seed_ratio());
		s.setValue("torrent/listen_interfaces", torrents->listen_interfaces());
		s.setValue("torrent/sequential", torrents->sequential());
	}
	s.sync();
}

}  // namespace settings_store

// -------------------------------------------------------------- the dialog --

settings_dialog::settings_dialog(player_launcher *players,
                                  download_manager *downloads,
                                  torrent_download_source *torrents,
                                  QWidget *parent)
	: QDialog(parent), m_players(players), m_downloads(downloads),
	  m_torrents(torrents) {
	setWindowTitle("Settings");
	resize(640, 540);

	auto *outer = new QVBoxLayout(this);
	auto *tabs  = new QTabWidget(this);
	outer->addWidget(tabs, 1);

	auto *player_page = new QWidget(tabs);
	auto *dl_page     = new QWidget(tabs);
	build_player_page(player_page);
	build_download_page(dl_page);
	tabs->addTab(player_page, "&Player");
	tabs->addTab(dl_page, "&Downloads");

	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		apply();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);

	load();
}

void settings_dialog::build_player_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);

	auto *intro = new QLabel(
		"Streams can be handed to your own player instead of the built-in "
		"one. Players that are not installed are listed but disabled, so you "
		"can see what is supported and what to install.", page);
	intro->setWordWrap(true);
	v->addWidget(intro);

	auto *group = new QGroupBox("External player", page);
	auto *gv    = new QVBoxLayout(group);

	// §11.3's radio group: everything supported is shown, installed ones are
	// selectable, missing ones are greyed with the reason. The point is that a
	// machine with only mplayer still gets a working default and can see why
	// the others are unavailable.
	for (const player_entry &e : m_players->players()) {
		auto *b = new QRadioButton(group);
		b->setProperty("player_id", e.id);
		if (e.id == QLatin1String(player_launcher::custom_id())) {
			b->setText("Custom…");
			b->setToolTip("Run any command; see the box below");
		} else {
			b->setText(e.installed ? e.label
			                       : QString("%1 — not installed").arg(e.label));
			b->setEnabled(e.installed);
			if (!e.installed)
				b->setToolTip(QString("Install %1 to use it").arg(e.id));
		}
		connect(b, &QRadioButton::toggled, this,
		         &settings_dialog::update_custom_state);
		m_player_buttons << b;
		gv->addWidget(b);
	}
	v->addWidget(group);

	auto *custom_box = new QGroupBox("Custom command", page);
	auto *cf = new QFormLayout(custom_box);
	m_custom_cmd = new QLineEdit(custom_box);
	m_custom_cmd->setPlaceholderText("mpv --fullscreen %U");
	cf->addRow("Command:", m_custom_cmd);
	auto *hint = new QLabel(
		"<small><b>%U</b> is replaced by the stream URL; if you leave it out "
		"the URL is appended. Arguments are split on spaces — this is not a "
		"shell, so quoting and pipes will not work.</small>", custom_box);
	hint->setWordWrap(true);
	cf->addRow(hint);
	connect(m_custom_cmd, &QLineEdit::textChanged, this,
	         &settings_dialog::update_custom_state);
	v->addWidget(custom_box);

	m_player_note = new QLabel(page);
	m_player_note->setWordWrap(true);
	v->addWidget(m_player_note);
	v->addStretch(1);
}

void settings_dialog::build_download_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);

	auto *where = new QGroupBox("Location", page);
	auto *wh    = new QHBoxLayout(where);
	m_dir = new QLineEdit(where);
	auto *browse = new QPushButton("Browse…", where);
	wh->addWidget(m_dir, 1);
	wh->addWidget(browse);
	connect(browse, &QPushButton::clicked, this, [this] {
		const QString d = QFileDialog::getExistingDirectory(
			this, "Download folder", m_dir->text());
		if (!d.isEmpty())
			m_dir->setText(d);
	});
	v->addWidget(where);

	auto *bt = new QGroupBox("BitTorrent", page);
	auto *bf = new QFormLayout(bt);

	m_conn_global = new QSpinBox(bt);
	m_conn_global->setRange(10, 20000);
	m_conn_global->setSingleStep(50);
	m_conn_torrent = new QSpinBox(bt);
	m_conn_torrent->setRange(5, 5000);
	m_conn_torrent->setSingleStep(10);
	bf->addRow("Connections, all torrents:", m_conn_global);
	bf->addRow("Connections, per torrent:", m_conn_torrent);

	// The §11.4 argument in one sentence, where the person changing the number
	// can read it.
	auto *caps_note = new QLabel(
		"<small>Swarm speed comes from holding many mostly-slow peers rather "
		"than a few fast ones, so these are the numbers that matter. Defaults "
		"here are already well above a typical desktop client's; raise them "
		"further if your connection and file-descriptor limit allow.</small>",
		bt);
	caps_note->setWordWrap(true);
	bf->addRow(caps_note);

	m_seed_ratio = new QDoubleSpinBox(bt);
	m_seed_ratio->setRange(0.0, 100.0);
	m_seed_ratio->setSingleStep(0.1);
	m_seed_ratio->setDecimals(2);
	m_seed_ratio->setSpecialValueText("Do not seed");
	bf->addRow("Seed until ratio:", m_seed_ratio);

	m_sequential = new QCheckBox("Download in order by default", bt);
	m_sequential->setToolTip(
		"Slightly slower overall, but the file is playable from the start. "
		"Watch turns this on for one torrent regardless.");
	bf->addRow(QString(), m_sequential);

	m_interfaces = new QLineEdit(bt);
	m_interfaces->setPlaceholderText("0.0.0.0:6881  (blank = any interface)");
	bf->addRow("Listen on:", m_interfaces);

	// Not a VPN, and says so. §11.4 decided Hydra ships no tunnel; this is the
	// field that makes the user's own system-level choice reliable instead of
	// competing with it.
	auto *iface_note = new QLabel(
		"<small>Hydra does not tunnel torrent traffic. If you run a VPN at the "
		"system level, naming its interface here (for example "
		"<tt>tun0:6881</tt>) keeps torrent traffic on it, and announces stop if "
		"that interface goes away.</small>", bt);
	iface_note->setWordWrap(true);
	bf->addRow(iface_note);

	if (!torrent_download_source::available() || !m_torrents) {
		bt->setEnabled(false);
		bt->setTitle("BitTorrent — unavailable in this build");
		bt->setToolTip("Built without libtorrent-rasterbar");
	}
	v->addWidget(bt);
	v->addStretch(1);
}

void settings_dialog::load() {
	const QString sel = m_players ? m_players->selected() : QString();
	for (QRadioButton *b : m_player_buttons) {
		if (b->property("player_id").toString() == sel) {
			b->setChecked(true);
			break;
		}
	}
	if (m_players)
		m_custom_cmd->setText(m_players->custom_command());

	if (m_downloads)
		m_dir->setText(m_downloads->directory());

	if (m_torrents) {
		m_conn_global->setValue(m_torrents->connection_limit_global());
		m_conn_torrent->setValue(m_torrents->connection_limit_per_torrent());
		m_seed_ratio->setValue(m_torrents->seed_ratio());
		m_interfaces->setText(m_torrents->listen_interfaces());
		m_sequential->setChecked(m_torrents->sequential());
	}
	update_custom_state();
}

void settings_dialog::update_custom_state() {
	QString chosen;
	for (QRadioButton *b : m_player_buttons)
		if (b->isChecked())
			chosen = b->property("player_id").toString();

	const bool custom = chosen == QLatin1String(player_launcher::custom_id());
	if (m_custom_cmd)
		m_custom_cmd->setEnabled(custom);

	if (!m_player_note)
		return;
	if (chosen.isEmpty()) {
		m_player_note->setText(
			"<b>No player selected.</b> Watch will not be offered until one is "
			"installed or a custom command is set.");
	} else if (custom && m_custom_cmd->text().trimmed().isEmpty()) {
		m_player_note->setText("<b>Enter a command</b> for the custom player.");
	} else {
		m_player_note->clear();
	}
}

void settings_dialog::apply() {
	if (m_players) {
		// The command first: selecting Custom… is only meaningful once the
		// launcher knows what to run, and set_custom_command re-resolves
		// whether that entry counts as installed.
		m_players->set_custom_command(m_custom_cmd->text());
		for (QRadioButton *b : m_player_buttons) {
			if (b->isChecked()) {
				m_players->set_selected(b->property("player_id").toString());
				break;
			}
		}
	}

	if (m_downloads && !m_dir->text().trimmed().isEmpty()) {
		QDir().mkpath(m_dir->text());
		m_downloads->set_directory(m_dir->text());
	}

	if (m_torrents) {
		m_torrents->set_connection_limits(m_conn_global->value(),
		                                   m_conn_torrent->value());
		m_torrents->set_seed_ratio(m_seed_ratio->value());
		m_torrents->set_listen_interfaces(m_interfaces->text().trimmed());
		m_torrents->set_sequential(m_sequential->isChecked());
	}

	settings_store::save_from(m_players, m_downloads, m_torrents);
}
