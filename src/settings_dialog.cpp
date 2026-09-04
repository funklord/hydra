#include "settings_dialog.h"
#include "claude_provider.h"
#include "download_manager.h"
#include "flow_layout.h"
#include "ollama_provider.h"
#include "player_launcher.h"
#include "torrent_download_source.h"
#include "web_view_factory.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QPalette>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QResizeEvent>
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
#include <QPointer>
#include <QStackedWidget>
#include "policy_engine.h"
#include "settings_bundle.h"
#include "theme.h"
#include <QHBoxLayout>
#include <QFrame>
#include <QPalette>
#include <QFont>
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

// **The kiosk group was written somewhere else, by nobody's decision.**
// `kiosk()` and `set_kiosk()` used a default-constructed `QSettings` while
// every other accessor here went through `open_settings()`. Nothing sets an
// organisation name, so the default constructor resolves to
// `<config>/Unknown Organization/Hydra.conf` -- measured on this machine, where
// that file exists and holds exactly the nine keys `set_kiosk` writes, while
// `<config>/hydra/hydra.ini` holds every other setting and no `[kiosk]` group
// at all.
//
// Two things about it are worth writing down, because both are the opposite of
// what the symptom suggests:
//
//   * **It worked.** The file is there and reads come back correct. The defect
//     is the location, not a failure -- so a fix that just moved the accessor
//     would silently strand a working configuration.
//   * **`status()` is `AccessError` on that object, before and after a
//     successful read.** So a migration that gated on the status would refuse
//     to migrate exactly the case it exists for. It gates on there being keys.
//
// Carried over rather than reset, once, on the first read that finds the new
// location empty. A kiosk configuration is somebody's deployment -- a home url,
// a design size, possibly `allowEscape=false` on a screen with no keyboard --
// and losing it silently costs more than a few lines here. The old file is left
// where it is: it is not ours to delete, it is harmless once nothing reads it,
// and leaving it means an operator who wants to check the values still can.
void migrate_kiosk_group(QSettings &to) {
	if (to.contains("kiosk/home"))
		return;                    // already here; `set_kiosk` always writes it

	QSettings from;                // the wrong location, spelled the wrong way
	from.beginGroup("kiosk");
	const QStringList keys = from.childKeys();
	if (keys.isEmpty())
		return;
	for (const QString &key : keys)
		to.setValue("kiosk/" + key, from.value(key));
	from.endGroup();
	to.sync();
}

}  // namespace

// --------------------------------------------------------------- the store --

namespace settings_store {

kiosk_config kiosk() {
	QSettings st = open_settings();
	migrate_kiosk_group(st);
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
	// Defaults to false, and a stored value is the only thing that turns it on.
	// It deletes the browser's cookies -- see `kiosk_config` for why that is
	// asked for rather than assumed.
	c.clear_between_sessions = st.value("clearBetweenSessions", false).toBool();
	st.endGroup();
	return c;
}

void set_kiosk(const kiosk_config &c) {
	QSettings st = open_settings();
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
	st.setValue("clearBetweenSessions", c.clear_between_sessions);
	st.endGroup();
	// Written through, like every other setter here. The default-constructed
	// QSettings this used to use was destroyed at the end of the call and
	// synced on the way out; `open_settings()` returns a value that is too, but
	// saying it keeps this the same shape as `set_appearance` and friends.
	st.sync();
}

theme::choice appearance() {
	// Stored as the word rather than a number, because these files are meant to
	// be read: "appearance=dark" says what it is where "appearance=2" does not.
	return theme::from_name(
	  open_settings().value("ui/appearance", "system").toString());
}

void set_appearance(theme::choice c) {
	QSettings s = open_settings();
	s.setValue("ui/appearance", theme::name_of(c));
	s.sync();
}

QString search_engine() {
	// **DuckDuckGo by default, and the default is a decision rather than a
	// pick.** This is the only setting in the program that sends what somebody
	// typed to a third party, so the shipped answer should be the one that
	// asks least of them: no account, no profile built from the queries, and
	// no redirect through a tracker on the way to the result. A browser that
	// blocks trackers on every page and then routes its own address bar
	// through one would be arguing with itself.
	//
	// Stored as the whole template rather than an engine name from a list, so
	// changing it needs no code here -- a self-hosted SearxNG or a company
	// search is one line in the settings file, and neither would ever be worth
	// adding to a menu.
	QSettings s = open_settings();
	return s.value("ui/searchEngine", "https://duckduckgo.com/?q=%1").toString();
}

void set_search_engine(const QString &tmpl) {
	QSettings s = open_settings();
	s.setValue("ui/searchEngine", tmpl);
	s.sync();
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
	{ policy::feature::desktop_site,        "Content" },
	{ policy::feature::ads,                 "Content" },
	{ policy::feature::cookies,             "Cookies and site data" },
	{ policy::feature::third_party_cookies, "Cookies and site data" },
	{ policy::feature::cookie_notices,      "Cookies and site data" },
	{ policy::feature::geolocation,         "Permissions" },
	{ policy::feature::camera,              "Permissions" },
	{ policy::feature::microphone,          "Permissions" },
	{ policy::feature::notifications,       "Permissions" },
	// **Three capabilities that had no row here at all.**
	//
	// This table is hand-written and shorter than the enum, and nothing reports
	// the difference -- a feature added and forgotten simply stops appearing on
	// this page, silently, because no assertion can fail about a row nobody
	// wrote. `site_policy_dialog`'s equivalent has a guard that says so, and it
	// is what caught `screen_share` missing from that one.
	//
	// Two of these mattered less while they were permanently blocked. They do
	// not any more: `pointer_lock` now asks, and a capability that asks and
	// cannot be configured from the settings page is one whose prompt somebody
	// will want to stop and cannot. `clipboard_read` is still blocked for a
	// reason of its own -- the engine gates it -- and belongs here beside them
	// rather than being the one that stays hidden.
	{ policy::feature::clipboard_read,      "Permissions" },
	{ policy::feature::pointer_lock,        "Permissions" },
	{ policy::feature::screen_share,        "Permissions" },
	{ policy::feature::referer,             "Sent with requests" },
	{ policy::feature::autofill,            "Passwords" },
};

// **Deliberately not on this page, each for a stated reason.**
//
// The list exists so that leaving a feature off is a decision somebody wrote
// down, rather than the silence that has already cost this table four missing
// rows. A feature added to the enum and put in neither place now says so.
const policy::feature k_not_on_this_page[] = {
	// Belongs with the extractor's own settings, where the tier it governs is
	// explained; a bare "Extractor may fetch" here means nothing to anybody who
	// has not read that page.
	policy::feature::extractor_fetch,
	// A per-site judgement about one site's traffic, which is what the shield is
	// for. A global default for it would be a switch that turns the media badge
	// off everywhere, and that is not a thing anybody has asked for.
	policy::feature::media_detect,
};

// Every feature either has a row here or is excused above -- exactly one of the
// two, exactly once.
//
// `site_policy_dialog` has had this check for a while and it has now caught two
// features in a day. This page had no equivalent, and the comment beside that
// one said so: a feature added to the enum simply stopped appearing here,
// silently, because no assertion can fail about a row nobody wrote.
bool privacy_layout_is_complete() {
	const int n = policy::feature_count();
	QList<int> seen(n, 0);
	for (const feature_group &fg : k_privacy_layout) {
		const int i = static_cast<int>(fg.f);
		if (i < 0 || i >= n)
			return false;
		++seen[i];
	}
	for (policy::feature f : k_not_on_this_page) {
		const int i = static_cast<int>(f);
		if (i < 0 || i >= n)
			return false;
		++seen[i];
	}
	for (int i = 0; i < n; ++i)
		if (seen[i] != 1)
			return false;
	return true;
}

}  // namespace

settings_dialog::settings_dialog(player_launcher *players,
                                  download_manager *downloads,
                                  torrent_download_source *torrents,
                                  ollama_provider *local_ai,
                                  claude_provider *external_ai,
                                  policy_engine *policy, filter_list *filters,
                                  const QString &filters_path,
                                  consent_blocker *consent,
                                  const QString &rules_path, QWidget *parent,
                                  web_view_factory *views,
                                  annoyance_log *annoyances)
  : QDialog(parent), m_policy(policy), m_filters(filters),
    m_filters_path(filters_path), m_consent(consent), m_views(views),
    m_annoyances(annoyances),
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

	// Firefox and Chrome both let preferences be searched, and it is what makes
	// a category list scale: six pages is already more than anyone will read
	// through to find one switch.
	//
	// **Above** the pages, which is where both of them put it and where a reader
	// looks first. It was below, under the page stack and next to the OK button,
	// which is where a filter for a *result list* would go -- and it read as one.
	// Nothing asserted the position and nothing could: it took looking at a
	// screenshot, which is why this driver now takes them.
	m_search = new QLineEdit(this);
	m_search->setObjectName("settings_search");
	m_search->setPlaceholderText("Find a setting…");
	m_search->setClearButtonEnabled(true);
	outer->addWidget(m_search);

	auto *split = new QHBoxLayout;
	outer->addLayout(split, 1);

	m_categories = new QListWidget(this);
	m_categories->setObjectName("categories");
	m_categories->setMaximumWidth(190);
	split->addWidget(m_categories);

	// **The same list, as a dropdown, for when there is no room for a
	// sidebar.** 190 pixels of permanent category list is a fair trade on a
	// desktop and a bad one on a 360-pixel screen, where it leaves about 150
	// for the settings themselves -- enough to wrap every description to two
	// words a line and still clip the controls off the right edge.
	//
	// Sits under the search box so it reads as saying which page is below it,
	// and stays hidden until `update_layout_mode` says otherwise. Populated by
	// `add_page` alongside the list, so the two cannot fall out of step.
	m_category_pick = new QComboBox(this);
	m_category_pick->setObjectName("categories_narrow");
	m_category_pick->hide();
	outer->insertWidget(outer->indexOf(m_search) + 1, m_category_pick);

	auto *stack = new QStackedWidget(this);
	m_stack = stack;
	stack->setObjectName("pages");
	split->addWidget(stack, 1);
	connect(m_categories, &QListWidget::currentRowChanged,
	         stack, &QStackedWidget::setCurrentIndex);
	// Both directions, so whichever one is on screen stays right and the other
	// is already correct when the width changes under it. `setCurrentIndex` on
	// an unchanged index emits nothing, so there is no loop between them.
	connect(m_category_pick, &QComboBox::currentIndexChanged, this,
	         [this](int i) { m_categories->setCurrentRow(i); });
	connect(m_categories, &QListWidget::currentRowChanged, this,
	         [this](int i) { m_category_pick->setCurrentIndex(i); });

	// **Named, because a diagnostic that says "QWidget" names nothing.**
	// `try_phone` reports the widest children of a dialog that will not fit a
	// phone screen, and on this one the answer came back as four anonymous
	// QWidgets -- which is the question again, not an answer.
	auto *privacy_page = new QWidget;
	auto *appearance_page = new QWidget;
	auto *kiosk_page  = new QWidget;
	auto *filter_page = new QWidget;
	auto *player_page = new QWidget;
	auto *dl_page     = new QWidget;
	auto *ai_page     = new QWidget;
	privacy_page->setObjectName("page_privacy");
	appearance_page->setObjectName("page_appearance");
	kiosk_page->setObjectName("page_kiosk");
	filter_page->setObjectName("page_filters");
	player_page->setObjectName("page_players");
	dl_page->setObjectName("page_downloads");
	ai_page->setObjectName("page_ai");
	build_privacy_page(privacy_page);
	build_appearance_page(appearance_page);
	build_kiosk_page(kiosk_page);
	build_filter_page(filter_page);
	build_player_page(player_page);
	build_download_page(dl_page);
	build_ai_page(ai_page);

	// Each page scrolls. These pages carry explanatory text, and text height
	// depends on the system font and the user's scaling -- any fixed dialog
	// height is a guess that silently clips the explanation on somebody's
	// machine, which is worse than a scrollbar.
	auto wrap = [stack](QWidget *page) {
		auto *area = new QScrollArea(stack);
		area->setWidget(page);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		// Never sideways. Everything on these pages wraps or is a control, so a
		// horizontal scrollbar can only ever mean the layout is a few pixels
		// out -- and that is exactly how it appeared: the vertical scrollbar
		// arrives, takes ~15px off the viewport, and content laid out at the
		// wider size then overflows by that much. The result was a full-width
		// scrollbar whose slider filled its own track, scrolling nothing, on
		// pages that measured as fitting comfortably.
		area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		// A gutter, set here so every page gets the same one rather than each
		// remembering to. Without it a right-aligned control ends exactly where
		// the vertical scrollbar begins, which reads as clipping -- it was
		// mistaken for clipping here, and the difference is a margin rather
		// than a layout fault.
		if (QLayout *lay = page->layout())
			lay->setContentsMargins(16, 12, 22, 16);
		return area;
	};
	// Privacy first, because it is the one page that is about what the browser
	// refuses to do on your behalf and the others are about conveniences.
	auto add_page = [&](QWidget *w, const QString &name) {
		stack->addWidget(w);
		m_categories->addItem(name);
		m_category_pick->addItem(name);
	};
	// A QListWidgetItem draws its text literally -- no mnemonics -- so "&&"
	// arrived on screen as two ampersands. The escape belongs to menus and
	// buttons, not here.
	add_page(wrap(privacy_page), "Privacy & security");
	add_page(wrap(appearance_page), "Appearance");
	add_page(wrap(player_page), "Media & players");
	add_page(wrap(dl_page), "Downloads");
	add_page(wrap(filter_page), "Filters");
	add_page(wrap(kiosk_page), "Kiosk");

	// The AI page scrolls, but its status line does not. It is the answer to
	// the button directly above it, and a result that has scrolled out of
	// sight is the same as no result -- pressing Check now would appear to do
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
	// Directly under the box that fills it, which is now index 1.
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
			// **The scroll area is found by walking up from the widget**, not
			// by casting the category list's parent -- which is a different
			// widget and was never what needed scrolling.
			//
			// That cast used to sit above this loop as an `if` whose only
			// guarded statement was `Q_UNUSED` on its own result, so the loop
			// ran either way and the condition decided nothing. Harmless, and
			// invisible until an indentation pass lined the loop up under it
			// and the compiler pointed out that the shape was a lie
			// (-Wmisleading-indentation). Removed rather than re-indented:
			// re-indenting would have preserved a condition that has no effect
			// and no meaning.
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
	  QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
	    QDialogButtonBox::RestoreDefaults,
	  this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// One button, in the standard place, that names the page it acts on.
	//
	// Firefox puts a "Restore Defaults" inside each section and Chrome has one
	// global reset; this is the middle, and the middle is here because a global
	// reset of a browser that holds per-site rules and learned filters is a
	// bigger hammer than anyone reaches for on purpose. Naming the page in the
	// label is what makes one button unambiguous.
	m_restore = buttons->button(QDialogButtonBox::RestoreDefaults);
	m_restore->setObjectName("restore_defaults");
	connect(m_restore, &QPushButton::clicked, this, [this] {
		restore_page_defaults(m_categories->currentRow());
	});
	connect(m_categories, &QListWidget::currentRowChanged, this,
	         &settings_dialog::update_restore_button);
	// After the button exists, not when the first page is selected: that happens
	// while the button box is still being built, so the call landed on a null
	// pointer and the label kept Qt's generic "Restore Defaults".
	update_restore_button();
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


// --- browser-shaped rows ---------------------------------------------------
//
// A settings row here is what Firefox and Chrome both settled on: the name of
// the thing, one line under it saying what the thing *is*, and the control on
// the right. The alternative -- a QFormLayout of "Label:" and a combo -- fits
// more rows on the screen and tells you nothing, so the reader has to already
// know what "Referer header" means before the page can help them.
//
// The description is dimmed and one step smaller, which is the whole visual
// grammar: it says "context, not another setting" without needing a rule about
// it.
namespace {

QLabel *dim_label(const QString &text, QWidget *parent) {
	auto *l = new QLabel(text, parent);
	// Named so a test can find every description and check it is legible,
	// whatever it is being coloured by -- searching for the colouring itself
	// would stop finding them the moment the colouring is what broke.
	l->setObjectName("help");
	l->setWordWrap(true);
	QFont f = l->font();
	f.setPointSizeF(f.pointSizeF() * 0.92);
	l->setFont(f);
	// Dimmed by *role*, not by writing a colour in. Setting a palette on the
	// widget freezes it: these labels are built once, and a colour computed
	// under the theme that happened to be current stays put when the theme
	// changes. That is not hypothetical -- lightening the text worked in dark
	// and left white-on-white the moment the scheme went light, which is what
	// the live preview in this very dialog does.
	//
	// PlaceholderText is the role Qt already keeps a legible dimmed foreground
	// in, and asking for it by name means the style recomputes it on every
	// palette change for free.
	// Dimmed by *role*, not by writing a colour in. Setting a palette on the
	// widget freezes it: these labels are built once, and a colour computed
	// under whichever theme was current stays put when the theme changes. Not
	// hypothetical -- lightening the text read fine in dark and went white on
	// white the moment the scheme went light, which is exactly what the live
	// preview in this dialog does. PlaceholderText is the role Qt already keeps
	// a legible dimmed foreground in, and asking for it by name means the style
	// recomputes it on every palette change for free.
	l->setForegroundRole(QPalette::PlaceholderText);
	return l;
}

// A section heading: bold, with a hairline under it. Not a QGroupBox -- a box
// around every three rows is a lot of furniture for something whose only job is
// to say "these belong together".
QWidget *section_heading(const QString &text, QWidget *parent) {
	auto *holder = new QWidget(parent);
	auto *v = new QVBoxLayout(holder);
	v->setContentsMargins(0, 14, 0, 4);
	v->setSpacing(4);
	auto *title = new QLabel(text, holder);
	QFont f = title->font();
	f.setBold(true);
	title->setFont(f);
	v->addWidget(title);
	auto *line = new QFrame(holder);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Plain);
	v->addWidget(line);
	return holder;
}

// One row. `control` is adopted; `help` may be empty for settings whose name is
// the whole story.
// `wide` is for controls that hold text rather than a choice -- a path, a
// hostname. A combo box wants its own width and looks wrong stretched; a line
// edit squeezed to its size hint shows the *end* of a path and nothing else,
// which is the half nobody needs.
QWidget *settings_row(const QString &title, const QString &help,
                       QWidget *control, QWidget *parent, bool wide = false) {
	auto *row = new QWidget(parent);
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 6, 0, 6);
	h->setSpacing(12);

	auto *text_col = new QVBoxLayout;
	text_col->setSpacing(2);
	auto *name = new QLabel(title, row);
	name->setWordWrap(true);
	text_col->addWidget(name);
	if (!help.isEmpty())
		text_col->addWidget(dim_label(help, row));
	h->addLayout(text_col, wide ? 1 : 2);

	// The control keeps to its own width and sits against the right edge, so a
	// column of them lines up however long the descriptions are -- unless it is
	// one of the wide ones, which take a share of the row instead.
	control->setParent(row);
	if (wide)
		h->addWidget(control, 1, Qt::AlignTop);
	else
		h->addWidget(control, 0, Qt::AlignRight | Qt::AlignTop);

	// So search can jump to the control rather than to its name.
	name->setBuddy(control);
	return row;
}

// Turn a clear's report into something a person can read.
//
// **Each store gets its own sentence**, because the interesting outcome is a
// mixed one: cookies counted and gone, the cache still running, visited links
// asked for and unconfirmable. A single "Done" would flatten all three into
// the most optimistic of them, which is the summary that makes a report
// useless.
QString describe_clear(const web_view_factory::clear_report &r) {
	using state = web_view_factory::clear_state;
	auto say = [](const QString &name, state s, const QString &extra) {
		switch (s) {
		case state::not_asked:   return QString();
		case state::done:        return name + ": cleared" + extra + ".";
		case state::unconfirmed: return name + ": requested, not confirmed"
			                                 + extra + ".";
		case state::refused:     return name + ": not cleared" + extra + ".";
		}
		return QString();
	};

	QStringList lines;
	lines << say("Cookies", r.cookies,
	              r.cookies_removed >= 0
	                ? QString(" -- %1 removed").arg(r.cookies_removed)
	                : QString());
	lines << say("Cached files", r.cache, QString());
	lines << say("Visited links", r.visited_links, QString());
	lines.removeAll(QString());
	lines += r.notes;
	return lines.join("\n");
}

}  // namespace

// **Its own page, because it was on the wrong one.** The colour scheme sat at
// the top of Privacy & security, under that page's own opening line -- "what
// every site is allowed to do unless you have said otherwise" -- which is not
// what a colour scheme is. It was put there to be findable rather than because
// it belonged, and a page of its own is more findable still.
//
// It also fixes something that followed from the misplacement: "Restore Privacy
// & security defaults" never touched the scheme, so the one setting on that
// page which was not a permission was also the one its restore button ignored.
void settings_dialog::build_appearance_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);
	v->setSpacing(0);

	auto *intro = new QLabel(
	  "How Hydra itself looks. Everything else in here is about what pages are "
	  "allowed to do; this is about the window around them.", page);
	intro->setWordWrap(true);
	v->addWidget(intro);

	v->addWidget(section_heading("Appearance", page));
	m_appearance = new QComboBox(page);
	m_appearance->setObjectName("appearance");
	m_appearance->addItem("Follow the desktop", int(theme::choice::system));
	m_appearance->addItem("Light", int(theme::choice::light));
	m_appearance->addItem("Dark", int(theme::choice::dark));
	const theme::choice now = settings_store::appearance();
	m_appearance->setCurrentIndex(m_appearance->findData(int(now)));
	// Applied as it is chosen rather than on OK: this one is visible, so seeing
	// it is how you decide whether you want it. Cancel puts back what was
	// stored, which is what makes trying one safe.
	connect(m_appearance, &QComboBox::currentIndexChanged, this, [this] {
		theme::apply(static_cast<theme::choice>(m_appearance->currentData().toInt()));
	});
	v->addWidget(settings_row(
	  "Colour scheme",
	  "Following the desktop is the default, and it keeps following: a system "
	  "that switches at sunset takes Hydra with it. Web pages pick this up when "
	  "Hydra next starts — the engine reads it once, at launch.",
	  m_appearance, page));

	v->addStretch(1);
}

// **The only control on this dialog that does something the moment you press
// it**, and the divergence is deliberate rather than an oversight.
//
// Everything else here is pending until OK, so that Cancel is the undo. That
// contract cannot be kept for a deletion: there is no undo for a cookie, so
// "pending until OK" would only mean the user pressed a button and could not
// tell whether anything had happened. A confirmation before, and a report of
// what actually went afterwards, are what stand in for Cancel here.
void settings_dialog::build_clear_section(QVBoxLayout *v, QWidget *page) {
	v->addWidget(section_heading("Clear browsing data", page));

	v->addWidget(dim_label(
	  "The settings above decide what sites may store from now on. This is the "
	  "only thing in Hydra that deletes what they have stored already, and it "
	  "cannot be undone. Your tabs, their history and the tree are not touched.",
	  page));

	if (!m_views) {
		v->addWidget(new QLabel("No web engine was supplied, so there is "
		                         "nothing here to clear.", page));
		return;
	}

	// Cookies and cache are on because they are what "clear browsing data"
	// means to almost everybody who goes looking for it. Visited links is off
	// because it is the one whose loss is purely cosmetic to most people and
	// genuinely wanted by some -- the purple-link trail is a readable list of
	// where you have been.
	m_clear_cookies = new QCheckBox(page);
	m_clear_cookies->setObjectName("clear_cookies");
	m_clear_cookies->setChecked(true);
	v->addWidget(settings_row(
	  "Cookies",
	  "Every site's cookies, which is every login the browser is holding. "
	  "Site storage -- localStorage, IndexedDB, service workers -- is not "
	  "included; the report below says where it is and why.",
	  m_clear_cookies, page));

	m_clear_cache = new QCheckBox(page);
	m_clear_cache->setObjectName("clear_cache");
	m_clear_cache->setChecked(true);
	v->addWidget(settings_row(
	  "Cached files",
	  "Pages, images and scripts kept on disk to make a revisit fast. Costs "
	  "nothing but a slower reload.",
	  m_clear_cache, page));

	// **Its own control, and off by default, because it is not a trace.**
	// `main_window::forget_shell_caches` states the rule this follows:
	// forgetting is about what browsing left behind, not about undoing
	// decisions -- and an annoyance report is something a person went to the
	// trouble of filing. So it is never swept along with cookies.
	//
	// It is here at all because it holds the host and the address that was
	// open, which is a record of where somebody has been by this project's own
	// description of it, and there was no way to remove one from inside Hydra:
	// `clear_host` and `clear_all` had no caller outside the tests.
	if (m_annoyances) {
		m_clear_reports = new QCheckBox(page);
		m_clear_reports->setObjectName("clear_reports");
		v->addWidget(settings_row(
		  "Reports of sites that annoyed you",
		  "What the Annoyed button recorded: the site, the page you were on "
		  "and what you chose to do. Yours rather than something a site left, "
		  "so it is never cleared unless you ask here.",
		  m_clear_reports, page));
	}

	m_clear_links = new QCheckBox(page);
	m_clear_links->setObjectName("clear_visited_links");
	v->addWidget(settings_row(
	  "Visited links",
	  "The record that colours a link as already followed. On a shared screen "
	  "it is a readable list of where the last person went.",
	  m_clear_links, page));

	m_clear_go = new QPushButton("&Clear now…", page);
	m_clear_go->setObjectName("clear_now");
	connect(m_clear_go, &QPushButton::clicked, this,
	         &settings_dialog::clear_browsing_data);
	auto *go_row = new QHBoxLayout;
	go_row->addStretch(1);
	go_row->addWidget(m_clear_go);
	v->addLayout(go_row);

	// Starts empty and is only ever filled by a completed clear. A label that
	// said something before anything had been done would be the blind claim
	// this whole section is written to avoid.
	m_clear_note = dim_label(QString(), page);
	v->addWidget(m_clear_note);
}

void settings_dialog::clear_browsing_data() {
	if (!m_views || !m_clear_cookies)
		return;

	web_view_factory::browsing_data what;
	what.cookies       = m_clear_cookies->isChecked();
	what.cache         = m_clear_cache->isChecked();
	what.visited_links = m_clear_links->isChecked();
	const bool reports = m_clear_reports && m_clear_reports->isChecked();
	if (!what.any() && !reports) {
		m_clear_note->setText("Nothing was ticked, so nothing was cleared.");
		return;
	}

	// Named one by one rather than "the selected items", because the whole
	// value of a confirmation is that it repeats the decision back in words.
	QStringList named;
	if (what.cookies)
		named << "cookies, and with them every login";
	if (what.cache)
		named << "cached files";
	if (what.visited_links)
		named << "the record of which links you have followed";
	if (reports)
		named << "every report you filed with the Annoyed button";

	QMessageBox box(this);
	box.setIcon(QMessageBox::Warning);
	box.setWindowTitle("Clear browsing data");
	box.setText("This deletes " + named.join(", ") + ".");
	box.setInformativeText(
	  "It applies to every site and cannot be undone. Your tabs, their history "
	  "and the tree are left alone.");
	box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
	box.button(QMessageBox::Yes)->setText("Clear");
	// Cancel is the default button: the safe answer is the one a stray Return
	// lands on.
	box.setDefaultButton(QMessageBox::Cancel);
	if (box.exec() != QMessageBox::Yes)
		return;

	m_clear_go->setEnabled(false);
	m_clear_note->setText("Clearing…");

	// **Guarded, because the answer outlives the window.** None of these
	// stores empties synchronously, and this dialog is created on the stack
	// and exec()'d -- pressing Clear and then OK destroys it while the
	// deletion is still in flight. The factory is not, so the clear itself
	// finishes either way; what the guard prevents is writing the result into
	// a label that has been freed.
	// **Said before the backend answers, not after.** The shell's own caches
	// are cleared synchronously and the report may never arrive -- a store can
	// answer `unconfirmed` or nothing at all -- so hanging this off the
	// callback would make forgetting depend on a backend being talkative.
	emit browsing_data_cleared();
	if (reports)
		emit annoyance_reports_cleared();

	// **Nothing to ask the engine when only the reports were ticked.** Those
	// are the shell's own file; handing the backend an empty request would
	// make it answer about a clear it was never given.
	if (!what.any()) {
		m_clear_go->setEnabled(true);
		m_clear_note->setText("Your Annoyed reports were cleared.");
		return;
	}

	QPointer<settings_dialog> alive(this);
	m_views->clear_browsing_data(what, [alive](
	    const web_view_factory::clear_report &r) {
		if (!alive)
			return;
		alive->m_clear_go->setEnabled(true);
		alive->m_clear_note->setText(describe_clear(r));
	});
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

	// Clearing goes directly under the cookie policy, which is where Firefox
	// puts it and for the reason the table above already gives: the question
	// "what may a site keep" and the question "get rid of what it kept" arrive
	// together, and until now this page could only answer the first.
	//
	// The name is matched against the table, so it follows the group if the
	// group is renamed -- and the flag is what stops it *vanishing* if the
	// group is renamed. A section that silently fails to appear because
	// somebody edited a string in a table is the same failure the "Other"
	// sweep below exists to prevent.
	bool clear_section_placed = false;

	// Checked rather than trusted, for the reason the shield's twin gives: the
	// cost of the table being short by one is a setting that silently cannot be
	// reached from this page, which is invisible from inside it.
	if (!privacy_layout_is_complete())
		qWarning("settings_dialog: the privacy layout does not account for every "
		          "policy feature exactly once -- a setting is missing from this "
		          "page, or is listed both here and as excluded");

	for (const QString &group_name : order) {
		QList<policy::feature> in_group;
		for (int i = 0; i < policy::feature_count(); ++i) {
			const auto f = static_cast<policy::feature>(i);
			QString mine = "Other";
			for (const feature_group &fg : k_privacy_layout)
				if (fg.f == f)
					mine = QString::fromUtf8(fg.group);
			if (mine == group_name)
				in_group << f;
		}
		if (in_group.isEmpty())
			continue;

		v->addWidget(section_heading(group_name, page));
		for (policy::feature f : in_group) {
			auto *combo = new QComboBox(page);
			// Global defaults are always allow or block. "Default" is what a
			// *site* says when it has no opinion and falls through to here, so
			// offering it at this level would be a setting that points at itself.
			combo->addItem("Allow", int(policy::setting::allow));
			combo->addItem("Block", int(policy::setting::block));
			// **A third answer, on the features that can carry one.** Offered
			// only where `policy::can_ask` says a prompt exists, because
			// `is_allowed` grants on `allow` alone -- setting `ask` on a
			// feature nothing prompts for would be a control that silently
			// means Block, which is the worst of the three things it could do.
			//
			// Absent rather than greyed here, unlike the per-site dialog. That
			// one keeps a fixed entry count because its index *is* its meaning;
			// this one carries the setting in the item's data, so a row with
			// two entries and a row with three both read back correctly.
			if (policy::can_ask(f))
				combo->addItem("Ask each time", int(policy::setting::ask));
			const policy::setting cur = m_policy->global_default(f);
			// By data, not by position. It was `block ? 1 : 0`, which read an
			// `ask` default as Allow and then wrote that back on save -- the
			// setting would have been quietly widened by opening the page and
			// pressing OK without touching anything.
			const int at = combo->findData(int(cur));
			combo->setCurrentIndex(at >= 0 ? at : 0);
			// Greyed on the desktop for the reason the shield gives: the
			// desktop already asks as a desktop, so this changes nothing there.
#ifndef Q_OS_ANDROID
			if (f == policy::feature::desktop_site) {
				combo->setEnabled(false);
				combo->setToolTip("Only meaningful on a phone. This build "
				                   "already asks as a desktop.");
			}
#endif
			combo->setObjectName(QString("feature_%1").arg(policy::feature_name(f)));
			m_feature_combos[int(f)] = combo;
			v->addWidget(settings_row(policy::feature_label(f),
			                           policy::feature_help(f), combo, page));
		}
		if (group_name == "Cookies and site data") {
			build_clear_section(v, page);
			clear_section_placed = true;
		}
	}
	if (!clear_section_placed)
		build_clear_section(v, page);

	// --- Search ------------------------------------------------------------
	//
	// **On the privacy page rather than a general one, because that is what it
	// is.** Everything else here decides what a site may do to you; this
	// decides who is told what you typed. Typing terms into the address bar
	// sends them somewhere, and the somewhere is the setting -- so it belongs
	// beside the permissions rather than beside the colour scheme.
	//
	// The control is the template itself rather than a menu of engines. The
	// stored value has always been a template, and a menu would either be a
	// second representation to keep in step with it or a list that quietly
	// refuses whatever is not on it. A self-hosted SearxNG is a first-class
	// answer here and would never have earned a menu entry.
	v->addWidget(section_heading("Search", page));
	m_search_engine = new QLineEdit(settings_store::search_engine(), page);
	m_search_engine->setObjectName("search_engine");
	m_search_engine->setPlaceholderText("https://duckduckgo.com/?q=%1");
	v->addWidget(settings_row(
	  "Search engine",
	  "Where the address bar sends what you type when it is not an address. "
	  "%1 stands for the terms, url-encoded. The default asks least of you: "
	  "no account, and no profile built from what you search for.",
	  m_search_engine, page, true));

	// The one thing that can be typed here and silently do nothing. `%1` is
	// where the terms go, so a template without it searches for the empty
	// string on every query -- a request that goes out and a results page that
	// comes back, with no sign anything is wrong. Said as it is typed rather
	// than on OK, because the mistake is invisible afterwards.
	m_search_note = dim_label(QString(), page);
	v->addWidget(m_search_note);
	auto say_if_broken = [this] {
		const bool ok = m_search_engine->text().contains("%1");
		m_search_note->setText(
		  ok ? QString()
		     : QString("No %1 in the template, so there is nowhere to put the "
		                "terms. The address bar will say so rather than search."));
	};
	connect(m_search_engine, &QLineEdit::textChanged, this, say_if_broken);
	say_if_broken();

	// --- Site exceptions ---------------------------------------------------
	//
	// The page above says an exception always wins over what is chosen here,
	// and until now there was nowhere to see one. The only way to find out that
	// JavaScript had been blocked on some site months ago was to go back to that
	// site and open the shield -- which is fine if you remember which site, and
	// useless otherwise. Every browser with per-site permissions grew this list
	// for the same reason.
	v->addWidget(section_heading("Site exceptions", page));
	m_exception_note = dim_label(QString(), page);
	v->addWidget(m_exception_note);

	m_exceptions = new QTreeWidget(page);
	m_exceptions->setObjectName("site_exceptions");
	m_exceptions->setHeaderLabels({ "Site", "What differs from the defaults" });
	m_exceptions->setRootIsDecorated(false);
	m_exceptions->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_exceptions->setMinimumHeight(120);
	v->addWidget(m_exceptions);

	m_exception_drop = new QPushButton("&Remove selected", page);
	m_exception_drop->setObjectName("drop_exception");
	m_exception_drop->setEnabled(false);
	m_exception_drop->setToolTip(
	  "The site falls back to the defaults above. Applied when you press OK.");
	connect(m_exceptions, &QTreeWidget::itemSelectionChanged, this, [this] {
		m_exception_drop->setEnabled(!m_exceptions->selectedItems().isEmpty());
	});
	connect(m_exception_drop, &QPushButton::clicked, this, [this] {
		for (QTreeWidgetItem *it : m_exceptions->selectedItems())
			m_dropped_patterns.insert(it->text(0));
		rebuild_exceptions();
	});
	auto *drop_row = new QHBoxLayout;
	drop_row->addStretch(1);
	drop_row->addWidget(m_exception_drop);
	v->addLayout(drop_row);
	rebuild_exceptions();

	// --- All of it in one file ----------------------------------------------
	v->addWidget(section_heading("Settings file", page));
	m_bundle_note = dim_label(
	  "Everything on these pages, plus the site exceptions above and the "
	  "accepted filter rules, in one INI file you can read, edit and carry to "
	  "another machine.\n\nIt does not include your tabs, which have their own "
	  "file, the Claude API key, which is never written to disk, or the "
	  "learned site rules — those are imported on the Filters page, where each "
	  "one is reviewed before it takes effect.", page);
	v->addWidget(m_bundle_note);

	auto *bundle_row = new flow_layout;
	auto *do_export = new QPushButton("E&xport all settings…", page);
	do_export->setObjectName("settings_export");
	auto *do_import = new QPushButton("Im&port settings…", page);
	do_import->setObjectName("settings_import");
	connect(do_export, &QPushButton::clicked, this, &settings_dialog::export_settings);
	connect(do_import, &QPushButton::clicked, this, &settings_dialog::import_settings);
	bundle_row->addWidget(do_export);
	bundle_row->addWidget(do_import);
	v->addLayout(bundle_row);

	// What used to be a footnote about cookie consent banners is now the
	// description on that row itself, where somebody reading that row will see
	// it. The one thing the row has no space for is the consequence, which is
	// worth keeping because it surprises people.
	auto *note = dim_label(
	  "Answering a consent banner allows that site's own cookies: the answer "
	  "is itself stored in a cookie, and would otherwise be forgotten on every "
	  "visit.", page);
	v->addSpacing(10);
	v->addWidget(note);

	v->addStretch(1);
}


void settings_dialog::update_restore_button() {
	if (!m_restore || !m_categories)
		return;
	const int page = m_categories->currentRow();
	// The text as it is. These are list items, which have no mnemonics, so the
	// ampersand in "Privacy & security" is an ampersand -- stripping it left
	// "Privacy  security" with two spaces in the button, which is what a
	// screenshot showed and no assertion would have.
	const QString name = page >= 0 ? m_categories->item(page)->text() : QString();

	// The filters page holds learned data rather than preferences: accepted
	// filter rules and consent wording this machine worked out. "Defaults" for
	// those would mean deleting them, which is not what a Restore Defaults
	// button means anywhere else, so the button says why it is off rather than
	// being mysteriously grey.
	const bool resettable = name != "Filters";
	m_restore->setEnabled(resettable);
	// Escaped for a *button*, where an ampersand is a mnemonic and would be
	// eaten -- "Privacy & security" came out as "Privacy &security" with an
	// underlined s. A list item needs the literal character and a button needs
	// it doubled, which is why the escaping belongs here rather than in the name.
	QString label = name;
	label.replace("&", "&&");
	m_restore->setText(resettable ? QString("Restore %1 defaults").arg(label)
	                               : QStringLiteral("Restore defaults"));
	m_restore->setToolTip(
	  resettable
	    ? QString("Put the %1 page back to a fresh install's settings. "
	               "Nothing is written until you press OK, so Cancel undoes "
	               "it.").arg(name)
	    : QStringLiteral("The filters page holds rules learned on this "
	                      "machine, not preferences — remove them "
	                      "individually rather than all at once."));
}

void settings_dialog::restore_page_defaults(int page) {
	if (!m_categories || page < 0 || page >= m_categories->count())
		return;
	const QString name = m_categories->item(page)->text();

	// "Default" means what a freshly built object holds before anything has
	// been stored -- the same values the program runs on the first time it is
	// started. Reading them from a new instance rather than from a second table
	// of constants is what keeps the two from drifting apart.
	if (name.startsWith("Privacy")) {
		policy_engine fresh;
		for (int i = 0; i < m_feature_combos.size(); ++i) {
			QComboBox *c = m_feature_combos[i];
			if (!c)
				continue;
			const policy::setting want =
			  fresh.global_default(static_cast<policy::feature>(i));
			const int at = c->findData(int(want));
			c->setCurrentIndex(at >= 0 ? at : 0);
		}
		// Site exceptions are deliberately left alone. They are decisions about
		// particular sites rather than defaults, they each have their own
		// Remove, and quietly discarding them behind a button labelled
		// "defaults" would be the kind of surprise this project keeps out.
	} else if (name.startsWith("Appearance")) {
		// Back to following the desktop, and applied at once, because that
		// is how this control behaves everywhere else on the page.
		if (m_appearance)
			m_appearance->setCurrentIndex(
			  m_appearance->findData(int(theme::choice::system)));
	} else if (name.startsWith("Media")) {
		player_launcher fresh;
		m_custom_cmd->clear();
		for (QRadioButton *b : m_player_buttons)
			if (b->property("player_id").toString() == fresh.selected()) {
				b->setChecked(true);
				break;
			}
	} else if (name.startsWith("Downloads")) {
		download_manager fresh;
		m_dir->setText(fresh.directory());
		if (m_torrents) {
			// The torrent knobs read their defaults from a fresh source, which
			// is only constructible when the build has libtorrent -- and when it
			// does not, those controls are disabled anyway.
			torrent_download_source fresh_t;
			m_conn_global->setValue(fresh_t.connection_limit_global());
			m_conn_torrent->setValue(fresh_t.connection_limit_per_torrent());
			m_seed_ratio->setValue(fresh_t.seed_ratio());
			m_interfaces->setText(fresh_t.listen_interfaces());
			m_sequential->setChecked(fresh_t.sequential());
		}
	} else if (name.startsWith("Kiosk")) {
		const kiosk_config d;   // the struct's own initialisers are the defaults
		m_kiosk_home->setText(d.home.toString());
		m_kiosk_w->setValue(d.design_size.isValid() ? d.design_size.width() : 0);
		m_kiosk_h->setValue(d.design_size.isValid() ? d.design_size.height() : 0);
		m_kiosk_scale->setCurrentIndex(m_kiosk_scale->findData(int(d.scale)));
		m_kiosk_fit->setCurrentIndex(m_kiosk_fit->findData(int(d.fit)));
		m_kiosk_cursor->setChecked(d.hide_cursor);
		m_kiosk_idle->setValue(d.idle_reset_seconds);
		m_kiosk_dog->setChecked(d.watchdog);
		m_kiosk_escape->setChecked(d.allow_escape);
		m_kiosk_clear->setChecked(d.clear_between_sessions);
	} else if (name.startsWith("AI")) {
		ollama_provider fresh_local;
		claude_provider fresh_remote;
		m_ollama_url->setText(fresh_local.endpoint().toString());
		m_ollama_model->setText(fresh_local.model());
		m_probe_timeout->setValue(fresh_local.probe_timeout());
		m_claude_model->setText(fresh_remote.model());
		m_ai_auto->setChecked(true);
		// The API key is not touched: it was never stored, so it has no default
		// to go back to, and clearing it would throw away something the user
		// typed this session for no gain.
	}

	update_custom_state();
	update_ai_state();
}

void settings_dialog::export_settings() {
	const QString path = QFileDialog::getSaveFileName(
	  this, "Export settings", QDir::homePath() + "/hydra-settings.ini",
	  "Settings files (*.ini)");
	if (path.isEmpty())
		return;

	// The controls first. Exporting before applying would write the settings as
	// they were when the window opened, which is the one thing nobody means by
	// "export what I have".
	apply();
	apply_kiosk();
	const settings_bundle::summary s =
	  settings_bundle::write(path, m_policy, m_filters);
	m_bundle_note->setText(
	  s.ok() ? QString("Wrote %1 to %2.").arg(s.describe(), path)
	         : QString("<b>%1</b>").arg(s.error.toHtmlEscaped()));
}

void settings_dialog::import_settings() {
	const QString path = QFileDialog::getOpenFileName(
	  this, "Import settings", QDir::homePath(), "Settings files (*.ini)");
	if (path.isEmpty())
		return;

	const settings_bundle::summary s =
	  settings_bundle::read(path, m_policy, m_filters);
	if (!s.ok()) {
		m_bundle_note->setText(QString("<b>%1</b>").arg(s.error.toHtmlEscaped()));
		return;
	}

	// The dialog is now showing what was here before the file was read, so the
	// controls are refilled from what the import actually applied. Without this
	// the window would still say "allow" over a policy that now says "block",
	// and pressing OK would write the stale answer back over the import.
	load();
	rebuild_exceptions();
	rebuild_filter_list();
	m_bundle_note->setText(QString("Read %1 from %2.").arg(s.describe(), path));
}

void settings_dialog::rebuild_exceptions() {
	if (!m_exceptions)
		return;
	m_exceptions->clear();
	if (!m_policy) {
		m_exception_note->setText("No policy engine was supplied.");
		return;
	}

	int shown = 0;
	for (const policy_engine::rule &r : m_policy->rules()) {
		if (m_dropped_patterns.contains(r.pattern))
			continue;
		// A rule that expresses nothing is not an exception. They exist because
		// clearing the last feature of a rule leaves the rule behind, and
		// listing it would offer the user something to remove that is not there.
		QStringList said;
		for (int i = 0; i < policy::feature_count(); ++i) {
			const auto f = static_cast<policy::feature>(i);
			const policy::setting st = policy::get_setting(r.bits, f);
			if (st == policy::setting::unset)
				continue;
			// `setting_word`, not a two-way ternary. The ternary called `ask`
			// "block", which is the same class of quiet lie as the combo above
			// and harder to notice, since this list is read rather than edited.
			said << QString("%1: %2").arg(
			  policy::feature_label(f),
			  QString::fromLatin1(policy::setting_word(st)));
		}
		if (said.isEmpty())
			continue;
		auto *it = new QTreeWidgetItem(m_exceptions);
		it->setText(0, r.pattern);
		it->setText(1, said.join(", "));
		++shown;
	}
	for (int i = 0; i < 2; ++i)
		m_exceptions->resizeColumnToContents(i);

	m_exception_note->setText(
	  shown == 0
	    ? QStringLiteral("No site has an exception yet. The shield in the "
	                      "toolbar makes one for the site you are on.")
	    : QString("%1 site%2 with settings of their own. Removing one sends "
	               "that site back to the defaults above.")
	          .arg(shown)
	          .arg(shown == 1 ? "" : "s"));
	if (!m_dropped_patterns.isEmpty())
		m_exception_note->setText(
		  m_exception_note->text() +
		  QString(" %1 marked for removal when you press OK.")
		      .arg(m_dropped_patterns.size()));
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
		bool wrote = true;
		if (m_filters->remove(sel.first()->text(0)) && !m_filters_path.isEmpty())
			wrote = m_filters->save(m_filters_path);
		rebuild_filter_list();
		// **Set after the rebuild, which writes this label itself.** And worth
		// saying at all because of the immediacy above: the rule is off now,
		// so without a word the list looks correct and quietly disagrees with
		// the disk until the next launch puts the rule back.
		if (!wrote)
			m_filter_note->setText(
			    "Removed for this session only — the filter list could not be "
			    "saved, so this rule returns next launch.");
	});

	rebuild_filter_list();

	// The second corpus, on the same page because it is the same kind of thing:
	// small perishable facts about how sites behave, kept as data so they can be
	// corrected without a release -- and, in the stated direction, exchanged.
	// A heading and a dimmed line rather than a titled box: the page above it
	// is already a heading and a table, and one bordered frame among them looks
	// like the frame means something.
	auto *rules_box = new QWidget(page);
	auto *rv = new QVBoxLayout(rules_box);
	rv->setContentsMargins(0, 0, 0, 0);
	rv->addWidget(section_heading("Learned site rules", rules_box));
	rv->addWidget(dim_label(
	  "Consent-banner wording and ad-blocker detector names learned on this "
	  "machine. Built-in rules are not listed: they come from the program and "
	  "cannot be edited here.", rules_box));

	m_rules_view = new QTreeWidget(rules_box);
	m_rules_view->setObjectName("site_rules");
	m_rules_view->setColumnCount(5);
	m_rules_view->setHeaderLabels(
	  { "Rule", "Kind", "Applies to", "Status", "From" });
	m_rules_view->setRootIsDecorated(false);
	rv->addWidget(m_rules_view, 1);

	// Five buttons that wrap rather than squeeze. A QHBoxLayout's minimum is
	// the sum of its children, so at any width below that it shrinks all of
	// them past their labels -- the failure `flow_layout` was written for.
	auto *rrow = new flow_layout;
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
		bool wrote = true;
		if (!m_rules_path.isEmpty())
			wrote = kept.save(m_rules_path);
		rebuild_site_rules();
		m_rules_note->setText(
		  !wrote ? QString("Forgot them for this session only — the rules could "
		                    "not be saved, so they return next launch.")
		  : gone == 0
		  ? QString("Nothing had been imported.")
		  : QString("Forgot %1 imported rule%2. What you learned here is "
		             "untouched.").arg(gone).arg(gone == 1 ? "" : "s"));
	});

	// Sharing, in the only form it takes for now: a file someone sends. The
	// transport is deliberately undecided -- what a received rule has to prove is
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
		bool wrote = true;
		if (!m_rules_path.isEmpty())
			wrote = merged.save(m_rules_path);
		rebuild_site_rules();
		if (!wrote)
			m_rules_note->setText(
			    "Imported for this session only — the rules could not be "
			    "saved, so they are gone next launch.");
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
		bool wrote = true;
		if (!m_rules_path.isEmpty())
			wrote = kept.save(m_rules_path);
		rebuild_site_rules();
		if (!wrote)
			m_rules_note->setText(
			    "Removed for this session only — the rules could not be saved, "
			    "so this one returns next launch.");
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
		// applied on the tracker's own domain. Network rules are global here --
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

	v->addWidget(section_heading("What it shows", page));

	m_kiosk_home = new QLineEdit(page);
	m_kiosk_home->setPlaceholderText("blank = whatever tab you were on");
	v->addWidget(settings_row(
	  "Home page",
	  "Where kiosk mode starts, and where it returns to when left alone.",
	  m_kiosk_home, page, /*wide=*/true));

	m_kiosk_idle = new QSpinBox(page);
	m_kiosk_idle->setRange(0, 86400);
	m_kiosk_idle->setSuffix(" s");
	m_kiosk_idle->setSpecialValueText("never");
	v->addWidget(settings_row(
	  "Return home when idle",
	  "How long an abandoned session sits before it walks back to the home "
	  "page. Never means never.",
	  m_kiosk_idle, page));

	v->addWidget(section_heading("How it is scaled", page));

	m_kiosk_scale = new QComboBox(page);
	m_kiosk_scale->addItem("Reflow — one zoom factor, the page re-lays out",
	                        int(scale_mode::reflow));
	m_kiosk_scale->addItem("None — native size, cropped by the stage",
	                        int(scale_mode::none));
	m_kiosk_scale->addItem("Geometric — exact transform (test on the target GPU)",
	                        int(scale_mode::geometric));
	m_kiosk_scale->setObjectName("kiosk_scale");
	// **A combo's minimum width is its longest item**, and the longest here is
	// "Geometric -- exact transform (test on the target GPU)". That one line
	// made this page 439 pixels wide at minimum, which is why the settings
	// window would not fit a phone and why its text came out clipped -- the
	// page overflowed the scroll area it sits in and had to be scrolled
	// sideways to read.
	//
	// Costs the desktop nothing: this is a `wide` row, so the control is given
	// a share of the width by the layout's stretch rather than by its own size
	// hint. Lowering the hint changes what it will *accept*, not what it gets.
	// Twelve characters is enough to show that something is selected; the
	// popup still lists every option in full, which is where they are read.
	m_kiosk_scale->setSizeAdjustPolicy(
	  QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_kiosk_scale->setMinimumContentsLength(12);
	v->addWidget(settings_row(
	  "Scaling",
	  "Reflow re-lays the page out at a zoom factor and is the robust choice. "
	  "Geometric transforms the pixels exactly, and is the one that has "
	  "historically rendered black on some GPUs — try it on the hardware you "
	  "will deploy on.",
	  m_kiosk_scale, page, /*wide=*/true));

	m_kiosk_fit = new QComboBox(page);
	m_kiosk_fit->addItem("Contain", int(fit_mode::contain));
	m_kiosk_fit->addItem("Cover", int(fit_mode::cover));
	m_kiosk_fit->addItem("Stretch", int(fit_mode::stretch));
	m_kiosk_fit->addItem("Actual size", int(fit_mode::actual));
	m_kiosk_fit->setObjectName("kiosk_fit");
	v->addWidget(settings_row(
	  "Fit",
	  "How the design size meets the screen — the same words CSS uses. Not "
	  "every pair means something: reflow cannot stretch, because one zoom "
	  "factor cannot scale the axes independently, so it approximates with "
	  "cover rather than pretending.",
	  m_kiosk_fit, page));

	m_kiosk_w = new QSpinBox(page);
	m_kiosk_w->setRange(0, 16384);
	m_kiosk_w->setSpecialValueText("screen");
	m_kiosk_h = new QSpinBox(page);
	m_kiosk_h->setRange(0, 16384);
	m_kiosk_h->setSpecialValueText("screen");
	auto *size_pair = new QWidget(page);
	auto *sh = new QHBoxLayout(size_pair);
	sh->setContentsMargins(0, 0, 0, 0);
	sh->addWidget(m_kiosk_w);
	sh->addWidget(new QLabel("×", size_pair));
	sh->addWidget(m_kiosk_h);
	v->addWidget(settings_row(
	  "Design size",
	  "The size the page was built for. Leave both on \"screen\" to use "
	  "whatever display it ends up on.",
	  size_pair, page));

	v->addWidget(section_heading("Running unattended", page));

	m_kiosk_cursor = new QCheckBox(page);
	v->addWidget(settings_row("Hide the mouse pointer",
	                           "There is usually no mouse on a kiosk, and a "
	                           "stationary cursor on a display looks like a "
	                           "fault.", m_kiosk_cursor, page));
	m_kiosk_dog = new QCheckBox(page);
	v->addWidget(settings_row("Reload if the page's process dies",
	                           "A renderer crash leaves a blank screen that "
	                           "nobody is there to notice.", m_kiosk_dog, page));
	m_kiosk_escape = new QCheckBox(page);
	m_kiosk_escape->setObjectName("kiosk_escape");
	v->addWidget(settings_row(
	  "Esc leaves kiosk mode",
	  "Turning this off locks the screen down: Esc and F11 will not leave, and "
	  "on a machine with no keyboard shortcut left there may be no way out "
	  "except ending the process. It is here because unattended displays need "
	  "it — not because it is a normal thing to switch on.",
	  m_kiosk_escape, page));

	m_kiosk_clear = new QCheckBox(page);
	m_kiosk_clear->setObjectName("kiosk_clear");
	v->addWidget(settings_row(
	  "Forget the session between visitors",
	  "Kiosk shows a tab from the ordinary browser, in the ordinary profile, "
	  "so without this the last person's cookies and logins are still there "
	  "for the next one — returning to the home page resets the navigation "
	  "and nothing else. Switching it on clears cookies, cached files and "
	  "visited links when kiosk starts, each time it returns home after "
	  "being left alone, and when it is left. Those are Hydra's own cookies, "
	  "not a copy: your logins go too, which is why it is off unless you ask "
	  "for it. It does not apply when a page asks for fullscreen.",
	  m_kiosk_clear, page));

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

	v->addWidget(section_heading("External player", page));

	// The radio list stays a list: this is one choice among several, and a
	// column of names with their availability beside them is the clearest way
	// to show which ones exist on this machine.
	auto *group = new QWidget(page);
	m_player_group_layout = new QVBoxLayout(group);
	m_player_group_layout->setContentsMargins(0, 0, 0, 0);
	populate_players();
	v->addWidget(group);

	auto *rescan = new QPushButton("&Rescan for players", page);
	rescan->setObjectName("rescan_players");
	rescan->setToolTip("Look again at PATH — use this after installing one");
	connect(rescan, &QPushButton::clicked, this, &settings_dialog::rescan_players);
	v->addWidget(settings_row("Rescan",
	                           "Look at PATH again, after installing one.",
	                           rescan, page));

	v->addWidget(section_heading("Custom command", page));
	m_custom_cmd = new QLineEdit(page);
	m_custom_cmd->setPlaceholderText("mpv --fullscreen %U");
	connect(m_custom_cmd, &QLineEdit::textChanged, this,
	         &settings_dialog::update_custom_state);
	v->addWidget(settings_row(
	  "Command",
	  "%U is replaced by the stream URL; leave it out and the URL is "
	  "appended. Arguments are split on spaces — this is not a shell, so "
	  "quoting and pipes will not work.",
	  m_custom_cmd, page, /*wide=*/true));

	m_player_note = new QLabel(page);
	m_player_note->setWordWrap(true);
	v->addWidget(m_player_note);
	v->addStretch(1);
}

void settings_dialog::build_download_page(QWidget *page) {
	auto *v = new QVBoxLayout(page);

	v->addWidget(section_heading("Location", page));
	auto *where = new QWidget(page);
	auto *wh    = new QHBoxLayout(where);
	wh->setContentsMargins(0, 0, 0, 0);
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
	v->addWidget(settings_row("Save files to",
	                           "Where a download lands unless something says "
	                           "otherwise.", where, page, /*wide=*/true));

	// The whole BitTorrent section can be unavailable, so it is built into a
	// holder that can be disabled in one move -- a heading and six rows greyed
	// individually would look like six separate accidents.
	auto *bt = new QWidget(page);
	auto *bv = new QVBoxLayout(bt);
	bv->setContentsMargins(0, 0, 0, 0);

	const bool have_torrents = torrent_download_source::available() && m_torrents;
	bv->addWidget(section_heading(have_torrents
	                                  ? QStringLiteral("BitTorrent")
	                                  : QStringLiteral("BitTorrent — unavailable "
	                                                    "in this build"),
	                              bt));

	m_conn_global = new QSpinBox(bt);
	m_conn_global->setRange(10, 20000);
	m_conn_global->setSingleStep(50);
	m_conn_torrent = new QSpinBox(bt);
	m_conn_torrent->setRange(5, 5000);
	m_conn_torrent->setSingleStep(10);

	// The sec 11.4 argument, split across the two rows it actually applies to,
	// rather than as a paragraph underneath both.
	bv->addWidget(settings_row(
	  "Connections, all torrents",
	  "Swarm speed comes from holding many mostly-slow peers rather than a few "
	  "fast ones, so this is the number that matters. The default is already "
	  "well above a typical desktop client's.",
	  m_conn_global, bt));
	bv->addWidget(settings_row(
	  "Connections, per torrent",
	  "Raise it further if your connection and file-descriptor limit allow.",
	  m_conn_torrent, bt));

	m_seed_ratio = new QDoubleSpinBox(bt);
	m_seed_ratio->setRange(0.0, 100.0);
	m_seed_ratio->setSingleStep(0.1);
	m_seed_ratio->setDecimals(2);
	m_seed_ratio->setSpecialValueText("Do not seed");
	bv->addWidget(settings_row(
	  "Seed until ratio",
	  "How much to give back before a finished torrent stops uploading.",
	  m_seed_ratio, bt));

	m_sequential = new QCheckBox(bt);
	bv->addWidget(settings_row(
	  "Download in order by default",
	  "Slightly slower overall, and the file is playable from the start. Watch "
	  "turns this on for one torrent regardless.",
	  m_sequential, bt));

	m_interfaces = new QLineEdit(bt);
	m_interfaces->setPlaceholderText("0.0.0.0:6881  (blank = any interface)");
	// The 260-pixel minimum that used to be here was reaching for the right
	// thing by the wrong instrument. It existed so the placeholder above stays
	// readable -- but a *minimum* says "never narrower than this", which held
	// the whole page at 367 and is a promise no phone screen can keep. This is
	// a `wide` row, so the layout's stretch already gives the field a share of
	// the width wherever there is width to give; the minimum only ever decided
	// what happened when there was not.

	// Not a VPN, and says so. sec 11.4 decided Hydra ships no tunnel; this is the
	// field that makes the user's own system-level choice reliable instead of
	// competing with it.
	bv->addWidget(settings_row(
	  "Listen on",
	  "Hydra does not tunnel torrent traffic. If you run a VPN at the system "
	  "level, naming its interface here — tun0:6881, say — keeps torrent "
	  "traffic on it, and announces stop if that interface goes away.",
	  m_interfaces, bt, /*wide=*/true));

	if (!have_torrents) {
		// The *controls* are disabled, not the whole section.
		//
		// Disabling the container greys the explanations too, and an explanation
		// is exactly what someone wants to read when a feature is unavailable --
		// "what would this have done, and what do I need to get it?". Chrome
		// makes the same distinction. It also avoids a palette fight: a label
		// with an explicit foreground colour does not reliably follow its
		// parent into the disabled colour group, which showed up here as grey
		// titles above full-contrast descriptions.
		for (QWidget *w : QList<QWidget *>{ m_conn_global, m_conn_torrent,
			                                   m_seed_ratio, m_sequential,
			                                   m_interfaces }) {
			w->setEnabled(false);
			w->setToolTip("Built without libtorrent-rasterbar");
		}
	}
	v->addWidget(bt);
	v->addStretch(1);
}

// sec 11.3's radio group: everything supported is shown, installed ones are
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
	// the user asked -- not because a window opened.
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

	v->addWidget(section_heading("Which backend", page));

	// A radio group stays a radio group: three mutually exclusive answers to
	// one question, which is exactly what radios are for.
	auto *group = new QWidget(page);
	auto *gv    = new QVBoxLayout(group);
	gv->setContentsMargins(0, 0, 0, 0);

	// **A short label with the explanation under it, which is what every other
	// setting on these pages does.** These three carried their qualifier in the
	// label -- "Automatic -- use the local model when it is running" -- and a
	// QRadioButton does not wrap, so that one line held this page at 309 pixels
	// and would not give any of it back.
	//
	// The explanations were tooltips, and a tooltip needs a pointer to hover.
	// On the Android build there is none, so the reasoning behind the one
	// setting on these pages that decides whether anything leaves the machine
	// was reachable on a desktop and invisible on a phone. Putting it under the
	// button costs three lines and is legible everywhere.
	m_ai_auto     = new QRadioButton("Automatic", group);
	m_ai_local    = new QRadioButton("Local only", group);
	m_ai_external = new QRadioButton("Claude", group);

	// The point of the middle one: sec 1's "data stays on the machine" should be
	// something you can hold the app to, not a default that silently lapses the
	// first time Ollama is not running.
	const char *why[] = {
		"Uses the local model when it is running, and falls back to Claude "
		"only when none answers.",
		"Never uses an external service. If the local model is not running "
		"the AI features are simply unavailable, rather than quietly "
		"switching.",
		"An external service. Everything sent is reviewed first, on every "
		"feature that sends anything.",
	};
	int which = 0;
	for (QRadioButton *b : { m_ai_auto, m_ai_local, m_ai_external }) {
		gv->addWidget(b);
		QLabel *note = dim_label(why[which++], group);
		// Indented under its button rather than against the margin, so it reads
		// as belonging to the one above it and not to the one below.
		note->setContentsMargins(22, 0, 0, 6);
		gv->addWidget(note);
		connect(b, &QRadioButton::toggled, this, &settings_dialog::update_ai_state);
	}
	v->addWidget(group);
	// The paragraph that used to sit here said what "Local only" means, which
	// is now written under the button it is about. Two statements of one fact,
	// four lines apart, read as two facts and send the reader back to compare
	// them.

	v->addWidget(section_heading("Local model (Ollama)", page));
	m_ollama_url = new QLineEdit(page);
	m_ollama_url->setPlaceholderText("http://localhost:11434");
	v->addWidget(settings_row("Endpoint", "Where Ollama is listening.",
	                           m_ollama_url, page, /*wide=*/true));
	m_ollama_model = new QLineEdit(page);
	m_ollama_model->setPlaceholderText("llama3");
	v->addWidget(settings_row("Model", "Which model to ask for.",
	                           m_ollama_model, page, /*wide=*/true));

	m_probe_timeout = new QSpinBox(page);
	m_probe_timeout->setRange(100, 15000);
	m_probe_timeout->setSingleStep(250);
	m_probe_timeout->setSuffix(" ms");
	// Why anyone would touch this: the cost is only paid when the endpoint
	// stops being local, and then it is paid every time.
	v->addWidget(settings_row(
	  "Reachability timeout",
	  "Rechecked whenever a backend is needed, since Ollama is started and "
	  "stopped like any service. Loopback answers in about a millisecond; a "
	  "remote host that drops packets never answers at all and costs this "
	  "whole timeout each time — so lower it if the endpoint is not on this "
	  "machine.",
	  m_probe_timeout, page));

	m_check_local = new QPushButton("&Check now", page);
	m_check_local->setObjectName("check_local");
	connect(m_check_local, &QPushButton::clicked, this,
	         &settings_dialog::check_local_model);
	v->addWidget(settings_row("Check the local model",
	                           "Ask the endpoint above whether it is there.",
	                           m_check_local, page));

	v->addWidget(section_heading("Claude", page));
	m_claude_model = new QLineEdit(page);
	m_claude_model->setPlaceholderText("claude-opus-5");
	v->addWidget(settings_row("Model", "Which Claude model to send to.",
	                           m_claude_model, page, /*wide=*/true));

	m_claude_key = new QLineEdit(page);
	m_claude_key->setEchoMode(QLineEdit::Password);
	m_claude_key->setPlaceholderText("from ANTHROPIC_API_KEY");
	// Not persisted, and it says so rather than letting someone find out. The
	// settings file is plain INI and an API key does not belong in one; the
	// same reasoning that keeps it out of the tree and policy files applies
	// here, and writing it anywhere readable would be security theatre.
	v->addWidget(settings_row(
	  "API key",
	  "Not saved — kept in memory for this session only. Set "
	  "ANTHROPIC_API_KEY in your environment for it to persist; this settings "
	  "file is plain text and a credential does not belong in one.",
	  m_claude_key, page, /*wide=*/true));

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
	// honest answer is "not checked", not "unavailable" -- the latter would be
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
	m_kiosk_clear->setChecked(c.clear_between_sessions);
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
	c.clear_between_sessions = m_kiosk_clear->isChecked();
	settings_store::set_kiosk(c);
}

void settings_dialog::reject() {
	// The colour scheme was applied while the user was choosing it, so Cancel
	// has to undo that too -- otherwise "Cancel" leaves the window in a theme
	// nobody agreed to keep.
	theme::apply(settings_store::appearance());
	QDialog::reject();
}

// One place decides which of the two is on screen, called on every resize.
//
// A dialog is not resized often, so this is cheap; what it must not do is
// thrash. `update_layout_mode` returns immediately when the mode is unchanged,
// which matters because Android hands the dialog a geometry on every show.
void settings_dialog::resizeEvent(QResizeEvent *event) {
	QDialog::resizeEvent(event);
	update_layout_mode();
}

void settings_dialog::update_layout_mode() {
	if (!m_categories || !m_category_pick)
		return;
	const bool narrow = width() < k_narrow_threshold;
	if (narrow == m_category_pick->isVisible())
		return;
	m_category_pick->setVisible(narrow);
	m_categories->setVisible(!narrow);
}

void settings_dialog::accept() {
	apply();
	apply_kiosk();
	QDialog::accept();
}

void settings_dialog::apply() {
	if (m_appearance)
		settings_store::set_appearance(
		  static_cast<theme::choice>(m_appearance->currentData().toInt()));
	// **Stored even when it has no %1**, deliberately. Refusing to save it
	// would leave the dialog showing one thing and the browser using another,
	// which is worse than a setting that is wrong in the way its own note
	// says it is wrong. The address bar reports it at the moment it matters
	// and searches nothing rather than searching for nothing.
	if (m_search_engine) {
		const QString tmpl = m_search_engine->text().trimmed();
		if (!tmpl.isEmpty())
			settings_store::set_search_engine(tmpl);
	}
	if (m_policy) {
		// Exceptions the user removed. Every feature is set back to unset, which
		// is what "falls through to the defaults" means in the policy model --
		// there is no delete, and clearing is the same thing said properly.
		for (const QString &pattern : m_dropped_patterns)
			for (int i = 0; i < policy::feature_count(); ++i)
				m_policy->set_setting(pattern, static_cast<policy::feature>(i),
				                       policy::setting::unset);
		m_dropped_patterns.clear();
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
		// The command first: selecting Custom... is only meaningful once the
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
