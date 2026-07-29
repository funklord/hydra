// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "tab_tree_model.h"
#include "tree_sort_proxy.h"
#include "state_store.h"
#include "policy_engine.h"
#include "request_interceptor.h"
#include "site_policy_dialog.h"
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
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineHistory>
#include <QWebEngineSettings>
#include <QWebEngineCookieStore>

main_window::main_window(QWidget *parent)
	: QWidget(parent) {
	// One shared profile. The request interceptor, cookie filter, and download
	// handler attach here in later steps (architecture doc §6/§7/§10).
	m_profile = QWebEngineProfile::defaultProfile();

	// --- Security spine: policy_engine + interceptor + cookie filter ------
	m_policy      = new policy_engine(this);
	m_interceptor = new request_interceptor(m_policy, this);
	m_profile->setUrlRequestInterceptor(m_interceptor);

	policy_engine *pe = m_policy;
	m_profile->cookieStore()->setCookieFilter(
		[pe](const QWebEngineCookieStore::FilterRequest &r) {
			const QString site = r.firstPartyUrl.host();
			if (r.thirdParty &&
			    !pe->is_allowed(policy::feature::third_party_cookies, site))
				return false;
			return pe->is_allowed(policy::feature::cookies, site);
		});

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

	QMenu *tools_menu = menu->addMenu("&Tools");
	tools_menu->addAction("&Site Controls…", this, &main_window::open_site_controls);

	QMenu *help_menu = menu->addMenu("&Help");
	help_menu->addAction("&About", this, &main_window::on_about);

	return menu;
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

bool main_window::load_tree(const QString &path) {
	m_tree_path = path;
	const QString dir = QFileInfo(path).absolutePath();
	delete m_state;
	m_state = new state_store(dir + "/state");

	m_policy_path = dir + "/policy.json";
	m_policy->load(m_policy_path);   // no-op if the file doesn't exist yet

	const bool ok = m_model->load(path);
	m_tree->expandAll();
	return ok;
}

QWebEngineView *main_window::current_view() const {
	return qobject_cast<QWebEngineView *>(m_stack->currentWidget());
}

void main_window::open_node(node *n) {
	if (!n || n->url.isEmpty())
		return;  // folders and empty nodes have nothing to show

	n->last_seen = QDateTime::currentDateTime();

	QWebEngineView *view = m_views_by_id.value(n->id, nullptr);
	if (!view) {
		view = new QWebEngineView(this);
		QWebEnginePage *page = new QWebEnginePage(m_profile, view);
		view->setPage(page);
		m_views_by_id.insert(n->id, view);
		m_stack->addWidget(view);

		apply_policy(page, QUrl::fromUserInput(n->url).host());

		connect(view, &QWebEngineView::urlChanged, this, [this, view, page](const QUrl &u) {
			apply_policy(page, u.host());
			if (view == current_view())
				update_address(u.toString());
		});

		// Feature permissions (geo/cam/mic/notifications) decided from policy.
		// Note: featurePermissionRequested is deprecated in Qt 6.8+ but still
		// functional; migrate to QWebEnginePermission when targeting only 6.8+.
		connect(page, &QWebEnginePage::featurePermissionRequested, this,
		         [this, page](const QUrl &origin, QWebEnginePage::Feature f) {
			policy::feature pf;
			switch (f) {
				case QWebEnginePage::Geolocation:
					pf = policy::feature::geolocation; break;
				case QWebEnginePage::MediaAudioCapture:
					pf = policy::feature::microphone; break;
				case QWebEnginePage::MediaVideoCapture:
				case QWebEnginePage::MediaAudioVideoCapture:
					pf = policy::feature::camera; break;
				case QWebEnginePage::Notifications:
					pf = policy::feature::notifications; break;
				default:
					page->setFeaturePermission(origin, f,
						QWebEnginePage::PermissionDeniedByUser);
					return;
			}
			const bool grant = m_policy->is_allowed(pf, origin.host());
			page->setFeaturePermission(origin, f,
				grant ? QWebEnginePage::PermissionGrantedByUser
				      : QWebEnginePage::PermissionDeniedByUser);
		});

		if (n->type == node_type::suspended_tab && m_state && m_state->has_state(n->id)) {
			// Restore navigation history from the suspended blob.
			QByteArray blob = m_state->load(n->id);
			QDataStream ds(&blob, QIODevice::ReadOnly);
			ds >> *view->history();
			m_state->remove(n->id);
		} else {
			view->setUrl(QUrl::fromUserInput(n->url));
		}
	}

	n->type = node_type::open_tab;
	m_model->refresh_node(n);
	m_stack->setCurrentWidget(view);
	update_address(view->url().toString());
	touch_lru(n->id);
	enforce_live_cap(n->id);
	mark_dirty();
	update_status();
}

void main_window::suspend_node(node *n) {
	if (!n)
		return;
	QWebEngineView *view = m_views_by_id.value(n->id, nullptr);
	if (!view)
		return;

	// Serialize navigation history to a blob keyed by node id.
	QByteArray blob;
	{
		QDataStream ds(&blob, QIODevice::WriteOnly);
		ds << *view->history();
	}
	if (m_state)
		m_state->save(n->id, blob);

	if (view == current_view())
		m_stack->setCurrentIndex(0);  // back to placeholder
	m_stack->removeWidget(view);
	m_views_by_id.remove(n->id);
	m_lru.removeAll(n->id);
	view->deleteLater();

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
	if (QWebEngineView *view = current_view())
		view->setUrl(QUrl::fromUserInput(m_address->text()));
}

void main_window::go_back() {
	if (QWebEngineView *v = current_view())
		v->triggerPageAction(QWebEnginePage::Back);
}

void main_window::go_forward() {
	if (QWebEngineView *v = current_view())
		v->triggerPageAction(QWebEnginePage::Forward);
}

void main_window::reload_page() {
	if (QWebEngineView *v = current_view())
		v->triggerPageAction(QWebEnginePage::Reload);
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

void main_window::apply_policy(QWebEnginePage *page, const QString &host) {
	if (!page)
		return;
	QWebEngineSettings *s = page->settings();
	using F = policy::feature;
	s->setAttribute(QWebEngineSettings::JavascriptEnabled,
	                m_policy->is_allowed(F::javascript, host));
	s->setAttribute(QWebEngineSettings::AutoLoadImages,
	                m_policy->is_allowed(F::images, host));
	s->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture,
	                !m_policy->is_allowed(F::autoplay, host));
	s->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
	                m_policy->is_allowed(F::popups, host));
}

void main_window::open_site_controls() {
	if (!m_policy_dialog) {
		m_policy_dialog = new site_policy_dialog(m_policy, this);
		connect(m_policy_dialog, &site_policy_dialog::policy_changed,
		         this, &main_window::on_policy_changed);
	}
	QString host;
	if (QWebEngineView *v = current_view())
		host = v->url().host();
	m_policy_dialog->set_host(host);
	m_policy_dialog->move(m_address->mapToGlobal(QPoint(0, m_address->height() + 2)));
	m_policy_dialog->show();
	m_policy_dialog->raise();
}

void main_window::on_policy_changed() {
	if (!m_policy_path.isEmpty())
		m_policy->save(m_policy_path);
	if (QWebEngineView *v = current_view()) {
		apply_policy(v->page(), v->url().host());
		v->triggerPageAction(QWebEnginePage::Reload);
	}
}

void main_window::closeEvent(QCloseEvent *event) {
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
