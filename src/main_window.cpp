// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "tab_tree_model.h"
#include "tree_sort_proxy.h"
#include "state_store.h"
#include "policy_engine.h"
#include "site_policy_dialog.h"
#include "web_view_backend.h"
#include "web_view_factory.h"
#include "scheme_rules.h"
#include "kiosk_controller.h"
#include "reorganize_dialog.h"
#include "ollama_provider.h"
#include "claude_provider.h"
#include "media_detector.h"
#include "media_dialog.h"
#include "player_launcher.h"
#include "download_manager.h"
#include "http_download_source.h"
#include "torrent_download_source.h"
#include "downloads_dialog.h"
#include "settings_dialog.h"
#include "ytdlp_resolver.h"
#include "mse_tap.h"
#include "capture_source.h"
#include "extractor_signals.h"
#include "extractor_dialog.h"
#include "extractor_helpers.h"
#include "network_fetcher.h"
#include "ai_provider.h"
#include "local_proxy.h"
#include "filter_signals.h"
#include "filter_list.h"
#include "cosmetic_filters.h"
#ifdef Q_OS_ANDROID
#include "android_downloads.h"
#endif
#include "filter_dialog.h"
#include "credential_store.h"
#include "keepass_bridge.h"
#include "autofill_controller.h"
#include "consent_blocker.h"
#include "consent_dialog.h"
#include "antiadblock_watch.h"
#include "autofill_script.h"
#include "element_picker.h"
#include "picker_script.h"
#include "policy.h"
#include "node.h"

#include <QApplication>
#include <QSplitter>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QTreeView>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolBar>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QStyle>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QActionGroup>
#include <QAction>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QLocale>
#include <QStandardPaths>
#include <QDataStream>
#include <QCloseEvent>
#include <QSet>

#include <memory>

namespace {

// Schemes a browser is expected to render itself. Anything outside this set is
// not a page, which makes it a candidate for handing to something that can
// actually deal with it. Kept as a property of the web rather than of any
// engine, so the same rule holds behind the Android backend (§19.2).
}  // namespace

main_window::main_window(web_view_factory *factory, policy_engine *policy,
                          request_filter *filter, QWidget *parent)
	: QWidget(parent), m_factory(factory), m_policy(policy) {
	// The two interceptor consumers (architecture doc §10): both observe the
	// same request stream the blocker already rides, rather than adding a
	// second sensor.
	m_media      = new media_detector(this);
	m_signals    = new filter_signals(this);
	m_ex_signals = new extractor_signals(this);
	// Constructed before it is registered, which is not a style point: an
	// observer added while still null was harmless for exactly as long as the
	// function it landed in touched no members, and became a segfault in every
	// live driver the moment one did.
	m_antiadblock = new antiadblock_watch(this);
	m_filter = filter;
	if (filter) {
		filter->add_observer(m_media);
		filter->add_observer(m_signals);
		filter->add_observer(m_ex_signals);
		filter->add_observer(m_antiadblock);
	}
	m_extractors.load(QDir(QStandardPaths::writableLocation(
	                            QStandardPaths::AppDataLocation))
	                       .filePath("extractors.json"));
	// The §11.6 tap: what a page is actually feeding its <video>, for the sites
	// where watching request URLs finds nothing.
	m_mse = new mse_tap(this);
	connect(m_mse, &mse_tap::site_updated, this, [this](const QString &host) {
		refresh_media_affordance(host);
	});

	m_keepass  = new keepass_bridge(this);
	m_autofill = new autofill_controller(m_keepass, m_policy, this);
	// More than one login for a site is a question only the user can answer, and
	// it is asked here rather than in the page: the controller holds the
	// passwords until the answer comes back, so the picker is a list of names
	// and the page learns nothing until a choice is made.
	connect(m_autofill, &autofill_controller::choice_needed, this,
	        [this](const QStringList &labels) {
		bool ok = false;
		const QString pick = QInputDialog::getItem(
		    this, "Which login?",
		    "More than one login is stored for this site:", labels, 0,
		    /*editable=*/false, &ok);
		// A dismissed dialog fills nothing, which is a legitimate answer and
		// not an error worth a message.
		m_autofill->choose(ok ? labels.indexOf(pick) : -1);
	});
	// The refusal is the key icon's job (§13.2) and there is no key icon yet, so
	// the status bar carries it meanwhile. Silence here is the thing to avoid:
	// "nothing stored for this site" and "KeePassXC is not running" produce the
	// same empty form, and only one of them is worth doing something about.
	// The key's four states. Each is a different thing to do next, which is why
	// they read differently rather than all being "autofill unavailable".
	connect(m_autofill, &autofill_controller::requested, this, [this] {
		if (!m_key_action)
			return;
		m_key_action->setVisible(true);
		m_key_action->setText("Key");
		m_key_action->setToolTip("This page has a login form. Click to fill it "
		                          "from KeePassXC.");
	});
	connect(m_autofill, &autofill_controller::refused, this,
	        [this](const QString &why) {
		// On the key *and* in the status bar. The tooltip is where it stays
		// readable -- a status message is gone in six seconds and the form is
		// still sitting there empty.
		if (m_key_action) {
			m_key_action->setVisible(true);
			m_key_action->setText("Key ✕");
			m_key_action->setToolTip(why);
		}
		m_status->showMessage(why, 6000);
	});
	// Offering to save, which is the one path that *writes* to the vault. Asked
	// with a plain question naming the login and the site and never showing the
	// password -- a prompt that displays it is a prompt that shoulder-surfs --
	// and never remembered as a preference: "never for this site" is a policy
	// decision and belongs in the shield beside the other per-site settings,
	// not in a checkbox on a transient dialog.
	connect(m_autofill, &autofill_controller::save_offered, this,
	        [this](const QString &login, const QString &host) {
		const QString who = login.isEmpty() ? QStringLiteral("this login")
		                                     : login;
		const bool yes = QMessageBox::question(
		    this, "Save to KeePassXC?",
		    QString("Save the password for %1 at %2?").arg(who, host)) ==
		    QMessageBox::Yes;
		m_autofill->confirm_save(yes);
	});
	connect(m_autofill, &autofill_controller::save_finished, this,
	        [this](bool, const QString &message) {
		m_status->showMessage(message, 6000);
	});

	connect(m_autofill, &autofill_controller::credentials_ready, this,
	        [this](const QString &) {
		if (!m_key_action)
			return;
		m_key_action->setVisible(true);
		m_key_action->setText("Key ✓");
		m_key_action->setToolTip("Filled from KeePassXC. Click to fill again.");
	});
	m_consent  = new consent_blocker(m_policy, this);
	// Say it once, and say what the lever is. A page that will not run because
	// we block ads is indistinguishable from a broken site unless we tell them,
	// and only we know it was us.
	// A page that refuses to run because we block ads is fixed rather than
	// merely reported, and said out loud. The alternative -- telling the user
	// the shield exists and leaving them to it -- was tried first and is worse:
	// the message is easy to miss and the page stays broken, which reads as a
	// broken browser.
	//
	// Three things keep this from being presumptuous. It never overrides a
	// choice the user made for that site; it writes an ordinary per-site rule
	// the shield shows and can revert; and it happens once per site per session,
	// so a page that keeps asking cannot put the browser in a reload loop.
	connect(m_antiadblock, &antiadblock_watch::detected, this,
	         [this](const QString &host, const QString &what) {
		if (host.isEmpty())
			return;
		// An explicit per-site rule is the user speaking. Someone who blocked
		// ads on this site *knowing* it might break is not to be second-guessed
		// by the thing that noticed it broke.
		if (m_policy->setting_for(host, policy::feature::ads) !=
		        policy::setting::unset) {
			m_status->showMessage(
			    QString("%1 is checking for an ad blocker (%2). Your own setting "
			             "for this site is left as it is.").arg(host, what), 9000);
			return;
		}
		if (m_antiadblock_fixed.contains(host))
			return;
		m_antiadblock_fixed.insert(host);

		m_policy->set_setting(host, policy::feature::ads, policy::setting::allow);
		m_status->showMessage(
		    QString("%1 would not run with ads blocked (%2), so ads are now "
		             "allowed for this site and it is being reloaded — undo it "
		             "in the shield.").arg(host, what), 15000);

		// The allowance only affects requests from here on, and the page has
		// already been assembled without them, so it has to be loaded again or
		// the fix is invisible. Once per site, by the guard above.
		if (web_view_backend *v = current_view()) {
			if (v->url().host() == host) {
				apply_policy(v, host);
				v->reload();
			}
		}
	});
	// Non-empty from the start: a view can be built before any tree is loaded,
	// and an empty rule set is a blocker that silently does nothing.

	connect(m_consent, &consent_blocker::acted, this,
	         [this](const QString &host, const QString &choice) {
		m_status->showMessage(
		    QString("Answered the cookie banner on %1 (%2)").arg(host, choice), 6000);
	});
	// A policy change nobody asked for has to be visible when it happens, not
	// merely findable in the shield afterwards.
	connect(m_consent, &consent_blocker::relaxed_cookies, this,
	         [this](const QString &host) {
		m_status->showMessage(
		    QString("Allowed first-party cookies for %1 so its consent choice "
		             "sticks — change it in the shield").arg(host), 9000);
	});
	m_picker   = new element_picker(this);
	connect(m_picker, &element_picker::picked, this,
	         [this](const picked_element &) { open_filter_evolution(); });
	connect(m_picker, &element_picker::aborted, this, [this] {
		m_status->showMessage("Element pick cancelled.", 3000);
	});
	connect(m_keepass, &keepass_bridge::associated_changed, this,
	         [this](bool ok, const QString &msg) {
		m_status->showMessage((ok ? "KeePassXC: " : "KeePassXC: ") + msg, 6000);
	});
	connect(m_keepass, &keepass_bridge::error, this, [this](const QString &msg) {
		m_status->showMessage("KeePassXC: " + msg, 8000);
	});

	m_players   = new player_launcher;
	m_downloads = new download_manager(this);
#ifdef Q_OS_ANDROID
	// A download nobody can find afterwards has not really been downloaded: Qt's
	// download location on Android is app-private and invisible to every file
	// manager, so a finished file is copied into the shared Downloads collection.
	{
		auto *publisher = new android_downloads(m_downloads, this);
		connect(publisher, &android_downloads::published, this,
		         [this](const QString &, const QString &) {
			// Said in our own status bar rather than a system toast, because the
			// browser knows when it is showing something else that matters more.
			m_status->showMessage("Saved to Downloads.", 6000);
		});
		connect(publisher, &android_downloads::failed, this,
		         [this](const QString &path, const QString &why) {
			// Named, because the file is still there and still openable: the
			// copy failed, not the download.
			m_status->showMessage(QString("Downloaded, but not copied to "
			                               "Downloads (%1). It is at %2")
			                          .arg(why, path),
			                       10000);
		});
	}
#endif
	// The manager has no transport of its own (§11.4); give it one. Sources are
	// tried in order, so adding a torrent source later is one line here.
	m_downloads->add_source(new http_download_source);
	// BitTorrent is a first-class source, not a side feature (§11.4). It is
	// only present when the build found libtorrent; there is no degraded mode.
	// A recording is a download once it exists; it is only started differently.
	m_capture_src = new capture_source;
	m_downloads->add_source(m_capture_src);
	connect(m_capture_src, &capture_source::stop_requested, this,
	         [this](int) { if (!m_capture_url.isEmpty()) toggle_capture(); });

	if (torrent_download_source::available()) {
		m_torrents = new torrent_download_source;
		m_downloads->add_source(m_torrents);
	}
	// The §11.4 obligation that replaces the VPN we deliberately do not ship:
	// a source whose participation is publicly observable cannot start until
	// the user has been told what it does. The manager holds the job; this is
	// where the telling happens.
	// Queued, not direct. consent_required is emitted from inside
	// download_manager::enqueue(), and this handler opens a modal dialog — so a
	// direct connection would run a nested event loop inside enqueue() and not
	// return until the user answered. Every caller would then have to be safe
	// against that, including the URL-scheme handler that turns an in-page
	// magnet link into a download, where a nested loop inside an engine
	// callback is a genuinely bad place to be. Deferring keeps enqueue() a
	// function that returns.
	connect(m_downloads, &download_manager::consent_required, this,
	         &main_window::confirm_public_download, Qt::QueuedConnection);

	// Links that are not pages (§11.4). The rule is deliberately general rather
	// than a test for one scheme: anything the browser will not render, but
	// which some download source will take, becomes a download. A magnet link
	// is simply the first thing that fits, and the shell still never names a
	// transport.
	if (factory) {
		factory->set_external_url_handler([this](const QUrl &url) {
			if (renders_as_page(url))
				return;
			if (!m_downloads->source_for(url)) {
				m_status->showMessage(
					QString("Nothing here can open %1").arg(url.scheme() + ":"),
					6000);
				return;
			}
			start_download(url);
		});
	}
	// The local proxy is optional: if it cannot listen, Watch still works and
	// simply hands over the raw URL (§10 — it is an upgrade tier, not a
	// prerequisite).
	m_local_proxy = new local_proxy(this);
	m_local_proxy->start();
	m_filters   = new filter_list;
	// The cosmetic half of §12. It reads the same list the interceptor does, and
	// reaches pages the only way a `##` rule can: through the DOM.
	m_cosmetic  = new cosmetic_filters(m_filters, this);

	// Saved settings are applied here rather than by the settings dialog, so
	// they take effect on a run where that dialog is never opened — otherwise
	// "settings persist" quietly means "settings persist if you go and look".
	settings_store::load_into(m_players, m_downloads, m_torrents, nullptr, nullptr);
	connect(m_media, &media_detector::site_updated,
	         this, &main_window::on_media_found, Qt::QueuedConnection);
	// The security spine is wired up before we get here: the factory owns the
	// profile with the interceptor and cookie filter already installed on it
	// (architecture doc §6/§7.3), and the policy engine is shared with them.

	m_model = new tab_tree_model(this);
	m_proxy = new tree_sort_proxy(this);
	m_proxy->setSourceModel(m_model);

	m_save_timer = new QTimer(this);
	m_save_timer->setSingleShot(true);
	m_save_timer->setInterval(1500);   // debounce structural saves
	connect(m_save_timer, &QTimer::timeout, this, &main_window::flush_tree);

	// --- Layout: menu bar, toolbar, splitter, status bar -----------------
	// No QMainWindow, so each piece of window furniture is an ordinary child
	// widget in the layout (see the note on the class declaration).
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	outer->addWidget(build_menu_bar());

	// --- Toolbar --------------------------------------------------------
	QToolBar *bar = new QToolBar(this);
	bar->setMovable(false);

	// Icons from the style, with the characters as their text. On the desktop
	// the characters alone were fine; on a phone the font had no glyph for `↻`
	// and the reload button rendered as an empty box, while back and forward
	// happened to survive. Asking the style for the icon is both prettier and
	// not a bet on what fonts a platform ships.
	QAction *back_act   = bar->addAction("◀");
	QAction *fwd_act    = bar->addAction("▶");
	QAction *reload_act = bar->addAction("↻");
	// Only shown in drawer mode; on a wide window the tree is already visible
	// and a button to reveal it would be a control that does nothing.
	m_drawer_action = bar->addAction("☰");
	m_drawer_action->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
	m_drawer_action->setStatusTip("Show or hide the tab tree");
	m_drawer_action->setVisible(false);
	connect(m_drawer_action, &QAction::triggered, this,
	         [this] { set_drawer_open(!m_drawer_open); });

	back_act->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
	fwd_act->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
	reload_act->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
	bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
	connect(back_act,   &QAction::triggered, this, &main_window::go_back);
	connect(fwd_act,    &QAction::triggered, this, &main_window::go_forward);
	connect(reload_act, &QAction::triggered, this, &main_window::reload_page);

	m_address = new QLineEdit(this);
	m_address->setPlaceholderText("Address");
	m_address->setClearButtonEnabled(true);
	connect(m_address, &QLineEdit::returnPressed, this, &main_window::navigate_to_address);
	bar->addWidget(m_address);

	// The media affordance sits next to the policy shield and stays hidden
	// until something is detected — detection is progressive, so it fades in a
	// beat after load rather than being present and empty (§11.3).
	m_media_action = bar->addAction("Media");
	m_media_action->setToolTip("Watch or download media on this page");
	m_media_action->setVisible(false);
	connect(m_media_action, &QAction::triggered, this, &main_window::open_media);

	// §13.2 asks for a key icon in the field or the toolbar. It is a *text*
	// action here, beside Media and Shield, because that is what this toolbar
	// is made of -- inventing a glyph for one affordance would make it the odd
	// one out, and the icon set in `icons/` is the app's own mark at seven
	// sizes, not a symbol library. The behaviour §13.2 wants is the same either
	// way: it appears when a page has a login form, and it says what happened.
	m_key_action = bar->addAction("Key");
	m_key_action->setVisible(false);
	connect(m_key_action, &QAction::triggered, this, [this] {
		// Ask again, through the whole gate. Cheaper designs were available --
		// re-opening the last picker, or caching the entries -- and both mean
		// holding credentials for longer than the fill that asked for them.
		// Re-asking costs one round trip to a local socket and keeps §13.3.
		if (m_autofill)
			m_autofill->request_credentials(m_autofill->page_origin());
	});

	QAction *shield_act = bar->addAction("Shield");
	shield_act->setToolTip("Site controls");
	connect(shield_act, &QAction::triggered, this, &main_window::open_site_controls);

	outer->addWidget(bar);

	// --- Central splitter ----------------------------------------------
	m_tree = new QTreeView(this);
	m_tree->setModel(m_proxy);
	// One place to persist, however the change was made -- a drag, a rename, a
	// new folder. The tree file is the canonical record and a change that
	// survived only until the next launch would be worse than a refusal.
	connect(m_model, &tab_tree_model::structure_changed, this, [this] {
		if (!m_tree_path.isEmpty())
			m_model->save(m_tree_path);
	});
	m_tree->setHeaderHidden(true);
	m_tree->setUniformRowHeights(true);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	// File-manager gestures. `DragDrop` rather than `InternalMove` because the
	// model also publishes urls, so a tab can be dragged out to another
	// application -- InternalMove would refuse to hand anything over.
	m_tree->setDragEnabled(true);
	m_tree->setAcceptDrops(true);
	m_tree->setDropIndicatorShown(true);
	m_tree->setDragDropMode(QAbstractItemView::DragDrop);
	m_tree->setDefaultDropAction(Qt::MoveAction);
	m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_tree->expandAll();
	connect(m_tree, &QTreeView::activated, this, &main_window::on_tree_activated);
#ifdef Q_OS_ANDROID
	// A tap is not a double-click, and `activated` is what a double-click emits
	// on this style -- so on the device the tree looked responsive (rows
	// highlighted) and opened nothing, with the status bar still reading
	// "0 / 4 live" after several attempts. Touch gets `clicked` as well.
	//
	// Android only, deliberately: making a single click open a tab on the
	// desktop would change a behaviour nobody asked to have changed.
	connect(m_tree, &QTreeView::clicked, this, &main_window::on_tree_activated);
#endif
	connect(m_tree, &QTreeView::clicked,   this, &main_window::on_tree_activated);
	connect(m_tree, &QTreeView::customContextMenuRequested,
	         this, &main_window::on_tree_context_menu);

	m_stack = new QStackedWidget(this);
	QLabel *placeholder = new QLabel("Select a tab from the tree", this);
	placeholder->setAlignment(Qt::AlignCenter);
	m_stack->addWidget(placeholder);

	// Sort and Search filter the *tree*, so they belong above the tree rather
	// than in the toolbar beside Back/Forward/Address — those act on the page.
	// Putting a control next to what it does not control is a small lie about
	// the layout, and it costs a beat every time to work out which pane the
	// search box searches.
	QWidget *sidebar = new QWidget(this);
	m_sidebar = sidebar;
	QVBoxLayout *side = new QVBoxLayout(sidebar);
	side->setContentsMargins(4, 4, 4, 0);
	side->setSpacing(4);

	m_search = new QLineEdit(sidebar);
	m_search->setPlaceholderText("Search tree");
	m_search->setClearButtonEnabled(true);
	connect(m_search, &QLineEdit::textChanged, this, &main_window::on_search_changed);
	side->addWidget(m_search);

	QHBoxLayout *sort_row = new QHBoxLayout;
	sort_row->setContentsMargins(0, 0, 0, 0);
	sort_row->setSpacing(4);
	sort_row->addWidget(new QLabel("Sort:", sidebar));
	m_sort_box = new QComboBox(sidebar);
	m_sort_box->addItem("Tree order");
	m_sort_box->addItem("Title A–Z");
	m_sort_box->addItem("Newest");
	m_sort_box->addItem("Recently seen");
	connect(m_sort_box, &QComboBox::currentIndexChanged,
	         this, &main_window::on_sort_mode_changed);
	sort_row->addWidget(m_sort_box, 1);
	side->addLayout(sort_row);

	side->addWidget(m_tree, 1);

	QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
	m_splitter = splitter;
	splitter->addWidget(sidebar);
	splitter->addWidget(m_stack);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({280, 900});
	outer->addWidget(splitter, 1);   // takes all the leftover height

	// --- Status bar ------------------------------------------------------
	m_status = new QStatusBar(this);
	m_tab_counts = new QLabel(this);
	m_status->addPermanentWidget(m_tab_counts);
	m_status->showMessage("Ready");
	outer->addWidget(m_status);

	setWindowTitle("Hydra");
	resize(1180, 760);
#ifdef Q_OS_ANDROID
	// Nothing text-shaped gets focus at startup. Qt gives it to the first
	// focusable widget, which here is the address bar, and on a phone that
	// raises the soft keyboard over half the window before the user has asked
	// for anything -- seen on the device the moment the drawer worked.
	m_stack->setFocus(Qt::OtherFocusReason);
#endif

	// Realize this window with the visual the web engine needs, *before* it is
	// ever shown.
	//
	// The first web view added to a top-level that is already on screen makes
	// Qt recreate that window's native handle, and X then unmaps and remaps it:
	// measured at 30 fps, the whole browser window vanishes and the desktop
	// shows through for about a third of a second on the first tab open.
	// Building a view here and dropping it again forces the same handle to be
	// created up front, where there is nothing on screen to lose.
	//
	// Done through the factory rather than by naming an engine, so the seam
	// holds — a backend with no such requirement simply pays a cheap create
	// and destroy (§19.2).
	if (m_factory) {
		web_view_backend *warm = m_factory->create_view(this);
		m_stack->addWidget(warm->widget());
		// Loading is what does it, not creating: the engine instantiates its
		// render widget on the first load, and *that* is what forces the
		// window to be rebuilt. A blank load is enough.
		warm->load(QUrl("about:blank"));
		winId();
		QWidget *w = warm->widget();
		m_stack->removeWidget(w);
		delete w;                           // the backend is parented to it
	}

	update_status();
}

QMenuBar *main_window::build_menu_bar() {
	QMenuBar *menu = new QMenuBar(this);
	// Keep the bar inside the window; this is an X11-only app and a platform
	// menu bar would detach it from a widget we lay out ourselves.
	menu->setNativeMenuBar(false);

	QMenu *file_menu = menu->addMenu("&File");
	QAction *save_act = file_menu->addAction("&Save Tree", QKeySequence::Save,
	                                         this, &main_window::flush_tree);
	save_act->setStatusTip("Write the canonical tree file now");
	file_menu->addSeparator();
	QAction *quit_act = file_menu->addAction("&Quit", QKeySequence::Quit,
	                                         this, &QWidget::close);
	quit_act->setStatusTip("Suspend live tabs, save, and exit");

	QMenu *go_menu = menu->addMenu("&Go");
	go_menu->addAction("&Back", QKeySequence::Back, this, &main_window::go_back);
	go_menu->addAction("&Forward", QKeySequence::Forward, this, &main_window::go_forward);
	go_menu->addAction("&Reload", QKeySequence::Refresh, this, &main_window::reload_page);

	QMenu *view_menu = menu->addMenu("&View");
	QMenu *sort_menu = view_menu->addMenu("&Sort By");
	// The sort actions drive the toolbar combo rather than the proxy directly,
	// so the two controls cannot drift out of sync.
	static const char *sort_names[] = { "&Tree Order", "Title &A–Z",
	                                    "&Newest", "&Recently Seen" };
	QActionGroup *sort_group = new QActionGroup(this);
	for (int i = 0; i < 4; ++i) {
		QAction *a = sort_menu->addAction(sort_names[i]);
		a->setCheckable(true);
		a->setChecked(i == 0);
		sort_group->addAction(a);
		connect(a, &QAction::triggered, this, [this, i] { m_sort_box->setCurrentIndex(i); });
	}
	view_menu->addSeparator();
	view_menu->addAction("&Expand All", this, [this] { m_tree->expandAll(); });
	view_menu->addAction("&Collapse All", this, [this] { m_tree->collapseAll(); });
	view_menu->addSeparator();
	m_kiosk_action = view_menu->addAction("&Kiosk Mode", QKeySequence(Qt::Key_F11),
	                                       this, &main_window::toggle_kiosk);
	m_kiosk_action->setCheckable(true);
	m_kiosk_action->setStatusTip("Fullscreen chrome-less presentation; Esc returns");

	QMenu *tools_menu = menu->addMenu("&Tools");
	QAction *dl = tools_menu->addAction("&Downloads…", QKeySequence("Ctrl+J"),
	                                     this, &main_window::open_downloads);
	dl->setStatusTip("Show transfers in progress and finished");
	QAction *learn = tools_menu->addAction("&Learn This Site…", this,
	                                        &main_window::learn_this_site);
	learn->setStatusTip("Ask for a parser that finds this site's stream, and "
	                     "check it against what the page really requested");
	m_capture_action = tools_menu->addAction("&Capture Playing Video", this,
	                                          &main_window::toggle_capture);
	m_capture_action->setCheckable(true);
	m_capture_action->setStatusTip("Record what this page plays, for sites where "
	                                "no stream URL can be found");
	QAction *find = tools_menu->addAction("&Find Media on This Page…", this,
	                                       &main_window::find_media_with_ytdlp);
	find->setStatusTip("Ask yt-dlp what the video on this page is");
	QAction *prefs = tools_menu->addAction("&Settings…", this,
	                                        &main_window::open_settings);
	prefs->setStatusTip("Player, download folder and BitTorrent options");
	tools_menu->addSeparator();
	tools_menu->addAction("&Site Controls…", this, &main_window::open_site_controls);
	QAction *kp = tools_menu->addAction("Connect to &KeePassXC…", this,
	                                     &main_window::toggle_password_manager);
	kp->setStatusTip(keepass_bridge::supported()
	                     ? QStringLiteral("Pair with a running KeePassXC for autofill")
	                     : keepass_bridge::unavailable_reason());
	kp->setEnabled(keepass_bridge::supported());
	// A pairing that is kept between runs has to be removable between runs, and
	// from here rather than from a keyring editor. It is the user's record of
	// having let this program at their vault; storing it silently and offering
	// no way back would be the wrong half of the feature to build.
	QAction *kpf = tools_menu->addAction("&Forget KeePassXC Pairing", this,
	                                      &main_window::forget_keepass_pairing);
	kpf->setStatusTip(
	    credential_store::available()
	        ? QStringLiteral("Remove the stored pairing; KeePassXC will ask "
	                          "again next time")
	        : credential_store::unavailable_reason());
	// Enabled on whether there is one to forget, asked now rather than assumed.
	kpf->setEnabled(keepass_bridge::supported() &&
	                keepass_bridge::pairing_is_stored());
	// Generating is triggered from here rather than from the page. A page could
	// be given a way to ask, and then a page could ask unprompted -- and a
	// browser that hands out passwords because a script requested one is a
	// browser with a new attack surface for no benefit. The shell asks, and the
	// injected script puts the answer in the field that wanted it.
	QAction *kpg = tools_menu->addAction("&Generate Password", this, [this] {
		if (m_autofill)
			m_autofill->request_generated_password(m_autofill->page_origin());
	});
	kpg->setStatusTip("Ask KeePassXC for a password and put it in this page's "
	                   "new-password field");
	kpg->setEnabled(keepass_bridge::supported());
	tools_menu->addSeparator();
	QAction *reorg = tools_menu->addAction("&Reorganize Tree with AI…", this,
                                        &main_window::open_reorganizer);
	reorg->setStatusTip("Propose a new tree layout; nothing changes until you accept");
	QAction *eva = tools_menu->addAction("Evolve Ad &Filters…", this,
	                                      &main_window::open_filter_evolution);
	eva->setStatusTip("Propose filter rules for ads that slipped through here");
	QAction *zap = tools_menu->addAction("&Zap an Element…", this,
	                                      &main_window::start_element_picker);
	zap->setStatusTip("Click a leaked ad; Escape cancels");
	QAction *banners = tools_menu->addAction("&Cookie Banners We Missed…", this,
	                                          &main_window::open_site_rules);
	banners->setStatusTip("Teach a rule from a consent banner nothing matched");
	tools_menu->addSeparator();
	// Ctrl+Shift+Z rather than Ctrl+Z, so this never steals undo from a focused
	// text field in the page or the address bar.
	m_undo_action = tools_menu->addAction("&Undo Reorganize",
	                                       QKeySequence("Ctrl+Shift+Z"), this,
	                                       &main_window::undo_reorganize);
	m_undo_action->setEnabled(false);
	m_undo_action->setStatusTip("Put the tree back the way it was before the "
	                             "last accepted reorganization");

	QMenu *help_menu = menu->addMenu("&Help");
	help_menu->addAction("&About", this, &main_window::on_about);

	return menu;
}

void main_window::toggle_kiosk() {
	if (!m_kiosk) {
		m_kiosk = new kiosk_controller(this);
		// Coming back from kiosk: put the view on screen again and resync the
		// menu check, which also covers Esc exiting without going through here.
		connect(m_kiosk, &kiosk_controller::left, this, [this] {
			if (web_view_backend *v = m_kiosk->view())
				m_stack->addWidget(v->widget());
			m_kiosk_action->setChecked(false);
			show();
			if (web_view_backend *v = current_view())
				m_stack->setCurrentWidget(v->widget());
			sync_page_context();
			update_status();
		});
	}

	if (m_kiosk->active()) {
		m_kiosk->exit();
		return;
	}

	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Kiosk mode needs an open tab", 4000);
		m_kiosk_action->setChecked(false);
		return;
	}

	// Saved settings, not the controller's compiled-in defaults: this is the
	// whole difference between the kiosk page existing and mattering.
	kiosk_config c = settings_store::kiosk();
	// A configured home wins, because someone who set one meant it for exactly
	// this. With none, the tab you were on is the sensible thing to show and is
	// what it has always done.
	if (!c.home.isValid() || c.home.toString().isEmpty())
		c.home = v->url();
	m_kiosk->set_config(c);

	m_stack->removeWidget(v->widget());   // the controller takes it from here
	if (m_kiosk->enter(v, m_stack)) {
		m_kiosk_action->setChecked(true);
		hide();                            // the stage is its own fullscreen window
	} else {
		m_stack->addWidget(v->widget());
		m_kiosk_action->setChecked(false);
	}
}

void main_window::open_reorganizer() {
	// Which backend, and whether one is allowed at all, is a single global
	// setting resolved in one place (§9.1). The dialog still gates an external
	// provider behind review-before-send either way.
	QString why;
	ai_provider *chosen = choose_ai(&why);
	if (!chosen) {
		m_status->showMessage(why, 8000);
		return;
	}

	// §9.4: snapshot before applying, so any accepted change is one keystroke
	// to revert. Structure only — payloads follow ids and are never touched.
	const tree_snapshot before = m_model->take_snapshot();
	reorganize_dialog dlg(m_model, chosen, this);
	if (dlg.exec() == QDialog::Accepted) {
		m_undo = before;
		m_undo_action->setEnabled(true);
		m_tree->expandAll();
		mark_dirty();
	}
}

void main_window::on_media_found(const QString &site_host, int count) {
	Q_UNUSED(count)
	refresh_media_affordance(site_host);
}

void main_window::refresh_media_affordance(const QString &site_host) {
	web_view_backend *v = current_view();
	if (!v || v->url().host() != site_host)
		return;   // detection on a background tab; don't retitle this one

	const int  found   = m_media->count_for(site_host);
	const bool playing = m_mse && m_mse->active_for(site_host);
	m_media_action->setVisible(found > 0 || playing);

	if (found > 0) {
		m_media_action->setText(QString("Media (%1)").arg(found));
		m_media_action->setToolTip("Watch or download media on this page");
		return;
	}

	// The §11.6 case: the page is demonstrably playing, and watching request
	// URLs found nothing. Saying so is worth more than an empty badge, which
	// on such a site is simply a lie.
	qint64 buffered = 0;
	QString mime;
	for (const mse_stream &s : m_mse->streams_for(site_host)) {
		buffered += s.bytes;
		if (mime.isEmpty())
			mime = s.mime;
	}
	m_media_action->setText("Media (playing)");
	m_media_action->setToolTip(
		QString("This page is playing video (%1 buffered, %2), but no stream "
		         "URL was detected — try Tools ▸ Find Media on This Page.")
		    .arg(QLocale().formattedDataSize(buffered), mime));
}

void main_window::open_media() {
	web_view_backend *v = current_view();
	if (!v)
		return;
	QString node_id;
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value() == v) { node_id = it.key(); break; }

	// Anything this site has been taught (§11.5) runs before the list is built,
	// so a learned stream appears beside whatever detection found on its own.
	apply_extractor(v->url().host(), v->url());

	// The context a CDN expects: the page that loaded the stream, and this
	// browser's own User-Agent.
	stream_context ctx;
	ctx.referer = v->url().toString();
	media_dialog dlg(m_media, m_players, m_downloads, m_local_proxy, m_mse, this);
	connect(&dlg, &media_dialog::capture_requested, this, [this] {
		// Queued: the dialog is closing itself, and starting a capture reloads
		// the page — not something to do from inside its own exec().
		QTimer::singleShot(0, this, [this] {
			if (m_capture_url.isEmpty())
				toggle_capture();
		});
	});
	dlg.set_site(v->url().host(), node_id, ctx);
	dlg.exec();
}

void main_window::open_filter_evolution() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first — filter evolution works from "
		                      "what that page requested.", 5000);
		return;
	}
	QString why;
	ai_provider *chosen = choose_ai(&why);
	if (!chosen) {
		m_status->showMessage(why, 8000);
		return;
	}

	filter_dialog dlg(m_signals, m_filters, chosen, v->url().host(),
	                   m_picker->last(), this);
	if (dlg.exec() == QDialog::Accepted && !m_filters_path.isEmpty())
		m_filters->save(m_filters_path);
}

void main_window::toggle_password_manager() {
	if (!keepass_bridge::supported()) {
		// The reason, not a guess at it: on a desktop without libsodium the
		// answer is to install it, and on Android there is nothing to install.
		m_status->showMessage(keepass_bridge::unavailable_reason(), 8000);
		return;
	}
	if (!m_keepass->connected()) {
		connect(m_keepass, &keepass_bridge::ready, this, [this] {
			// A stored pairing is tested rather than re-created, so the user is
			// not asked to confirm a new connection on every launch (§13.1).
			//
			// This comment described what was *meant* to happen for as long as
			// there was nowhere to store a pairing: `associated()` only ever
			// answered for this process's memory, which on a fresh launch is
			// empty, so the test branch was unreachable and every launch asked
			// the user to confirm again. Loading it is what makes it true.
			if (m_keepass->restore_pairing() || m_keepass->associated())
				m_keepass->test_association();
			else
				m_keepass->associate();
		}, Qt::SingleShotConnection);
		m_status->showMessage("Connecting to KeePassXC…", 4000);
		m_keepass->start();
	} else if (!m_keepass->associated()) {
		m_keepass->associate();
	} else {
		m_status->showMessage("Already paired with KeePassXC.", 4000);
	}
}

void main_window::forget_keepass_pairing() {
	if (!keepass_bridge::pairing_is_stored()) {
		m_status->showMessage(credential_store::available()
		                          ? QStringLiteral("No stored KeePassXC pairing.")
		                          : credential_store::unavailable_reason(),
		                      6000);
		return;
	}
	// Confirmed, because the cost of undoing it is the one step that needs a
	// human at the KeePassXC end -- this is cheap to click and not cheap to
	// reverse.
	if (QMessageBox::question(
	        this, "Forget KeePassXC pairing",
	        "Remove the stored pairing?\n\nKeePassXC will ask you to confirm a "
	        "new connection the next time this browser asks for a password. "
	        "Nothing in your vault is changed.") != QMessageBox::Yes)
		return;
	const bool gone = m_keepass->forget_pairing();
	m_status->showMessage(gone ? QStringLiteral("KeePassXC pairing forgotten.")
	                           : QStringLiteral(
	                                 "Could not remove the stored pairing."),
	                      6000);
}

void main_window::undo_reorganize() {
	if (!m_undo.valid())
		return;
	const int n = m_model->restore_snapshot(m_undo);
	m_undo = tree_snapshot{};        // one level, as §9.4 specifies
	m_undo_action->setEnabled(false);
	m_tree->expandAll();
	mark_dirty();
	m_status->showMessage(QString("Reverted the reorganization (%1 nodes).").arg(n),
	                       5000);
}

void main_window::open_site_rules() {
	consent_dialog dlg(m_consent, m_site_rules_path, this);
	dlg.exec();
}

void main_window::start_element_picker() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first.", 4000);
		return;
	}
	m_status->showMessage("Click the element to block — Escape cancels.", 0);
	m_picker->begin(v->url().toString());
}

void main_window::on_about() {
	QMessageBox::about(this, "About Hydra",
		"<b>Hydra</b><br>"
		"A tab-tree browser over an embedded Chromium, with a per-site "
		"security policy engine.<br><br>"
		"Licensed GPL-3.0-or-later.");
}

void main_window::update_status() {
	const int live = m_views_by_id.size();
	m_tab_counts->setText(QString("%1 / %2 live").arg(live).arg(k_max_live_views));
}

bool main_window::event(QEvent *e) {
	// A QMainWindow forwards status tips to its status bar for free. Without
	// one, hovering a menu item would do nothing, so route them here.
	if (e->type() == QEvent::StatusTip) {
		const QString tip = static_cast<QStatusTipEvent *>(e)->tip();
		if (m_status)
			m_status->showMessage(tip.isEmpty() ? QStringLiteral("Ready") : tip);
		return true;
	}
	return QWidget::event(e);
}

main_window::~main_window() {
	delete m_players;
	delete m_filters;
	// Leave kiosk while our children still exist: the controller hands the
	// presented view's widget back to the stack, and doing that after the
	// stack has been destroyed is a use-after-free. Destructor bodies run
	// before QObject tears down children, so here is the right place.
	if (m_kiosk && m_kiosk->active())
		m_kiosk->exit();
}

bool main_window::load_tree(const QString &path) {
	m_tree_path = path;
	const QString dir = QFileInfo(path).absolutePath();
	delete m_state;
	m_state = new state_store(dir + "/state");

	// .ini, and the loader reads a policy.json left by an older build once —
	// the first save writes the new file and the old one simply stops being
	// consulted. Nobody has to migrate anything by hand.
	m_policy_path = dir + "/policy.ini";
	m_policy->load(m_policy_path);   // no-op if the file doesn't exist yet

	// The AI/user-authored filter list lives beside the rest, kept separate
	// from any imported EasyList so upstream updates cannot clobber it (§12.5).
	m_filters_path = dir + "/filters-ai.txt";
	const bool filters_read = m_filters->load(m_filters_path);
	// Where the rules came from and how many, on request. A filter list that is
	// silently empty and one that is silently ignored look identical from a page,
	// and telling them apart by reasoning has already cost a round.
	if (qEnvironmentVariableIsSet("HYDRA_FILTER_DEBUG"))
		qWarning("filters: %s %s, %lld rule(s)", qPrintable(m_filters_path),
		          filters_read ? "read" : "MISSING",
		          static_cast<long long>(m_filters->rules().size()));
	// And hand them to the interceptor, which is the only thing that can act on
	// them. Loaded first so a rule accepted in an earlier session is in force
	// before the first request of this one.
	if (m_filter)
		m_filter->set_filter_list(m_filters);

	// Consent-banner rules, beside the rest and in the same spirit: data, not
	// code. The built-in set is always present; the file adds to it. This is
	// the unit a future exchange between users would move, which is why it is a
	// file from the start rather than something to be extracted from the binary
	// later (§7.1, `cookie_notices`).
	m_site_rules_path = dir + "/site-rules.ini";
	site_rules cr;
	if (!cr.load(m_site_rules_path))
		cr = site_rules::defaults();
	m_consent->set_rules(cr);
	m_antiadblock->set_rules(cr);

	const bool ok = m_model->load(path);
	m_tree->expandAll();
	return ok;
}

void main_window::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	update_layout_mode();
	if (m_drawer_mode && m_sidebar) {
		// Keep the drawer the right size and where it belongs, whichever state
		// it is in -- a rotation while it is open must not leave it half off.
		const int w = qMin(int(width() * 0.82), 420);
		const int top = m_splitter ? m_splitter->y() : 0;
		m_sidebar->resize(w, height() - top - (m_status ? m_status->height() : 0));
		m_sidebar->move(m_drawer_open ? 0 : -w, top);
	}
}

void main_window::update_layout_mode() {
	const bool narrow = width() < k_drawer_threshold;
	if (narrow == m_drawer_mode || !m_sidebar || !m_splitter)
		return;
	m_drawer_mode = narrow;
	m_drawer_action->setVisible(narrow);

	if (narrow) {
		// Out of the splitter and on top of the window. The stack then takes
		// the whole width, which is the entire point: a page on a phone gets
		// the screen rather than a third of it.
		m_sidebar->setParent(this);
		m_sidebar->setAutoFillBackground(true);
		m_sidebar->raise();
		set_drawer_open(false, false);
		m_sidebar->show();
	} else {
		// Back where it came from, at the position it had.
		m_splitter->insertWidget(0, m_sidebar);
		m_sidebar->move(0, 0);
		m_splitter->setSizes({280, qMax(400, width() - 280)});
		m_drawer_open = false;
	}
}

void main_window::set_drawer_open(bool open, bool animate) {
	if (!m_drawer_mode || !m_sidebar)
		return;
	m_drawer_open = open;
	const int w = m_sidebar->width();
	const int to = open ? 0 : -w;
	if (!animate) {
		m_sidebar->move(to, m_sidebar->y());
		return;
	}
	if (!m_drawer_anim)
		m_drawer_anim = new QPropertyAnimation(m_sidebar, "pos", this);
	m_drawer_anim->stop();
	m_drawer_anim->setDuration(180);
	m_drawer_anim->setEasingCurve(QEasingCurve::OutCubic);
	m_drawer_anim->setStartValue(m_sidebar->pos());
	m_drawer_anim->setEndValue(QPoint(to, m_sidebar->y()));
	m_drawer_anim->start();
	if (open)
		m_sidebar->raise();
}

web_view_backend *main_window::current_view() const {
	// Linear over at most k_max_live_views entries, which beats keeping a
	// second piece of state in sync with the stack.
	QWidget *w = m_stack->currentWidget();
	for (web_view_backend *v : m_views_by_id)
		if (v->widget() == w)
			return v;
	return nullptr;
}

void main_window::open_node(node *n) {
	if (!n || n->url.isEmpty())
		return;  // folders and empty nodes have nothing to show

	n->last_seen = QDateTime::currentDateTime();

	web_view_backend *view = m_views_by_id.value(n->id, nullptr);
	if (!view) {
		view = m_factory->create_view(this);
		m_views_by_id.insert(n->id, view);
		m_stack->addWidget(view->widget());

		apply_policy(view, QUrl::fromUserInput(n->url).host());

		// Autofill plumbing (architecture doc §13.2): the content script runs
		// in an isolated world and talks to a bridge object the shell owns.
		// The page never sets its own origin — the shell does, on navigation.
		view->set_script_bridge(m_autofill, "hydraAutofill");
		view->inject_script("hydra-autofill",
		                     QString::fromUtf8(autofill_script::source()));
		// The element picker rides the same seam — that is why it waited for
		// step 7 rather than shipping with the rest of §12.
		view->set_script_bridge(m_picker, "hydraPicker");
		view->inject_script("hydra-picker",
		                     QString::fromUtf8(picker_script::source()));
		// §11.6, in two halves. The relay is privileged and stays isolated with
		// the others; the hook is the only thing that goes into the page's own
		// world, and it carries nothing.
		// The consent banner, answered rather than merely hidden (§7.1's
		// cookie_notices). Same isolated world as the others: it clicks the
		// page's own buttons, so the page must not be able to rewrite it.
		view->set_script_bridge(m_consent, consent_blocker::bridge_name());
		// On subframes too, which is the whole difference between answering a
		// CMP and looking at one: vendors ship them as iframes.
		view->inject_script("hydra-consent", consent_blocker::script_source(), true);
		view->set_script_bridge(m_cosmetic, cosmetic_filters::bridge_name());
		view->inject_script("hydra-cosmetic", cosmetic_filters::script_source());
		view->set_script_bridge(m_mse, mse_tap::bridge_name());
		view->inject_script("hydra-mse-relay", mse_tap::relay_source());
		view->inject_main_world_script("hydra-mse-hook", mse_tap::hook_source());

		connect(view, &web_view_backend::url_changed, this, [this, view](const QUrl &u) {
			apply_policy(view, u.host());
			if (view == current_view()) {
				sync_page_context();
				update_address(u.toString());
			}
		});

		// Feature permissions (geo/cam/mic/notifications) answered from policy.
		// The backend maps its engine's own feature enum onto policy::feature,
		// so this stays a pure policy lookup on our side.
		policy_engine *pe = m_policy;
		view->set_permission_decider([pe](const QUrl &origin, policy::feature f) {
			return pe->is_allowed(f, origin.host());
		});


		if (n->type == node_type::suspended_tab && m_state && m_state->has_state(n->id)) {
			// Restore the session blob this node was suspended into.
			view->restore_state(m_state->load(n->id));
			m_state->remove(n->id);
		} else {
			view->load(QUrl::fromUserInput(n->url));
		}
	}

	n->type = node_type::open_tab;
	m_model->refresh_node(n);
	m_stack->setCurrentWidget(view->widget());
	sync_page_context();
	update_address(view->url().toString());
	touch_lru(n->id);
	enforce_live_cap(n->id);
	mark_dirty();
	update_status();
}


// Which site the page-scoped bridges are answering about.
//
// This used to happen in one place -- the current view's `url_changed` -- and
// that was wrong twice over.
//
// **Switching tabs did not update it.** Activating an already-loaded tab
// navigates nothing, so no signal arrived and the consent blocker went on
// answering `active_now()` and `rules_json()` for the site of the tab before it.
// A bridge deliberately built so the page cannot name its own host is not much
// use if the shell then names the wrong one.
//
// **And the ordering only worked by accident.** `activate_node()` calls
// `view->load()` before `setCurrentWidget()`. Qt WebEngine emits `urlChanged`
// asynchronously, so the signal landed after the switch and the
// `view == current_view()` test passed. The Android backend emits it from inside
// `load()`, synchronously, so it arrived while the previous view was still
// current, the test failed, and the host was never set at all -- cosmetic
// filtering silently did nothing there while every unit test passed. Reading it
// from whatever is current, after the switch, does not depend on which way a
// backend emits.
void main_window::sync_page_context() {
	web_view_backend *v = current_view();
	if (!v)
		return;
	const QUrl u = v->url();
	if (m_consent)
		m_consent->set_page_host(u.host());
	if (m_cosmetic)
		m_cosmetic->set_page_host(u.host());
	// The key belongs to the page that raised it: a new document has not asked
	// for anything yet, and a tick left over from the last one would claim a
	// fill that did not happen here.
	if (m_key_action)
		m_key_action->setVisible(false);
	if (m_autofill) {
		m_autofill->set_page_origin(
			u.adjusted(QUrl::RemovePath | QUrl::RemoveQuery |
			            QUrl::RemoveFragment).toString());
	}
}

void main_window::suspend_node(node *n) {
	if (!n)
		return;
	web_view_backend *view = m_views_by_id.value(n->id, nullptr);
	if (!view)
		return;

	// Never tear down the view a kiosk session is presenting. Without this the
	// live-view cap could suspend the tab that is currently fullscreen.
	if (m_kiosk && m_kiosk->active() && m_kiosk->view() == view)
		return;

	if (m_state)
		m_state->save(n->id, view->save_state());

	if (view == current_view())
		m_stack->setCurrentIndex(0);  // back to placeholder
	QWidget *w = view->widget();
	m_stack->removeWidget(w);
	m_views_by_id.remove(n->id);
	m_lru.removeAll(n->id);
	w->deleteLater();   // the backend is parented to its widget and goes too

	n->type = node_type::suspended_tab;
	m_model->refresh_node(n);
	mark_dirty();
	update_status();
}

void main_window::touch_lru(const QString &id) {
	m_lru.removeAll(id);
	m_lru.prepend(id);
}

void main_window::enforce_live_cap(const QString &keep_id) {
	while (m_views_by_id.size() > k_max_live_views) {
		QString victim;
		for (auto it = m_lru.crbegin(); it != m_lru.crend(); ++it) {
			if (*it != keep_id && m_views_by_id.contains(*it)) {
				victim = *it;
				break;
			}
		}
		if (victim.isEmpty())
			break;
		if (node *n = m_model->node_by_id(victim))
			suspend_node(n);
		else
			break;
	}
}

void main_window::on_tree_activated(const QModelIndex &proxy_index) {
	// Picking a tab is what the drawer was opened for, so it gets out of the
	// way. Leaving it up would cover the page the user just asked for.
	if (m_drawer_mode)
		set_drawer_open(false);
	if (!proxy_index.isValid())
		return;
	node *n = m_model->node_for_index(m_proxy->mapToSource(proxy_index));
	open_node(n);
}

void main_window::on_tree_context_menu(const QPoint &pos) {
	const QModelIndex proxy_index = m_tree->indexAt(pos);
	// A right-click on empty space is still a place to make a folder. The first
	// version returned here, and it also returned for folders -- so the only
	// rows with a menu at all were tabs, and the container everything lives in
	// could not be renamed, emptied or added to.
	node *n = proxy_index.isValid()
	              ? m_model->node_for_index(m_proxy->mapToSource(proxy_index))
	              : nullptr;

	QMenu menu(this);
	QAction *open_a = nullptr, *sus_a = nullptr;
	if (n && !n->is_folder()) {
		open_a = menu.addAction("&Open");
		sus_a  = menu.addAction("&Suspend");
		sus_a->setEnabled(m_views_by_id.contains(n->id));
		menu.addSeparator();
	}

	QAction *copy_url_a = nullptr, *dup_a = nullptr;
	if (n && !n->url.isEmpty())
		copy_url_a = menu.addAction("&Copy Address");
	if (n)
		dup_a = menu.addAction("&Duplicate");

	QAction *folder_a = menu.addAction("New &Folder Here");
	menu.addSeparator();
	QAction *props_a = n ? menu.addAction("&Properties…") : nullptr;
	QAction *del_a   = n ? menu.addAction("&Delete") : nullptr;

	QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
	if (!chosen)
		return;
	if (chosen == open_a)             open_node(n);
	else if (chosen == sus_a)         suspend_node(n);
	else if (chosen == copy_url_a)    QGuiApplication::clipboard()->setText(n->url);
	else if (chosen == dup_a)         m_model->duplicate_node(n);
	else if (chosen == folder_a) {
		// Into the folder that was clicked, or beside a tab -- which is what a
		// file manager does, and saves a drag immediately afterwards.
		node *parent = !n ? nullptr : (n->is_folder() ? n : n->parent);
		if (node *f = m_model->add_folder(parent, QString()))
			m_tree->expandAll(), edit_node_properties(f);
	} else if (chosen == props_a)     edit_node_properties(n);
	else if (chosen == del_a) {
		// Deleting a folder takes what is in it, so the count goes in the
		// question rather than being discovered afterwards.
		const int kids = n->children.size();
		const QString what = kids > 0
			? QString("Delete \"%1\" and the %2 item%3 inside it?")
			      .arg(n->title).arg(kids).arg(kids == 1 ? "" : "s")
			: QString("Delete \"%1\"?").arg(n->title);
		if (QMessageBox::question(this, "Delete", what) == QMessageBox::Yes)
			m_model->remove_node(n);
	}
}

// The properties editor §4 never had: what a node *is*, as opposed to where it
// sits. The id is shown and not editable -- it keys `state/<id>.blob` and the
// outline file, so retyping it would orphan a tab's history with no warning,
// which is exactly the kind of silent loss this project keeps finding.
void main_window::edit_node_properties(node *n) {
	if (!n)
		return;
	QDialog dlg(this);
	dlg.setWindowTitle(n->is_folder() ? "Folder properties" : "Tab properties");
	auto *form = new QFormLayout(&dlg);

	auto *title = new QLineEdit(n->title, &dlg);
	auto *url   = new QLineEdit(n->url, &dlg);
	auto *tags  = new QLineEdit(n->tags.join(", "), &dlg);
	url->setPlaceholderText("about:blank");
	tags->setPlaceholderText("comma separated");

	auto *id = new QLabel(n->id, &dlg);
	id->setTextInteractionFlags(Qt::TextSelectableByMouse);
	id->setToolTip("Not editable: this is what the tab's saved state and the "
	                "tree file are keyed by.");

	form->addRow("Title", title);
	if (!n->is_folder())
		form->addRow("Address", url);
	form->addRow("Tags", tags);
	form->addRow("Id", id);
	form->addRow("Added", new QLabel(n->created.toString(Qt::ISODate), &dlg));
	form->addRow("Last seen", new QLabel(n->last_seen.toString(Qt::ISODate), &dlg));

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
	                                      QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	form->addRow(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return;
	QStringList tag_list;
	for (const QString &t : tags->text().split(',', Qt::SkipEmptyParts))
		if (!t.trimmed().isEmpty())
			tag_list << t.trimmed();
	m_model->update_node(n, title->text(),
	                      n->is_folder() ? n->url : url->text(), tag_list);
}

void main_window::on_sort_mode_changed(int combo_index) {
	using SM = tree_sort_proxy::sort_mode;
	static const SM modes[] = { SM::tree_order, SM::title_asc,
	                            SM::newest_created, SM::recently_seen };
	if (combo_index >= 0 && combo_index < 4) {
		m_proxy->set_sort_mode(modes[combo_index]);
		// Dropping *between* rows is a position, and a position only means
		// something in tree order -- sorted by title, the row would jump back
		// the instant it re-sorted. Dropping *onto* a folder stays available in
		// every mode, so the honest half of the gesture keeps working.
		m_model->set_reorder_allowed(modes[combo_index] == SM::tree_order);
	}
	m_tree->expandAll();
}

void main_window::on_search_changed(const QString &text) {
	m_proxy->set_search_text(text);
	if (!text.trimmed().isEmpty())
		m_tree->expandAll();
}

void main_window::navigate_to_address() {
	const QString text = m_address->text().trimmed();

	// A magnet link is not a page. Handing it to the engine would produce an
	// error page, so route it to the download manager instead — which is what
	// "first class" means in practice: the same box, the same result as any
	// other download link (§11.4).
	const QUrl direct(text);
	if (m_downloads->source_for(direct) &&
	    direct.scheme().compare("magnet", Qt::CaseInsensitive) == 0) {
		start_download(direct);
		return;
	}

	if (web_view_backend *v = current_view())
		v->load(QUrl::fromUserInput(text));
}

void main_window::start_download(const QUrl &url) {
	QString error;
	// A download belongs to the node it came from (§11.2), so a torrent lands
	// in the tree beside every other download rather than in a separate world.
	QString node_id;
	if (web_view_backend *v = current_view()) {
		for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
			if (it.value() == v) { node_id = it.key(); break; }
	}

	const int id = m_downloads->enqueue(url, node_id, &error);
	if (!id) {
		m_status->showMessage(error, 8000);
		return;
	}
	m_address->clear();
	// Show the list rather than announce it in the status bar: a download the
	// user cannot watch is not a first-class download (§11.2).
	open_downloads();
}

ai_provider *main_window::choose_ai(QString *why) {
	if (!m_local_ai) {
		m_local_ai    = new ollama_provider(this);
		m_external_ai = new claude_provider(this);
		settings_store::load_into(nullptr, nullptr, nullptr, m_local_ai,
		                           m_external_ai);
	}

	// Re-probe every time rather than trusting a cached answer. Ollama is
	// started and stopped like any other local service, so a result from
	// earlier in the session says nothing about now — and the old code probed
	// asynchronously then read the answer immediately, which meant the first
	// use of the day *never* saw a running local model and quietly used the
	// external one instead.
	QApplication::setOverrideCursor(Qt::WaitCursor);
	const bool local_ok = m_local_ai->probe_now();
	QApplication::restoreOverrideCursor();
	const bool ext_ok   = m_external_ai->available();

	switch (settings_store::ai_mode()) {
		case ai_choice::local_only:
			// Deliberately does *not* fall back. The whole value of this
			// setting is that "nothing leaves this machine" keeps meaning that
			// on the day the local model is not running (§1, §9.1).
			if (local_ok)
				return m_local_ai;
			if (why)
				*why = "No local model is answering, and the AI backend is set "
				       "to local only. Start Ollama, or change it in Settings.";
			return nullptr;

		case ai_choice::external:
			if (ext_ok)
				return m_external_ai;
			if (why)
				*why = "Claude is selected but has no API key. Set one in "
				       "Settings, or ANTHROPIC_API_KEY.";
			return nullptr;

		case ai_choice::automatic:
			break;
	}

	// Local-first: a reachable local model handles everything and nothing
	// leaves the machine; the external one is the fallback, still gated behind
	// review-before-send by the dialogs.
	if (local_ok)
		return m_local_ai;
	if (ext_ok)
		return m_external_ai;
	if (why)
		*why = "No AI provider: start Ollama locally, or set ANTHROPIC_API_KEY.";
	return nullptr;
}

int main_window::apply_extractor(const QString &host, const QUrl &page) {
	if (host.isEmpty() || !m_extractors.has(host))
		return 0;
	const QList<evidence_request> ev = m_ex_signals->evidence_for(host);
	if (ev.isEmpty())
		return 0;

	// Judged again on every run, not merely when it was accepted. A stored
	// script is re-run against fresh evidence each time, and the same rule
	// applies: it may only return an address this visit actually requested.
	// A site that changes shape therefore stops producing results rather than
	// producing wrong ones.
	const extractor_verdict v =
	    site_extractor::check(m_extractors.source_for(host), page, ev);
	if (!v.usable) {
		m_status->showMessage(
		    QString("The saved extractor for %1 no longer matches this page (%2)")
		        .arg(host, v.message.left(90)), 9000);
		return 0;
	}

	media_item item;
	item.kind = v.result.kind == "hls"    ? media_kind::hls
	          : v.result.kind == "dash"   ? media_kind::dash
	                                      : media_kind::direct;
	item.url       = v.result.url;
	item.site_host = host;
	item.headers   = v.result.headers;   // the whole point of asking for them
	item.label     = QString("Learned · %1").arg(v.result.kind);
	m_media->add_item(host, item);
	return 1;
}

void main_window::learn_this_site() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first.", 5000);
		return;
	}
	QString why;
	ai_provider *chosen = choose_ai(&why);
	if (!chosen) {
		m_status->showMessage(why, 8000);
		return;
	}

	const QString host = v->url().host();
	extractor_dialog dlg(m_ex_signals, &m_extractors, chosen, host, v->url(), this);

	// The §11.5.1 helper tier, and only where this site has been trusted with
	// it. `extractor_fetch` defaults to block, so on an ordinary site the
	// script gets no `hydra` at all and cannot tell the surface exists.
	//
	// Both live on the stack beside the modal dialog on purpose: a helper_host
	// must outlive the run it was handed to, and the run cannot outlive an
	// exec() that has not returned.
	std::unique_ptr<network_fetcher> fetcher;
	std::unique_ptr<helper_allowlist> allow;
	std::unique_ptr<helper_host> helpers;
	if (m_policy && m_policy->is_allowed(policy::feature::extractor_fetch, host)) {
		stream_context ctx;
		ctx.referer = v->url().toString();
		fetcher = std::make_unique<network_fetcher>(ctx);
		allow   = std::make_unique<helper_allowlist>();
		allow->observe(m_ex_signals->evidence_for(host));
		helpers = std::make_unique<helper_host>(allow.get(), fetcher->as_function(),
		                                         helper_budget{});
		// The same ranking the content-type probe uses, handed to the script so
		// its budget is spent where the answer usually is rather than on
		// whatever the page happened to request first.
		QStringList ranked;
		for (const evidence_request &r : extractor_dialog::candidates(
		         m_ex_signals->evidence_for(host), v->url(), helper_budget{}.max_calls))
			ranked << r.url.toString();
		helpers->set_candidates(ranked);
		dlg.use_helpers(helpers.get());
	}

	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString dir = QStandardPaths::writableLocation(
	    QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);
	m_extractors.save(QDir(dir).filePath("extractors.json"));
	const int found = apply_extractor(host, v->url());
	m_status->showMessage(
	    found ? QString("Extractor saved for %1, and it found a stream — see the "
	                     "media list.").arg(host)
	          : QString("Extractor saved for %1.").arg(host), 9000);
	refresh_media_affordance(host);
}

void main_window::toggle_capture() {
	web_view_backend *v = current_view();
	if (!v) {
		m_capture_action->setChecked(false);
		m_status->showMessage("Open a page first.", 5000);
		return;
	}

	// --- stop --------------------------------------------------------------
	if (!m_capture_url.isEmpty()) {
		if (m_capture_timer)
			m_capture_timer->stop();
		m_capture_action->setText("&Capture Playing Video");
		const qint64 got = m_local_proxy->captured_bytes(m_capture_url);
		m_local_proxy->close_capture(m_capture_url);
		m_capture_url.clear();
		m_capture_action->setChecked(false);

		if (m_capture_src && m_capture_job) {
			m_capture_src->ended(m_capture_job, got > 0,
			                      got > 0 ? QString()
			                              : QString("Nothing was captured."));
			m_capture_job = 0;
		}
		if (got <= 0) {
			QFile::remove(m_capture_path);
			m_status->showMessage("Nothing was captured — the page may not use "
			                       "Media Source, or never started playing.", 9000);
			return;
		}
		// Offer it the same way anything else found on this page is offered, so
		// Watch and the download list need to know nothing about capture.
		media_item item;
		item.kind      = media_kind::direct;
		item.url       = QUrl::fromLocalFile(m_capture_path);
		item.site_host = v->url().host();
		item.label     = QString("Captured · %1")
		                     .arg(QLocale().formattedDataSize(got));
		m_media->add_item(item.site_host, item);
		m_status->showMessage(QString("Captured %1 to %2")
		                          .arg(QLocale().formattedDataSize(got),
		                               m_capture_path), 12000);
		return;
	}

	// --- start -------------------------------------------------------------
	if (!m_local_proxy || !m_local_proxy->listening()) {
		m_capture_action->setChecked(false);
		m_status->showMessage("Cannot capture: the local proxy is not listening.",
		                       8000);
		return;
	}
	const QString host = v->url().host();
	m_capture_path = QDir(m_downloads->directory())
	                     .filePath(QString("%1-%2.mp4")
	                                   .arg(host.isEmpty() ? "capture" : host,
	                                        QDateTime::currentDateTime()
	                                            .toString("yyyyMMdd-hhmmss")));
	m_capture_url = m_local_proxy->open_capture(m_capture_path);
	if (m_capture_url.isEmpty()) {
		m_capture_action->setChecked(false);
		m_status->showMessage("Cannot capture: could not open " + m_capture_path,
		                       8000);
		return;
	}

	// The hook has to be in place before the player builds its MediaSource, so
	// arming means injecting and reloading. Catching a stream mid-playback
	// would miss the init segment and produce a file nothing can decode.
	v->inject_main_world_script("hydra-mse-capture",
	                             mse_tap::capture_source(m_capture_url));
	m_capture_action->setChecked(true);
	m_status->showMessage("Capturing — reloading so the recorder is in place "
	                       "before playback starts. Press play, then turn this "
	                       "off when done.", 12000);

	// Without this the whole recording is silent: arm it, walk away, and
	// nothing says whether anything is being written until it is stopped. The
	// commonest way to get an empty file is forgetting to press play, and that
	// deserves to be said out loud rather than discovered at the end.
	// From here it is an ordinary job: the downloads window shows it with the
	// same progress and the same Cancel as anything else (§11.6).
	QString node_id;
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value() == v) { node_id = it.key(); break; }
	m_capture_job = m_downloads->adopt(m_capture_src, v->url(), node_id);
	m_capture_src->began(m_capture_job, m_capture_path);

	m_capture_last   = 0;
	m_capture_warned = false;
	m_capture_clock.start();
	if (!m_capture_timer) {
		m_capture_timer = new QTimer(this);
		m_capture_timer->setInterval(500);
		connect(m_capture_timer, &QTimer::timeout, this, &main_window::poll_capture);
	}
	m_capture_timer->start();

	v->reload();
}

void main_window::poll_capture() {
	if (m_capture_url.isEmpty() || !m_local_proxy)
		return;
	const qint64 got  = m_local_proxy->captured_bytes(m_capture_url);
	const qint64 ms   = qMax<qint64>(1, m_capture_clock.elapsed());
	const QString size = QLocale().formattedDataSize(got);

	// On the action itself, so the count is visible wherever the menu is, not
	// only while a transient status message happens to be showing.
	m_capture_action->setText(QString("&Capture Playing Video — %1").arg(size));

	if (got > m_capture_last) {
		m_capture_last   = got;
		m_capture_warned = false;
		const QString rate = QLocale().formattedDataSize(got * 1000 / ms) + "/s";
		if (m_capture_src)
			m_capture_src->progressed_to(m_capture_job, got, rate);
		m_status->showMessage(QString("Capturing %1 (%2)").arg(size, rate));
		return;
	}

	// Nothing arriving. Say why, once, rather than letting it look like work.
	if (!m_capture_warned && m_capture_clock.elapsed() > 12000) {
		m_capture_warned = true;
		m_status->showMessage(
			got == 0
			    ? QString("Capturing, but nothing has arrived — press play. If "
			               "the page is already playing, it may not use Media "
			               "Source, and nothing here can record it.")
			    : QString("Capture paused at %1 — the page has stopped feeding "
			               "its player.").arg(size),
			15000);
	}
}

void main_window::find_media_with_ytdlp() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first.", 5000);
		return;
	}
	if (!m_ytdlp)
		m_ytdlp = new ytdlp_resolver(this);
	if (!m_ytdlp->available()) {
		m_status->showMessage(m_ytdlp->description(), 8000);
		return;
	}
	if (m_ytdlp->busy()) {
		m_status->showMessage("Still looking…", 3000);
		return;
	}

	const QUrl page = v->url();
	const QString host = page.host();
	m_status->showMessage(QString("Asking yt-dlp about %1…").arg(host));

	// One-shot: these are reconnected per request so a later answer cannot
	// arrive against a page the user has since left.
	m_ytdlp->disconnect(this);
	connect(m_ytdlp, &ytdlp_resolver::resolved, this,
	         [this, host](const resolved_media &m) {
		int added = 0;
		for (const media_format &f : m.formats) {
			if (!f.has_video && !f.has_audio)
				continue;
			media_item item;
			// Trust yt-dlp's protocol rather than re-guessing from the URL —
			// knowing this authoritatively is the entire point of asking it.
			item.kind = (f.protocol.startsWith("m3u8")) ? media_kind::hls
			          : (f.protocol.startsWith("http_dash")) ? media_kind::dash
			                                                 : media_kind::direct;
			item.url       = f.url;
			item.site_host = host;
			item.label     = QString("%1 %2%3")
			                     .arg(m.title.isEmpty() ? host : m.title,
			                          f.height ? QString::number(f.height) + "p"
			                                   : f.ext,
			                          f.note.isEmpty() ? QString()
			                                           : " · " + f.note);
			m_media->add_item(host, item);
			++added;
		}
		m_status->showMessage(
			added ? QString("yt-dlp (%1): %2 stream(s) found.")
			            .arg(m.extractor).arg(added)
			      : QString("yt-dlp found nothing playable."), 8000);
		if (added)
			open_media();
	}, Qt::SingleShotConnection);
	connect(m_ytdlp, &ytdlp_resolver::failed, this, [this](const QString &e) {
		// yt-dlp's own words: "Unsupported URL" is the answer that matters,
		// and it is exactly the case §11.6's tap exists for.
		m_status->showMessage("yt-dlp: " + e, 10000);
	}, Qt::SingleShotConnection);

	m_ytdlp->resolve(page);
}

void main_window::open_settings() {
	QString ignored;
	choose_ai(&ignored);          // make sure the providers exist to configure
	settings_dialog dlg(m_players, m_downloads, m_torrents, m_local_ai,
	                     m_external_ai, m_policy, m_filters, m_filters_path,
	                     m_consent, m_site_rules_path, this);
	dlg.exec();
	// Global defaults may have moved, and every live view was configured from
	// the old ones. Re-apply rather than wait for the next navigation, or the
	// setting appears not to have taken until the page is reloaded.
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value())
			apply_policy(it.value(), it.value()->url().host());
}

void main_window::open_downloads() {
	if (!m_downloads_ui)
		m_downloads_ui = new downloads_dialog(m_downloads, m_players,
		                                      m_local_proxy, this);
	m_downloads_ui->show();
	m_downloads_ui->raise();
	m_downloads_ui->activateWindow();
}

void main_window::confirm_public_download(const QString &source_id,
                                           const QString &note, int job_id) {
	download_source *src = m_downloads->source_by_id(source_id);
	const QString name = src ? src->display_name() : source_id;

	QMessageBox box(this);
	box.setIcon(QMessageBox::Warning);
	box.setWindowTitle(name + " is not a private download");
	box.setText(note);
	box.setInformativeText(
		"Continue only if that is acceptable for what you are downloading. "
		"This choice is remembered for the rest of this session.");
	box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
	box.setDefaultButton(QMessageBox::No);

	if (box.exec() == QMessageBox::Yes) {
		m_downloads->set_consent(source_id, true);
	} else {
		m_downloads->cancel(job_id);
		m_status->showMessage("Download cancelled.", 5000);
	}
}

void main_window::go_back() {
	if (web_view_backend *v = current_view())
		v->back();
}

void main_window::go_forward() {
	if (web_view_backend *v = current_view())
		v->forward();
}

void main_window::reload_page() {
	if (web_view_backend *v = current_view())
		v->reload();
}

void main_window::mark_dirty() {
	m_save_timer->start();  // debounced flush_tree()
}

void main_window::flush_tree() {
	if (!m_tree_path.isEmpty())
		m_model->save(m_tree_path);
}

void main_window::update_address(const QString &url) {
	m_address->setText(url);
	if (m_status)
		m_status->showMessage(url.isEmpty() ? QStringLiteral("Ready") : url);
}

void main_window::apply_policy(web_view_backend *view, const QString &host) {
	if (!view)
		return;
	using F = policy::feature;
	view_settings s;
	s.javascript = m_policy->is_allowed(F::javascript, host);
	s.images     = m_policy->is_allowed(F::images, host);
	s.autoplay   = m_policy->is_allowed(F::autoplay, host);
	s.popups     = m_policy->is_allowed(F::popups, host);
	view->apply_settings(s);
}

void main_window::open_site_controls() {
	if (!m_policy_dialog) {
		m_policy_dialog = new site_policy_dialog(m_policy, this);
		connect(m_policy_dialog, &site_policy_dialog::policy_changed,
		         this, &main_window::on_policy_changed);
	}
	QString host;
	if (web_view_backend *v = current_view())
		host = v->url().host();
	m_policy_dialog->set_host(host);
	m_policy_dialog->move(m_address->mapToGlobal(QPoint(0, m_address->height() + 2)));
	m_policy_dialog->show();
	m_policy_dialog->raise();
}

void main_window::on_policy_changed() {
	if (!m_policy_path.isEmpty())
		m_policy->save(m_policy_path);
	if (web_view_backend *v = current_view()) {
		apply_policy(v, v->url().host());
		v->reload();
	}
}

void main_window::closeEvent(QCloseEvent *event) {
	// Leave kiosk first, so the presented view is back in the stack and can be
	// suspended with the rest.
	if (m_kiosk && m_kiosk->active())
		m_kiosk->exit();

	// Persist live tabs as suspended blobs so they restore next launch,
	// then write the tree structure.
	const QList<QString> live_ids = m_views_by_id.keys();
	for (const QString &id : live_ids)
		if (node *n = m_model->node_by_id(id))
			suspend_node(n);
	if (!m_tree_path.isEmpty())
		m_model->save(m_tree_path);
	if (!m_policy_path.isEmpty())
		m_policy->save(m_policy_path);
	QWidget::closeEvent(event);
}
