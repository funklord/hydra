// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_dialog.h"
#include "claude_provider.h"
#include "download_manager.h"
#include "ollama_provider.h"
#include "player_launcher.h"
#include "torrent_download_source.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QListWidget>
#include <QStackedWidget>
#include "policy_engine.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
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

ai_choice ai_mode() {
	const QString v = open_settings().value("ai/mode", "automatic").toString();
	if (v == "local_only")
		return ai_choice::local_only;
	if (v == "external")
		return ai_choice::external;
	return ai_choice::automatic;
}

void set_ai_mode(ai_choice mode) {
	QSettings s = open_settings();
	s.setValue("ai/mode", mode == ai_choice::local_only ? "local_only"
	                     : mode == ai_choice::external  ? "external"
	                                                    : "automatic");
	s.sync();
}

void load_into(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents, ollama_provider *local_ai,
                claude_provider *external_ai) {
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

	if (local_ai) {
		local_ai->set_endpoint(QUrl(s.value("ai/ollama_endpoint",
		                                     "http://localhost:11434").toString()));
		local_ai->set_model(s.value("ai/ollama_model", "llama3").toString());
		local_ai->set_probe_timeout(
			s.value("ai/probe_timeout_ms", 2500).toInt());
	}
	if (external_ai) {
		const QString model = s.value("ai/claude_model").toString();
		if (!model.isEmpty())
			external_ai->set_model(model);
		// The API key is deliberately absent: this file is plain INI, and
		// claude_provider states the key is held in memory only. Writing it
		// here would quietly break that, so the key comes from the environment
		// or is typed for one session.
	}
}

void save_from(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents, ollama_provider *local_ai,
                claude_provider *external_ai) {
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
	if (local_ai) {
		s.setValue("ai/ollama_endpoint", local_ai->endpoint().toString());
		s.setValue("ai/ollama_model", local_ai->model());
		s.setValue("ai/probe_timeout_ms", local_ai->probe_timeout());
	}
	if (external_ai)
		s.setValue("ai/claude_model", external_ai->model());
	s.sync();
}

}  // namespace settings_store

// -------------------------------------------------------------- the dialog --

namespace {

// Which page a per-site feature belongs on, and in which group.
//
// Grouped by hand because the order is an editorial judgement -- Firefox puts
// cookie-banner handling directly after site data and before permissions, and
// that is the right adjacency: the banner is about cookies, and permissions are
// a different question. But the *fallback* matters more than the table: a
// feature not listed here still appears, under "Other". A settings screen that
// silently omits a switch because nobody updated a list is worse than one with
// an untidy last section, and this project has added four features to that enum
// in a week.
struct feature_group { policy::feature f; const char *group; };

const feature_group k_privacy_layout[] = {
	{ policy::feature::javascript,          "Content" },
	{ policy::feature::images,              "Content" },
	{ policy::feature::autoplay,            "Content" },
	{ policy::feature::popups,              "Content" },
	{ policy::feature::ads,                 "Content" },
	{ policy::feature::cookies,             "Cookies and site data" },
	{ policy::feature::third_party_cookies, "Cookies and site data" },
	{ policy::feature::cookie_notices,      "Cookies and site data" },
	{ policy::feature::geolocation,         "Permissions" },
	{ policy::feature::camera,              "Permissions" },
	{ policy::feature::microphone,          "Permissions" },
	{ policy::feature::notifications,       "Permissions" },
	{ policy::feature::referer,             "Sent with requests" },
	{ policy::feature::autofill,            "Passwords" },
};

}  // namespace

settings_dialog::settings_dialog(player_launcher *players,
                                  download_manager *downloads,
                                  torrent_download_source *torrents,
                                  ollama_provider *local_ai,
                                  claude_provider *external_ai,
                                  policy_engine *policy, QWidget *parent)
	: QDialog(parent), m_policy(policy), m_players(players),
	  m_downloads(downloads), m_torrents(torrents), m_local_ai(local_ai),
	  m_external_ai(external_ai) {
	setWindowTitle("Settings");
	resize(660, 620);

	// A category list beside a stack, not tabs. Every browser that outgrew four
	// preference tabs made this move -- Firefox, Chrome and Vivaldi all present a
	// list -- and for a plain reason: tab strips run out of width and start
	// eliding or wrapping, while a vertical list has room for a name that says
	// what is inside. Hydra passed four the moment site defaults arrived.
	auto *outer = new QVBoxLayout(this);
	auto *split = new QHBoxLayout;
	outer->addLayout(split, 1);

	m_categories = new QListWidget(this);
	m_categories->setObjectName("categories");
	m_categories->setMaximumWidth(190);
	split->addWidget(m_categories);

	auto *stack = new QStackedWidget(this);
	stack->setObjectName("pages");
	split->addWidget(stack, 1);
	connect(m_categories, &QListWidget::currentRowChanged,
	         stack, &QStackedWidget::setCurrentIndex);

	auto *privacy_page = new QWidget;
	auto *player_page = new QWidget;
	auto *dl_page     = new QWidget;
	auto *ai_page     = new QWidget;
	build_privacy_page(privacy_page);
	build_player_page(player_page);
	build_download_page(dl_page);
	build_ai_page(ai_page);

	// Each page scrolls. These pages carry explanatory text, and text height
	// depends on the system font and the user's scaling — any fixed dialog
	// height is a guess that silently clips the explanation on somebody's
	// machine, which is worse than a scrollbar.
	auto wrap = [stack](QWidget *page) {
		auto *area = new QScrollArea(stack);
		area->setWidget(page);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		return area;
	};
	// Privacy first, because it is the one page that is about what the browser
	// refuses to do on your behalf and the others are about conveniences.
	auto add_page = [&](QWidget *w, const QString &name) {
		stack->addWidget(w);
		m_categories->addItem(name);
	};
	add_page(wrap(privacy_page), "Privacy && security");
	add_page(wrap(player_page), "Media && players");
	add_page(wrap(dl_page), "Downloads");

	// The AI page scrolls, but its status line does not. It is the answer to
	// the button directly above it, and a result that has scrolled out of
	// sight is the same as no result — pressing Check now would appear to do
	// nothing at all.
	auto *ai_tab = new QWidget(stack);
	auto *ai_col = new QVBoxLayout(ai_tab);
	ai_col->setContentsMargins(0, 0, 0, 0);
	ai_col->addWidget(wrap(ai_page), 1);
	ai_col->addWidget(m_ai_status);
	add_page(ai_tab, "AI");
	m_categories->setCurrentRow(0);

	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);

	// Opening a settings window must not reach out to the network by itself:
	// the endpoint may be remote, may be wrong, and may be mid-edit. The probe
	// is a button, and until it is pressed nothing is claimed either way.
	if (m_local_ai) {
		connect(m_local_ai, &ollama_provider::probe_finished, this,
		         [this](bool reachable) {
			m_probe_state = reachable ? probe_state::reachable
			                          : probe_state::unreachable;
			m_check_local->setEnabled(true);
			update_ai_state();
		});
	}

	load();
}

void settings_dialog::build_privacy_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);

	auto *intro = new QLabel(
		"What every site is allowed to do unless you have said otherwise for "
		"that site. The shield in the toolbar sets exceptions per site, and an "
		"exception always wins over what is chosen here.", page);
	intro->setWordWrap(true);
	v->addWidget(intro);

	if (!m_policy) {
		v->addWidget(new QLabel("No policy engine was supplied.", page));
		v->addStretch(1);
		return;
	}

	m_feature_combos.clear();
	for (int i = 0; i < policy::feature_count(); ++i)
		m_feature_combos.append(nullptr);

	// Build the groups in the order the table declares them, then sweep up
	// anything the table does not mention. The sweep is the part that matters:
	// it is what stops a newly added feature from being invisible here.
	QStringList order;
	for (const feature_group &fg : k_privacy_layout)
		if (!order.contains(QString::fromUtf8(fg.group)))
			order << QString::fromUtf8(fg.group);
	order << "Other";

	auto row_for = [&](QFormLayout *form, policy::feature f) {
		auto *combo = new QComboBox(page);
		// Global defaults are always allow or block. "Default" is what a *site*
		// says when it has no opinion and falls through to here, so offering it
		// at this level would be a setting that points at itself.
		combo->addItem("Allow", int(policy::setting::allow));
		combo->addItem("Block", int(policy::setting::block));
		const policy::setting cur = m_policy->global_default(f);
		combo->setCurrentIndex(cur == policy::setting::block ? 1 : 0);
		combo->setObjectName(QString("feature_%1").arg(policy::feature_name(f)));
		m_feature_combos[int(f)] = combo;
		form->addRow(QString(policy::feature_label(f)) + ":", combo);
	};

	for (const QString &group_name : order) {
		auto *box = new QGroupBox(group_name, page);
		auto *form = new QFormLayout(box);
		int rows = 0;
		for (int i = 0; i < policy::feature_count(); ++i) {
			const auto f = static_cast<policy::feature>(i);
			QString mine = "Other";
			for (const feature_group &fg : k_privacy_layout)
				if (fg.f == f)
					mine = QString::fromUtf8(fg.group);
			if (mine != group_name)
				continue;
			row_for(form, f);
			++rows;
		}
		if (rows == 0) {
			delete box;
			continue;
		}
		v->addWidget(box);
	}

	auto *note = new QLabel(
		"<i>Cookie consent banners</i>: blocking them means Hydra answers the "
		"\"do you want to accept cookies?\" dialog for you, taking the least "
		"permissive option a site actually offers. Answering one allows that "
		"site's own cookies, because the choice is itself stored in a cookie "
		"and would otherwise be forgotten on every visit.", page);
	note->setWordWrap(true);
	v->addWidget(note);

	v->addStretch(1);
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
	m_player_group_layout = new QVBoxLayout(group);
	populate_players();

	auto *rescan = new QPushButton("&Rescan for players", group);
	rescan->setObjectName("rescan_players");
	rescan->setToolTip("Look again at PATH — use this after installing one");
	connect(rescan, &QPushButton::clicked, this, &settings_dialog::rescan_players);
	m_player_group_layout->addWidget(rescan);
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

// §11.3's radio group: everything supported is shown, installed ones are
// selectable, missing ones are greyed with the reason. The point is that a
// machine with only mplayer still gets a working default and can see why the
// others are unavailable.
void settings_dialog::populate_players() {
	for (QRadioButton *b : m_player_buttons)
		delete b;
	m_player_buttons.clear();

	int at = 0;
	for (const player_entry &e : m_players->players()) {
		auto *b = new QRadioButton;
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
		m_player_group_layout->insertWidget(at++, b);
	}
}

void settings_dialog::rescan_players() {
	// Scanning PATH is cheap, but it is still a probe, and it happens because
	// the user asked — not because a window opened.
	QString was;
	for (QRadioButton *b : m_player_buttons)
		if (b->isChecked())
			was = b->property("player_id").toString();

	m_players->refresh();
	populate_players();

	for (QRadioButton *b : m_player_buttons) {
		if (b->property("player_id").toString() == was && b->isEnabled()) {
			b->setChecked(true);
			break;
		}
	}
	update_custom_state();
}

void settings_dialog::check_local_model() {
	if (!m_local_ai || m_probe_state == probe_state::checking)
		return;
	m_probe_state = probe_state::checking;
	m_check_local->setEnabled(false);
	// Apply what is currently in the fields first, or the button would test
	// the old endpoint while showing the new one.
	const QString url = m_ollama_url->text().trimmed();
	m_local_ai->set_endpoint(QUrl(url.isEmpty() ? "http://localhost:11434" : url));
	m_local_ai->set_probe_timeout(m_probe_timeout->value());
	update_ai_state();
	m_local_ai->probe();          // asynchronous: the window stays usable
}

void settings_dialog::build_ai_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);

	auto *intro = new QLabel(
		"The tree reorganizer and the ad-filter proposals ask a model. Only "
		"metadata is ever sent — titles, URLs and structure — and nothing "
		"leaves until you review it and press Send.", page);
	intro->setWordWrap(true);
	v->addWidget(intro);

	auto *group = new QGroupBox("Which backend", page);
	auto *gv    = new QVBoxLayout(group);

	m_ai_auto = new QRadioButton(
		"Automatic — use the local model when it is running", group);
	m_ai_local = new QRadioButton(
		"Local only — never use an external service", group);
	m_ai_external = new QRadioButton("Claude (external service)", group);

	m_ai_auto->setToolTip("Falls back to Claude only when no local model answers");
	// The point of this option: §1's "data stays on the machine" should be
	// something you can hold the app to, not a default that silently lapses
	// the first time Ollama is not running.
	m_ai_local->setToolTip(
		"If the local model is not running, the AI features are simply "
		"unavailable rather than quietly switching to a service");
	for (QRadioButton *b : { m_ai_auto, m_ai_local, m_ai_external }) {
		gv->addWidget(b);
		connect(b, &QRadioButton::toggled, this, &settings_dialog::update_ai_state);
	}
	v->addWidget(group);

	auto *local_box = new QGroupBox("Local model (Ollama)", page);
	auto *lf = new QFormLayout(local_box);
	m_ollama_url = new QLineEdit(local_box);
	m_ollama_url->setPlaceholderText("http://localhost:11434");
	m_ollama_model = new QLineEdit(local_box);
	m_ollama_model->setPlaceholderText("llama3");
	m_probe_timeout = new QSpinBox(local_box);
	m_probe_timeout->setRange(100, 15000);
	m_probe_timeout->setSingleStep(250);
	m_probe_timeout->setSuffix(" ms");
	lf->addRow("Endpoint:", m_ollama_url);
	lf->addRow("Model:", m_ollama_model);
	lf->addRow("Reachability timeout:", m_probe_timeout);

	m_check_local = new QPushButton("&Check now", local_box);
	m_check_local->setObjectName("check_local");
	m_check_local->setToolTip("Ask the endpoint above whether it is there");
	connect(m_check_local, &QPushButton::clicked, this,
	         &settings_dialog::check_local_model);
	lf->addRow(QString(), m_check_local);

	// Why anyone would touch this: the cost is only paid when the endpoint
	// stops being local, and then it is paid every time.
	auto *probe_note = new QLabel(
		"<small>Rechecked whenever a backend is needed, since Ollama is started "
		"and stopped like any service. Loopback answers in about a millisecond; "
		"a remote host that <i>drops</i> packets never answers at all and costs "
		"this whole timeout each time — so lower it if the endpoint is not on "
		"this machine.</small>", local_box);
	probe_note->setWordWrap(true);
	lf->addRow(probe_note);
	v->addWidget(local_box);

	auto *ext_box = new QGroupBox("Claude", page);
	auto *ef = new QFormLayout(ext_box);
	m_claude_model = new QLineEdit(ext_box);
	m_claude_model->setPlaceholderText("claude-opus-5");
	ef->addRow("Model:", m_claude_model);

	m_claude_key = new QLineEdit(ext_box);
	m_claude_key->setEchoMode(QLineEdit::Password);
	m_claude_key->setPlaceholderText("from ANTHROPIC_API_KEY");
	ef->addRow("API key:", m_claude_key);

	// Not persisted, and it says so rather than letting someone find out. The
	// settings file is plain INI and an API key does not belong in one; the
	// same reasoning that keeps it out of the tree and policy files applies
	// here, and writing it anywhere readable would be security theatre.
	auto *key_note = new QLabel(
		"<small>The key is <b>not saved</b> — it is kept in memory for this "
		"session only. Set <tt>ANTHROPIC_API_KEY</tt> in your environment for "
		"it to persist; this settings file is plain text and a credential does "
		"not belong in one.</small>", ext_box);
	key_note->setWordWrap(true);
	ef->addRow(key_note);
	v->addWidget(ext_box);

	// Created here but *not* added to this layout: the constructor pins it
	// below the scroll area so it is always visible.
	m_ai_status = new QLabel;
	m_ai_status->setObjectName("ai_status");
	m_ai_status->setWordWrap(true);
	m_ai_status->setContentsMargins(6, 4, 6, 4);
	v->addStretch(1);
}

void settings_dialog::update_ai_state() {
	if (!m_ai_status)
		return;

	const bool ext_ok = m_external_ai &&
	                     (m_external_ai->has_api_key() ||
	                      !m_claude_key->text().trimmed().isEmpty());

	// Report only what has been established. Before the button is pressed the
	// honest answer is "not checked", not "unavailable" — the latter would be
	// a claim about the user's machine that nothing here has verified.
	if (m_probe_state == probe_state::checking) {
		m_ai_status->setText("Checking the local model…");
		return;
	}

	QString msg;
	if (m_ai_external->isChecked() && !ext_ok) {
		msg = "<b>No API key.</b> Set one above for this session, or "
		      "<tt>ANTHROPIC_API_KEY</tt> in your environment.";
	} else if (m_probe_state == probe_state::unknown) {
		msg = m_ai_local->isChecked() || m_ai_auto->isChecked()
		          ? "Local model not checked yet — press <b>Check now</b> to see "
		            "whether it is running."
		          : QString();
	} else if (m_probe_state == probe_state::reachable) {
		msg = m_ai_external->isChecked()
		          ? "Local model is reachable, but Claude is selected — requests "
		            "will leave this machine."
		          : "Local model is reachable — nothing will leave this machine.";
	} else {   // unreachable
		if (m_ai_local->isChecked())
			msg = "<b>The local model did not answer.</b> With <i>Local only</i> "
			      "set, the reorganizer and filter proposals stay unavailable "
			      "rather than falling back — which is what that setting is for.";
		else if (m_ai_auto->isChecked())
			msg = ext_ok ? "<b>The local model did not answer</b>, so Claude "
			                "would be used — with review before anything is sent."
			             : "<b>Neither backend is available.</b> Start Ollama, or "
			                "set an API key.";
	}
	m_ai_status->setText(msg);
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
	if (m_local_ai) {
		m_ollama_url->setText(m_local_ai->endpoint().toString());
		m_ollama_model->setText(m_local_ai->model());
		m_probe_timeout->setValue(m_local_ai->probe_timeout());
	}
	if (m_external_ai)
		m_claude_model->setText(m_external_ai->model());

	switch (settings_store::ai_mode()) {
		case ai_choice::local_only: m_ai_local->setChecked(true); break;
		case ai_choice::external:   m_ai_external->setChecked(true); break;
		case ai_choice::automatic:  m_ai_auto->setChecked(true); break;
	}

	update_custom_state();
	update_ai_state();
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

void settings_dialog::accept() {
	apply();
	QDialog::accept();
}

void settings_dialog::apply() {
	if (m_policy) {
		for (int i = 0; i < m_feature_combos.size(); ++i) {
			QComboBox *c = m_feature_combos[i];
			if (!c)
				continue;
			const auto want =
				static_cast<policy::setting>(c->currentData().toInt());
			const auto f = static_cast<policy::feature>(i);
			if (m_policy->global_default(f) != want)
				m_policy->set_global_default(f, want);
		}
	}
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

	if (m_local_ai) {
		const QString url = m_ollama_url->text().trimmed();
		m_local_ai->set_endpoint(QUrl(url.isEmpty() ? "http://localhost:11434"
		                                            : url));
		const QString model = m_ollama_model->text().trimmed();
		if (!model.isEmpty())
			m_local_ai->set_model(model);
		m_local_ai->set_probe_timeout(m_probe_timeout->value());
		// No probe here. Pressing OK is not a request to contact anything, and
		// whatever needs a backend re-probes when it needs one anyway.
	}
	if (m_external_ai) {
		const QString model = m_claude_model->text().trimmed();
		if (!model.isEmpty())
			m_external_ai->set_model(model);
		const QString key = m_claude_key->text().trimmed();
		if (!key.isEmpty())
			m_external_ai->set_api_key(key);   // memory only, never stored
	}

	settings_store::set_ai_mode(m_ai_local->isChecked()    ? ai_choice::local_only
	                            : m_ai_external->isChecked() ? ai_choice::external
	                                                         : ai_choice::automatic);

	settings_store::save_from(m_players, m_downloads, m_torrents,
	                           m_local_ai, m_external_ai);
}
