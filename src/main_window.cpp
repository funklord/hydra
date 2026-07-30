// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "tab_tree_model.h"
#include "tree_sort_proxy.h"
#include "state_store.h"
#include "policy_engine.h"
#include "site_policy_dialog.h"
#include "web_view_backend.h"
#include "web_view_factory.h"
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
#include "local_proxy.h"
#include "filter_signals.h"
#include "filter_list.h"
#include "filter_dialog.h"
#include "keepass_bridge.h"
#include "autofill_controller.h"
#include "autofill_script.h"
#include "element_picker.h"
#include "picker_script.h"
#include "policy.h"
#include "node.h"

#include <QSplitter>
#include <QTreeView>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QToolBar>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QActionGroup>
#include <QAction>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QDataStream>
#include <QCloseEvent>

main_window::main_window(web_view_factory *factory, policy_engine *policy,
                          request_filter *filter, QWidget *parent)
	: QWidget(parent), m_factory(factory), m_policy(policy) {
	// The two interceptor consumers (architecture doc §10): both observe the
	// same request stream the blocker already rides, rather than adding a
	// second sensor.
	m_media   = new media_detector(this);
	m_signals = new filter_signals(this);
	if (filter) {
		filter->add_observer(m_media);
		filter->add_observer(m_signals);
	}
	m_keepass  = new keepass_bridge(this);
	m_autofill = new autofill_controller(m_keepass, m_policy, this);
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
	// The manager has no transport of its own (§11.4); give it one. Sources are
	// tried in order, so adding a torrent source later is one line here.
	m_downloads->add_source(new http_download_source);
	// BitTorrent is a first-class source, not a side feature (§11.4). It is
	// only present when the build found libtorrent; there is no degraded mode.
	if (torrent_download_source::available())
		m_downloads->add_source(new torrent_download_source);
	// The §11.4 obligation that replaces the VPN we deliberately do not ship:
	// a source whose participation is publicly observable cannot start until
	// the user has been told what it does. The manager holds the job; this is
	// where the telling happens.
	connect(m_downloads, &download_manager::consent_required, this,
	         &main_window::confirm_public_download);
	// The local proxy is optional: if it cannot listen, Watch still works and
	// simply hands over the raw URL (§10 — it is an upgrade tier, not a
	// prerequisite).
	m_local_proxy = new local_proxy(this);
	m_local_proxy->start();
	m_filters   = new filter_list;
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

	QAction *back_act   = bar->addAction("◀");
	QAction *fwd_act    = bar->addAction("▶");
	QAction *reload_act = bar->addAction("↻");
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

	QAction *shield_act = bar->addAction("Shield");
	shield_act->setToolTip("Site controls");
	connect(shield_act, &QAction::triggered, this, &main_window::open_site_controls);

	bar->addSeparator();
	bar->addWidget(new QLabel(" Sort: ", this));
	m_sort_box = new QComboBox(this);
	m_sort_box->addItem("Tree order");
	m_sort_box->addItem("Title A–Z");
	m_sort_box->addItem("Newest");
	m_sort_box->addItem("Recently seen");
	connect(m_sort_box, &QComboBox::currentIndexChanged,
	         this, &main_window::on_sort_mode_changed);
	bar->addWidget(m_sort_box);

	m_search = new QLineEdit(this);
	m_search->setPlaceholderText("Search tree");
	m_search->setClearButtonEnabled(true);
	m_search->setMaximumWidth(200);
	connect(m_search, &QLineEdit::textChanged, this, &main_window::on_search_changed);
	bar->addWidget(m_search);

	outer->addWidget(bar);

	// --- Central splitter ----------------------------------------------
	m_tree = new QTreeView(this);
	m_tree->setModel(m_proxy);
	m_tree->setHeaderHidden(true);
	m_tree->setUniformRowHeights(true);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	m_tree->expandAll();
	connect(m_tree, &QTreeView::activated, this, &main_window::on_tree_activated);
	connect(m_tree, &QTreeView::clicked,   this, &main_window::on_tree_activated);
	connect(m_tree, &QTreeView::customContextMenuRequested,
	         this, &main_window::on_tree_context_menu);

	m_stack = new QStackedWidget(this);
	QLabel *placeholder = new QLabel("Select a tab from the tree", this);
	placeholder->setAlignment(Qt::AlignCenter);
	m_stack->addWidget(placeholder);

	QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
	splitter->addWidget(m_tree);
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
	tools_menu->addAction("&Site Controls…", this, &main_window::open_site_controls);
	QAction *kp = tools_menu->addAction("Connect to &KeePassXC…", this,
	                                     &main_window::toggle_password_manager);
	kp->setStatusTip(keepass_bridge::supported()
	                     ? "Pair with a running KeePassXC for autofill"
	                     : "Unavailable: built without libsodium");
	kp->setEnabled(keepass_bridge::supported());
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

	kiosk_config c = m_kiosk->config();
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
	// Local-first (architecture doc §9.1): if a local model answers, it handles
	// the request and nothing leaves the machine. The external provider is used
	// only when there is no local one and a credential exists — and the dialog
	// gates it behind review-before-send either way.
	if (!m_local_ai) {
		m_local_ai    = new ollama_provider(this);
		m_external_ai = new claude_provider(this);
		m_local_ai->probe();
	}

	ai_provider *chosen = nullptr;
	if (m_local_ai->available())
		chosen = m_local_ai;
	else if (m_external_ai->available())
		chosen = m_external_ai;

	if (!chosen) {
		m_status->showMessage("No AI provider: start Ollama locally, or set "
		                      "ANTHROPIC_API_KEY.", 6000);
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
	web_view_backend *v = current_view();
	if (!v || v->url().host() != site_host)
		return;   // detection on a background tab; don't retitle this one
	m_media_action->setVisible(count > 0);
	m_media_action->setText(QString("Media (%1)").arg(count));
}

void main_window::open_media() {
	web_view_backend *v = current_view();
	if (!v)
		return;
	QString node_id;
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value() == v) { node_id = it.key(); break; }

	// The context a CDN expects: the page that loaded the stream, and this
	// browser's own User-Agent.
	stream_context ctx;
	ctx.referer = v->url().toString();
	media_dialog dlg(m_media, m_players, m_downloads, m_local_proxy, this);
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
	if (!m_local_ai) {
		m_local_ai    = new ollama_provider(this);
		m_external_ai = new claude_provider(this);
		m_local_ai->probe();
	}
	ai_provider *chosen = m_local_ai->available()    ? static_cast<ai_provider *>(m_local_ai)
	                    : m_external_ai->available() ? static_cast<ai_provider *>(m_external_ai)
	                                                 : nullptr;
	if (!chosen) {
		m_status->showMessage("No AI provider: start Ollama locally, or set "
		                      "ANTHROPIC_API_KEY.", 6000);
		return;
	}

	filter_dialog dlg(m_signals, m_filters, chosen, v->url().host(),
	                   m_picker->last(), this);
	if (dlg.exec() == QDialog::Accepted && !m_filters_path.isEmpty())
		m_filters->save(m_filters_path);
}

void main_window::toggle_password_manager() {
	if (!keepass_bridge::supported()) {
		m_status->showMessage("Built without libsodium — install libsodium-dev "
		                      "and rebuild.", 8000);
		return;
	}
	if (!m_keepass->connected()) {
		connect(m_keepass, &keepass_bridge::ready, this, [this] {
			// A stored pairing is tested rather than re-created, so the user is
			// not asked to confirm a new connection on every launch (§13.1).
			if (m_keepass->associated())
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

	m_policy_path = dir + "/policy.json";
	m_policy->load(m_policy_path);   // no-op if the file doesn't exist yet

	// The AI/user-authored filter list lives beside the rest, kept separate
	// from any imported EasyList so upstream updates cannot clobber it (§12.5).
	m_filters_path = dir + "/filters-ai.txt";
	m_filters->load(m_filters_path);

	const bool ok = m_model->load(path);
	m_tree->expandAll();
	return ok;
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

		connect(view, &web_view_backend::url_changed, this, [this, view](const QUrl &u) {
			apply_policy(view, u.host());
			if (view == current_view())
				m_autofill->set_page_origin(
					u.adjusted(QUrl::RemovePath | QUrl::RemoveQuery |
					            QUrl::RemoveFragment).toString());
			if (view == current_view())
				update_address(u.toString());
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
	update_address(view->url().toString());
	touch_lru(n->id);
	enforce_live_cap(n->id);
	mark_dirty();
	update_status();
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
	if (!proxy_index.isValid())
		return;
	node *n = m_model->node_for_index(m_proxy->mapToSource(proxy_index));
	open_node(n);
}

void main_window::on_tree_context_menu(const QPoint &pos) {
	const QModelIndex proxy_index = m_tree->indexAt(pos);
	if (!proxy_index.isValid())
		return;
	node *n = m_model->node_for_index(m_proxy->mapToSource(proxy_index));
	if (!n || n->is_folder())
		return;

	QMenu menu(this);
	QAction *open_a = menu.addAction("Open");
	QAction *sus_a  = menu.addAction("Suspend");
	sus_a->setEnabled(m_views_by_id.contains(n->id));

	QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
	if (chosen == open_a)
		open_node(n);
	else if (chosen == sus_a)
		suspend_node(n);
}

void main_window::on_sort_mode_changed(int combo_index) {
	using SM = tree_sort_proxy::sort_mode;
	static const SM modes[] = { SM::tree_order, SM::title_asc,
	                            SM::newest_created, SM::recently_seen };
	if (combo_index >= 0 && combo_index < 4)
		m_proxy->set_sort_mode(modes[combo_index]);
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
	m_status->showMessage(QString("Queued: %1").arg(url.toString()), 5000);
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
