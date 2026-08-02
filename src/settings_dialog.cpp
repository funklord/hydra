// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_dialog.h"
#include "claude_provider.h"
#include "download_manager.h"
#include "ollama_provider.h"
#include "player_launcher.h"
#include "torrent_download_source.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QTreeWidget>
#include <QPushButton>
#include "filter_list.h"
#include "consent_blocker.h"

#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QGuiApplication>
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

kiosk_config kiosk() {
	QSettings st;
	kiosk_config c;
	st.beginGroup("kiosk");
	c.home = QUrl(st.value("home").toString());
	const int w = st.value("designWidth", 0).toInt();
	const int h = st.value("designHeight", 0).toInt();
	// An invalid size means "whatever the screen is", which is the useful
	// default and cannot be spelled as a number.
	if (w > 0 && h > 0)
		c.design_size = QSize(w, h);
	c.scale = static_cast<scale_mode>(
		qBound(0, st.value("scale", int(scale_mode::reflow)).toInt(), 2));
	c.fit = static_cast<fit_mode>(
		qBound(0, st.value("fit", int(fit_mode::contain)).toInt(), 3));
	c.hide_cursor = st.value("hideCursor", true).toBool();
	c.idle_reset_seconds = qMax(0, st.value("idleReset", 0).toInt());
	c.watchdog = st.value("watchdog", true).toBool();
	// Defaults to true even though the stored value may say otherwise: see
	// set_kiosk. Escape is how someone gets out.
	c.allow_escape = st.value("allowEscape", true).toBool();
	st.endGroup();
	return c;
}

void set_kiosk(const kiosk_config &c) {
	QSettings st;
	st.beginGroup("kiosk");
	st.setValue("home", c.home.toString());
	st.setValue("designWidth", c.design_size.isValid() ? c.design_size.width() : 0);
	st.setValue("designHeight", c.design_size.isValid() ? c.design_size.height() : 0);
	st.setValue("scale", int(c.scale));
	st.setValue("fit", int(c.fit));
	st.setValue("hideCursor", c.hide_cursor);
	st.setValue("idleReset", c.idle_reset_seconds);
	st.setValue("watchdog", c.watchdog);
	st.setValue("allowEscape", c.allow_escape);
	st.endGroup();
}

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
                                  policy_engine *policy, filter_list *filters,
                                  const QString &filters_path,
                                  consent_blocker *consent,
                                  const QString &rules_path, QWidget *parent)
	: QDialog(parent), m_policy(policy), m_filters(filters),
	  m_filters_path(filters_path), m_consent(consent),
	  m_rules_path(rules_path), m_players(players),
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

	// Firefox and Chrome both let preferences be searched, and it is what makes
	// a category list scale: six pages is already more than anyone will read
	// through to find one switch.
	m_search = new QLineEdit(this);
	m_search->setObjectName("settings_search");
	m_search->setPlaceholderText("Find a setting…");
	m_search->setClearButtonEnabled(true);
	outer->addWidget(m_search);

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
	auto *kiosk_page  = new QWidget;
	auto *filter_page = new QWidget;
	auto *player_page = new QWidget;
	auto *dl_page     = new QWidget;
	auto *ai_page     = new QWidget;
	build_privacy_page(privacy_page);
	build_kiosk_page(kiosk_page);
	build_filter_page(filter_page);
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
	add_page(wrap(filter_page), "Filters");
	add_page(wrap(kiosk_page), "Kiosk");

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

	// Indexed after the pages are built, so what is searchable is whatever was
	// actually put on them.
	for (int i = 0; i < stack->count(); ++i)
		index_pages(stack->widget(i), i);

	m_results = new QListWidget(this);
	m_results->setObjectName("settings_results");
	m_results->setMaximumHeight(140);
	m_results->hide();
	outer->insertWidget(1, m_results);
	connect(m_search, &QLineEdit::textChanged, this, &settings_dialog::run_search);
	connect(m_results, &QListWidget::itemActivated, this,
	         [this](QListWidgetItem *it) {
		const int page = it->data(Qt::UserRole).toInt();
		m_categories->setCurrentRow(page);
		// Show the control, not merely its page. A result that drops you at the
		// top of a long page has answered a different question than the one
		// asked.
		if (auto *w = it->data(Qt::UserRole + 1).value<QWidget *>()) {
			w->setFocus(Qt::OtherFocusReason);
			if (auto *area = qobject_cast<QScrollArea *>(
			        m_categories->parentWidget()))
				Q_UNUSED(area)
			for (QWidget *p = w->parentWidget(); p; p = p->parentWidget())
				if (auto *sa = qobject_cast<QScrollArea *>(p)) {
					sa->ensureWidgetVisible(w);
					break;
				}
		}
	});
	connect(m_results, &QListWidget::itemClicked, m_results,
	         &QListWidget::itemActivated);

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
	load_kiosk();
}

void settings_dialog::index_pages(QWidget *page, int page_index) {
	// Labels, group titles, checkboxes and the labels QFormLayout attaches to a
	// control: between them that is every word a person could search for. The
	// widget recorded is the thing to reveal, which for a form row is the
	// control rather than its label.
	for (QWidget *w : page->findChildren<QWidget *>()) {
		QString text;
		if (auto *b = qobject_cast<QAbstractButton *>(w))       text = b->text();
		else if (auto *g = qobject_cast<QGroupBox *>(w))        text = g->title();
		else if (auto *l = qobject_cast<QLabel *>(w))           text = l->text();
		if (text.trimmed().isEmpty() || text.size() > 120)
			continue;
		text.remove('&');
		QWidget *target = w;
		if (auto *l = qobject_cast<QLabel *>(w)) {
			if (l->buddy())
				target = l->buddy();
			// A wall of explanatory prose is not a setting. Anything long is
			// indexed for its words but points at itself, which is fine.
		}
		m_index.append({ text.simplified(), page_index, target });
	}
}

void settings_dialog::run_search(const QString &text) {
	const QString q = text.trimmed();
	m_results->clear();
	if (q.size() < 2) {
		m_results->hide();
		return;
	}
	QSet<QString> seen;
	for (const searchable &s : m_index) {
		if (!s.text.contains(q, Qt::CaseInsensitive))
			continue;
		const QString key = s.text + "|" + QString::number(s.page);
		if (seen.contains(key))
			continue;
		seen.insert(key);
		auto *it = new QListWidgetItem(
			QString("%1  —  %2").arg(s.text.left(70),
			                          m_categories->item(s.page)->text()),
			m_results);
		it->setData(Qt::UserRole, s.page);
		it->setData(Qt::UserRole + 1, QVariant::fromValue(s.widget));
		if (m_results->count() >= 12)
			break;
	}
	m_results->setVisible(m_results->count() > 0);
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

void settings_dialog::build_filter_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);
	auto *intro = new QLabel(
		"Rules accepted from the filter-evolution loop (Tools → Evolve Ad "
		"Filters). Until now a rule could be accepted but never taken back "
		"except by editing the file by hand — which is a poor answer for a list "
		"built by accepting proposals one at a time, since the design assumes "
		"some will turn out wrong.", page);
	intro->setWordWrap(true);
	v->addWidget(intro);

	m_filter_view = new QTreeWidget(page);
	m_filter_view->setObjectName("filters");
	m_filter_view->setColumnCount(3);
	m_filter_view->setHeaderLabels({ "Rule", "Applies to", "Why" });
	m_filter_view->setRootIsDecorated(false);
	v->addWidget(m_filter_view, 1);

	auto *row = new QHBoxLayout;
	m_filter_remove = new QPushButton("&Remove selected", page);
	m_filter_remove->setObjectName("filter_remove");
	m_filter_remove->setEnabled(false);
	row->addWidget(m_filter_remove);
	row->addStretch(1);
	v->addLayout(row);

	m_filter_note = new QLabel(page);
	m_filter_note->setObjectName("filter_note");
	m_filter_note->setWordWrap(true);
	v->addWidget(m_filter_note);

	connect(m_filter_view, &QTreeWidget::itemSelectionChanged, this, [this] {
		m_filter_remove->setEnabled(!m_filter_view->selectedItems().isEmpty());
	});
	connect(m_filter_remove, &QPushButton::clicked, this, [this] {
		const auto sel = m_filter_view->selectedItems();
		if (sel.isEmpty() || !m_filters)
			return;
		// Removed from the list *and* written out here rather than at OK. A rule
		// that is blocking something now should stop blocking it now, and the
		// alternative is a window where pressing Cancel silently restores rules
		// the user watched disappear.
		if (m_filters->remove(sel.first()->text(0)) && !m_filters_path.isEmpty())
			m_filters->save(m_filters_path);
		rebuild_filter_list();
	});

	rebuild_filter_list();

	// The second corpus, on the same page because it is the same kind of thing:
	// small perishable facts about how sites behave, kept as data so they can be
	// corrected without a release — and, in the stated direction, exchanged.
	auto *rules_box = new QGroupBox("Learned site rules", page);
	auto *rv = new QVBoxLayout(rules_box);
	auto *rules_intro = new QLabel(
		"Consent-banner wording and ad-blocker detector names learned on this "
		"machine. Built-in rules are not listed: they come from the program and "
		"cannot be edited here.", rules_box);
	rules_intro->setWordWrap(true);
	rv->addWidget(rules_intro);

	m_rules_view = new QTreeWidget(rules_box);
	m_rules_view->setObjectName("site_rules");
	m_rules_view->setColumnCount(5);
	m_rules_view->setHeaderLabels(
		{ "Rule", "Kind", "Applies to", "Status", "From" });
	m_rules_view->setRootIsDecorated(false);
	rv->addWidget(m_rules_view, 1);

	auto *rrow = new QHBoxLayout;
	m_rules_remove = new QPushButton("R&emove", rules_box);
	m_rules_remove->setObjectName("rules_remove");
	m_rules_remove->setEnabled(false);
	rrow->addWidget(m_rules_remove);
	m_rules_copy = new QPushButton("&Copy the flagged ones", rules_box);
	m_rules_copy->setObjectName("rules_copy");
	rrow->addWidget(m_rules_copy);
	auto *rules_export = new QPushButton("E&xport…", rules_box);
	rules_export->setObjectName("rules_export");
	rrow->addWidget(rules_export);
	auto *rules_import = new QPushButton("&Import…", rules_box);
	rules_import->setObjectName("rules_import");
	rrow->addWidget(rules_import);
	auto *rules_forget = new QPushButton("Forget imported", rules_box);
	rules_forget->setObjectName("rules_forget");
	rrow->addWidget(rules_forget);
	rrow->addStretch(1);

	// The safety valve. If a rule set turns out to be careless or hostile,
	// undoing it has to be one action rather than a hunt through a list -- and
	// it must not take the user's own rules with it, which is the reason
	// `imported` is a separate field rather than something inferred.
	connect(rules_forget, &QPushButton::clicked, this, [this] {
		if (!m_consent)
			return;
		site_rules kept = m_consent->rules();
		const int gone = kept.forget_imported();
		m_consent->set_rules(kept);
		if (!m_rules_path.isEmpty())
			kept.save(m_rules_path);
		rebuild_site_rules();
		m_rules_note->setText(gone == 0
			? QString("Nothing had been imported.")
			: QString("Forgot %1 imported rule(s). What you learned here is "
			           "untouched.").arg(gone));
	});

	// Sharing, in the only form it takes for now: a file someone sends. The
	// transport is deliberately undecided — what a received rule has to prove is
	// the part that had to be right first, and that does not change when the
	// bytes eventually arrive some other way.
	connect(rules_export, &QPushButton::clicked, this, [this] {
		if (!m_consent)
			return;
		const QString path = QFileDialog::getSaveFileName(
			this, "Export learned rules", QString(), "Rule files (*.json)");
		if (path.isEmpty())
			return;
		QFile f(path);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			m_rules_note->setText("Could not write that file.");
			return;
		}
		f.write(QJsonDocument(m_consent->rules().export_learned())
		            .toJson(QJsonDocument::Indented));
		m_rules_note->setText(QString("Exported to %1.").arg(path));
	});
	connect(rules_import, &QPushButton::clicked, this, [this] {
		if (!m_consent)
			return;
		const QString path = QFileDialog::getOpenFileName(
			this, "Import rules", QString(), "Rule files (*.json)");
		if (path.isEmpty())
			return;
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly)) {
			m_rules_note->setText("Could not read that file.");
			return;
		}
		const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
		const site_rules::import_result got =
			site_rules::judge_import(doc.object(), QFileInfo(path).fileName());

		// Nothing is added by opening a file. What survived the check is shown,
		// and adding it is a second, deliberate act -- a rule set from elsewhere
		// is a licence to click buttons on pages you are signed into, and that
		// is not something to acquire by browsing to a filename.
		QString msg;
		if (!got.accepted.isEmpty())
			msg += QString("%1 rule(s) passed the safety check.\n")
			           .arg(got.accepted.size());
		for (const QString &why : got.refused)
			msg += "Refused: " + why + "\n";
		if (got.accepted.isEmpty()) {
			QMessageBox::information(this, "Import rules",
			                          msg.isEmpty() ? "Nothing to import." : msg);
			m_rules_note->setText("Nothing was imported.");
			return;
		}
		msg += "\nAdd them?";
		if (QMessageBox::question(this, "Import rules", msg) != QMessageBox::Yes)
			return;
		site_rules merged = m_consent->rules();
		for (const site_rule &r : got.accepted)
			merged.add(r);
		m_consent->set_rules(merged);
		if (!m_rules_path.isEmpty())
			merged.save(m_rules_path);
		rebuild_site_rules();
	});
	rv->addLayout(rrow);

	m_rules_note = new QLabel(rules_box);
	m_rules_note->setObjectName("rules_note");
	m_rules_note->setWordWrap(true);
	rv->addWidget(m_rules_note);
	v->addWidget(rules_box);

	connect(m_rules_view, &QTreeWidget::itemSelectionChanged, this, [this] {
		m_rules_remove->setEnabled(!m_rules_view->selectedItems().isEmpty());
	});
	connect(m_rules_remove, &QPushButton::clicked, this, [this] {
		const auto sel = m_rules_view->selectedItems();
		if (sel.isEmpty() || !m_consent)
			return;
		site_rules kept;
		for (const site_rule &r : m_consent->rules().all()) {
			if (!r.builtin && r.value == sel.first()->text(0))
				continue;
			if (r.builtin)
				continue;          // defaults() puts these back
			kept.add(r);
		}
		m_consent->set_rules(kept);
		if (!m_rules_path.isEmpty())
			kept.save(m_rules_path);
		rebuild_site_rules();
	});
	// What a maintainer needs and could not get: the flagged rules, in a form
	// that can be pasted into `site_rules::defaults()`. A rule that works
	// everywhere belongs in the binary, and "flagged" is worth nothing if
	// finding them means reading a JSON file by hand.
	connect(m_rules_copy, &QPushButton::clicked, this, [this] {
		if (!m_consent)
			return;
		QString out;
		for (const site_rule &r : m_consent->rules().promotable())
			out += QString("\tbuiltin(\"%1\", \"%2\", \"%3\");\n")
			           .arg(r.kind, QString(r.value).replace('"', "\\\""), r.note);
		QGuiApplication::clipboard()->setText(out);
		m_rules_note->setText(out.isEmpty()
			? QString("Nothing is flagged, so nothing was copied.")
			: QString("Copied %1 line(s) for site_rules::defaults().")
			      .arg(out.count('\n')));
	});

	rebuild_site_rules();
}

void settings_dialog::rebuild_site_rules() {
	if (!m_rules_view)
		return;
	m_rules_view->clear();
	if (!m_consent) {
		m_rules_note->setText("No rule store was supplied.");
		return;
	}
	int flagged = 0;
	for (const site_rule &r : m_consent->rules().all()) {
		if (r.builtin)
			continue;
		auto *it = new QTreeWidgetItem(m_rules_view);
		it->setText(0, r.value);
		it->setText(1, r.kind);
		it->setText(2, r.generic() ? QStringLiteral("every site") : r.host);
		// The flag is the whole point of the provenance field, so it is spelled
		// out rather than shown as a tick nobody can interpret.
		it->setText(3, r.promote ? QStringLiteral("→ ship as built-in")
		                          : QString());
		// Where it came from, because "learned here" and "somebody sent this"
		// are the difference between a rule you vouched for and one you did not.
		it->setText(4, r.imported
			? (r.origin.isEmpty() ? QStringLiteral("imported") : r.origin)
			: QStringLiteral("learned here"));
		if (r.promote)
			++flagged;
	}
	for (int i = 0; i < 5; ++i)
		m_rules_view->resizeColumnToContents(i);
	m_rules_note->setText(
		m_rules_view->topLevelItemCount() == 0
			? QString("Nothing learned yet. Rules arrive from Tools → Cookie "
			           "Banners We Missed.")
			: QString("%1 learned, %2 of them flagged to be shipped as built-ins "
			           "— a rule that works on every site belongs in the program "
			           "rather than in one person's file.")
			      .arg(m_rules_view->topLevelItemCount()).arg(flagged));
	m_rules_remove->setEnabled(false);
}

void settings_dialog::rebuild_filter_list() {
	if (!m_filter_view)
		return;
	m_filter_view->clear();
	if (!m_filters) {
		m_filter_note->setText("No filter list was supplied.");
		return;
	}
	for (const filter_rule &r : m_filters->rules()) {
		auto *it = new QTreeWidgetItem(m_filter_view);
		it->setText(0, r.text);
		// Not `r.scope` directly: for a cosmetic rule that field is the site the
		// rule applies on, but for `||host^` it is the host being *blocked*, so
		// this column was labelling a global tracker rule as though it only
		// applied on the tracker's own domain. Network rules are global here —
		// per-site ones would need `$domain=`, which the parser does not read.
		it->setText(1, r.cosmetic
		                   ? (r.scope.isEmpty() ? QStringLiteral("every site") : r.scope)
		                   : QStringLiteral("every site"));
		it->setText(2, r.note);
	}
	for (int i = 0; i < 3; ++i)
		m_filter_view->resizeColumnToContents(i);
	m_filter_note->setText(
		m_filters->rules().isEmpty()
			? QString("No rules yet. They arrive by accepting proposals in Tools → "
			           "Evolve Ad Filters.")
			: QString("%1 rule(s), stored in %2")
			      .arg(m_filters->rules().size())
			      .arg(m_filters_path.isEmpty() ? QString("memory only")
			                                     : m_filters_path));
	m_filter_remove->setEnabled(false);
}

void settings_dialog::build_kiosk_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);
	auto *intro = new QLabel(
		"Kiosk mode (View → Kiosk Mode, F11) shows a page fullscreen with no "
		"browser furniture. These are its defaults; until now they were only "
		"reachable by editing the source.", page);
	intro->setWordWrap(true);
	v->addWidget(intro);

	auto *what = new QGroupBox("What it shows", page);
	auto *wf = new QFormLayout(what);
	m_kiosk_home = new QLineEdit(what);
	m_kiosk_home->setPlaceholderText("blank = whatever tab you were on");
	wf->addRow("Home page:", m_kiosk_home);
	m_kiosk_idle = new QSpinBox(what);
	m_kiosk_idle->setRange(0, 86400);
	m_kiosk_idle->setSuffix(" s");
	m_kiosk_idle->setSpecialValueText("never");
	wf->addRow("Return home when idle:", m_kiosk_idle);
	v->addWidget(what);

	auto *how = new QGroupBox("How it is scaled", page);
	auto *hf = new QFormLayout(how);
	m_kiosk_scale = new QComboBox(how);
	m_kiosk_scale->addItem("Reflow — one zoom factor, the page re-lays out",
	                        int(scale_mode::reflow));
	m_kiosk_scale->addItem("None — native size, cropped by the stage",
	                        int(scale_mode::none));
	m_kiosk_scale->addItem("Geometric — exact transform (test on the target GPU)",
	                        int(scale_mode::geometric));
	m_kiosk_scale->setObjectName("kiosk_scale");
	hf->addRow("Scaling:", m_kiosk_scale);
	m_kiosk_fit = new QComboBox(how);
	m_kiosk_fit->addItem("Contain", int(fit_mode::contain));
	m_kiosk_fit->addItem("Cover", int(fit_mode::cover));
	m_kiosk_fit->addItem("Stretch", int(fit_mode::stretch));
	m_kiosk_fit->addItem("Actual size", int(fit_mode::actual));
	m_kiosk_fit->setObjectName("kiosk_fit");
	hf->addRow("Fit:", m_kiosk_fit);
	m_kiosk_w = new QSpinBox(how);
	m_kiosk_w->setRange(0, 16384);
	m_kiosk_w->setSpecialValueText("screen");
	m_kiosk_h = new QSpinBox(how);
	m_kiosk_h->setRange(0, 16384);
	m_kiosk_h->setSpecialValueText("screen");
	hf->addRow("Design width:", m_kiosk_w);
	hf->addRow("Design height:", m_kiosk_h);
	v->addWidget(how);

	auto *pair_note = new QLabel(
		"Not every pair means something. Reflow cannot stretch — one zoom "
		"factor cannot scale the axes independently — so it approximates with "
		"cover rather than pretending. Geometric is the only path that does "
		"genuine per-axis stretch, and it is the one that has historically "
		"rendered black on some GPUs, so try it on the hardware you will "
		"deploy on.", page);
	pair_note->setWordWrap(true);
	v->addWidget(pair_note);

	auto *unattended = new QGroupBox("Running unattended", page);
	auto *uv = new QVBoxLayout(unattended);
	m_kiosk_cursor = new QCheckBox("Hide the mouse pointer", unattended);
	uv->addWidget(m_kiosk_cursor);
	m_kiosk_dog = new QCheckBox("Reload if the page's process dies", unattended);
	uv->addWidget(m_kiosk_dog);
	m_kiosk_escape = new QCheckBox("Esc leaves kiosk mode", unattended);
	m_kiosk_escape->setObjectName("kiosk_escape");
	uv->addWidget(m_kiosk_escape);
	auto *escape_note = new QLabel(
		"<b>Turning that off locks the screen down.</b> Esc and F11 will not "
		"leave, and on a machine with no keyboard shortcut left there may be no "
		"way out except ending the process. It is here because unattended "
		"displays need it — not because it is a normal thing to switch on.",
		unattended);
	escape_note->setWordWrap(true);
	uv->addWidget(escape_note);
	v->addWidget(unattended);

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

void settings_dialog::load_kiosk() {
	const kiosk_config c = settings_store::kiosk();
	m_kiosk_home->setText(c.home.toString());
	m_kiosk_w->setValue(c.design_size.isValid() ? c.design_size.width() : 0);
	m_kiosk_h->setValue(c.design_size.isValid() ? c.design_size.height() : 0);
	m_kiosk_scale->setCurrentIndex(m_kiosk_scale->findData(int(c.scale)));
	m_kiosk_fit->setCurrentIndex(m_kiosk_fit->findData(int(c.fit)));
	m_kiosk_cursor->setChecked(c.hide_cursor);
	m_kiosk_idle->setValue(c.idle_reset_seconds);
	m_kiosk_dog->setChecked(c.watchdog);
	m_kiosk_escape->setChecked(c.allow_escape);
}

void settings_dialog::apply_kiosk() {
	kiosk_config c;
	c.home = QUrl(m_kiosk_home->text().trimmed());
	if (m_kiosk_w->value() > 0 && m_kiosk_h->value() > 0)
		c.design_size = QSize(m_kiosk_w->value(), m_kiosk_h->value());
	c.scale = static_cast<scale_mode>(m_kiosk_scale->currentData().toInt());
	c.fit = static_cast<fit_mode>(m_kiosk_fit->currentData().toInt());
	c.hide_cursor = m_kiosk_cursor->isChecked();
	c.idle_reset_seconds = m_kiosk_idle->value();
	c.watchdog = m_kiosk_dog->isChecked();
	c.allow_escape = m_kiosk_escape->isChecked();
	settings_store::set_kiosk(c);
}

void settings_dialog::accept() {
	apply();
	apply_kiosk();
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
