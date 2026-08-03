// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tree_diff.h"   // for the §9.4 undo snapshot
#include "site_extractor.h"
#include "site_rules.h"

#include <QWidget>
#include <QHash>
#include <QElapsedTimer>
#include <QUrl>
#include <QSet>
#include <QStringList>
#include <QString>

class QTreeView;
class QSplitter;
class QStackedWidget;
class QLineEdit;
class QComboBox;
class QLabel;
class QMenuBar;
class QStatusBar;
class QAction;
class QTimer;
class QPoint;
class QCloseEvent;
class QEvent;
class tab_tree_model;
class tree_sort_proxy;
class state_store;
class policy_engine;
class web_view_backend;
class web_view_factory;
class site_policy_dialog;
class kiosk_controller;
class ai_provider;
class media_detector;
class player_launcher;
class download_manager;
class downloads_dialog;
class torrent_download_source;
class ytdlp_resolver;
class mse_tap;
class capture_source;
class extractor_signals;
class filter_signals;
class filter_list;
class cosmetic_filters;
class request_filter;
class keepass_bridge;
class autofill_controller;
class consent_blocker;
class antiadblock_watch;
class local_proxy;
class element_picker;
class ollama_provider;
class claude_provider;
struct node;

// The shell: a splitter with the tab tree on the left and a stack of
// chrome-less web views on the right (architecture doc §6). Tabs follow a
// lifecycle — unopened -> open (live view) -> suspended (history blob) — with a
// small cap on how many views stay live at once (§5.4).
//
// A plain QWidget rather than a QMainWindow, stacking the classic desktop
// furniture itself in one QVBoxLayout: menu bar, toolbar, splitter, status bar.
// QMenuBar and QStatusBar are ordinary widgets and work fine outside a
// QMainWindow, so laying them out directly costs nothing and keeps the window's
// structure explicit — which kiosk mode (§8) and the Android drawer layout
// (§19.3) both need to rearrange later.
class main_window : public QWidget {
	Q_OBJECT
public:
	// The factory and policy engine are injected rather than constructed here:
	// that is what keeps this class free of any engine-specific type, and it
	// leaves main() as the single place naming a concrete backend
	// (architecture doc §19.2). Both must outlive the window.
	main_window(web_view_factory *factory, policy_engine *policy,
	             request_filter *filter, QWidget *parent = nullptr);
	~main_window() override;

	bool load_tree(const QString &path);

protected:
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	bool event(QEvent *e) override;   // routes status tips to the status bar

private slots:
	void on_tree_activated(const QModelIndex &proxy_index);
	void on_tree_context_menu(const QPoint &pos);
	void on_sort_mode_changed(int combo_index);
	void on_search_changed(const QString &text);
	void navigate_to_address();
	// Downloads that are publicly observable (§11.4) get an explanation before
	// they start, never after.
	void confirm_public_download(const QString &source_id, const QString &note,
	                              int job_id);
	void start_download(const QUrl &url);
	void open_downloads();
	void find_media_with_ytdlp();
	void toggle_capture();
	void learn_this_site();
	// Run a stored extractor for this host, if there is one, and file what it
	// finds with everything else the page offers.
	int  apply_extractor(const QString &host, const QUrl &page);
	void poll_capture();
	// One place decides what the media badge says, since two sources feed it.
	void refresh_media_affordance(const QString &site_host);
	void open_settings();
	// The one place that resolves which backend to use (§9.1). Returns null
	// when the user's choice cannot be satisfied, with `why` set.
	ai_provider *choose_ai(QString *why);
	void go_back();
	void go_forward();
	void reload_page();
	void flush_tree();
	void open_site_controls();
	void on_policy_changed();
	void on_about();
	void toggle_kiosk();
	void open_reorganizer();
	void open_media();
	void open_filter_evolution();
	void toggle_password_manager();
	void forget_keepass_pairing();
	void start_element_picker();
	void open_site_rules();
	void undo_reorganize();
	void on_media_found(const QString &site_host, int count);

private:
	QMenuBar *build_menu_bar();
	void update_status();
	// Tell the page-scoped bridges which site is on screen. Called whenever that
	// can change: a navigation in the current view, and a switch to another one.
	void sync_page_context();          // refresh the status bar's permanent counts
	web_view_backend *current_view() const;
	void open_node(node *n);            // create/restore a live view and show it
	void suspend_node(node *n);         // serialize + tear down the live view
	void touch_lru(const QString &id);
	void enforce_live_cap(const QString &keep_id);
	void mark_dirty();
	void update_address(const QString &url);
	void apply_policy(web_view_backend *view, const QString &host);

	// §19.3's adaptive layout. A horizontal splitter is right on a desktop and
	// unusable on a portrait phone, where it leaves the page a strip too narrow
	// to read -- measured on a device before this existed. Narrow windows get the
	// tree as a drawer that slides over the content instead; wide ones keep the
	// splitter exactly as it was.
	//
	// Driven by the window's own width rather than by the platform, because the
	// thing that makes a splitter wrong is the aspect ratio and a desktop window
	// dragged narrow has the same problem.
	void update_layout_mode();
	void set_drawer_open(bool open, bool animate = true);

	static constexpr int k_drawer_threshold = 620;   // logical px

	static constexpr int k_max_live_views = 4;

	tab_tree_model  *m_model = nullptr;
	tree_sort_proxy *m_proxy = nullptr;
	QTreeView       *m_tree  = nullptr;
	QWidget         *m_sidebar = nullptr;      // tree + search + sort
	QSplitter       *m_splitter = nullptr;
	QAction         *m_drawer_action = nullptr;
	class QPropertyAnimation *m_drawer_anim = nullptr;
	bool             m_drawer_mode = false;    // narrow: sidebar is an overlay
	bool             m_drawer_open = false;
	QStackedWidget  *m_stack = nullptr;

	QLineEdit       *m_address  = nullptr;
	QComboBox       *m_sort_box = nullptr;
	QLineEdit       *m_search   = nullptr;

	QStatusBar      *m_status     = nullptr;
	QLabel          *m_tab_counts = nullptr;   // permanent widget, right side

	web_view_factory   *m_factory       = nullptr;   // injected, not owned
	state_store        *m_state         = nullptr;
	policy_engine      *m_policy        = nullptr;   // injected, not owned
	site_policy_dialog *m_policy_dialog = nullptr;
	kiosk_controller   *m_kiosk         = nullptr;
	ollama_provider    *m_local_ai      = nullptr;
	claude_provider    *m_external_ai   = nullptr;
	media_detector     *m_media         = nullptr;
	player_launcher    *m_players       = nullptr;
	download_manager   *m_downloads     = nullptr;
	downloads_dialog   *m_downloads_ui  = nullptr;
	torrent_download_source *m_torrents = nullptr;
	ytdlp_resolver     *m_ytdlp          = nullptr;
	mse_tap            *m_mse            = nullptr;
	QAction            *m_capture_action = nullptr;
	QUrl                m_capture_url;      // the proxy endpoint, while capturing
	QString             m_capture_path;
	QTimer             *m_capture_timer = nullptr;
	QElapsedTimer       m_capture_clock;
	qint64              m_capture_last  = 0;
	bool                m_capture_warned = false;
	capture_source     *m_capture_src   = nullptr;
	extractor_signals  *m_ex_signals    = nullptr;
	extractor_store     m_extractors;
	int                 m_capture_job   = 0;
	local_proxy        *m_local_proxy   = nullptr;
	element_picker     *m_picker        = nullptr;
	filter_signals     *m_signals       = nullptr;
	filter_list        *m_filters       = nullptr;
	// Kept so the accepted rules can be handed to it once the list is loaded,
	// which happens after construction.
	request_filter     *m_filter        = nullptr;
	cosmetic_filters   *m_cosmetic      = nullptr;
	keepass_bridge     *m_keepass       = nullptr;
	autofill_controller *m_autofill     = nullptr;
	consent_blocker     *m_consent      = nullptr;
	antiadblock_watch   *m_antiadblock  = nullptr;
	// Sites already fixed this session, so a page that keeps checking cannot
	// put the browser in a reload loop.
	QSet<QString>        m_antiadblock_fixed;
	QString              m_site_rules_path;
	QAction            *m_media_action  = nullptr;
	QAction            *m_key_action    = nullptr;
	QString             m_filters_path;
	QAction            *m_kiosk_action  = nullptr;
	QAction            *m_undo_action   = nullptr;
	tree_snapshot       m_undo;
	QTimer             *m_save_timer    = nullptr;
	QString             m_tree_path;
	QString             m_policy_path;

	QHash<QString, web_view_backend *> m_views_by_id;  // node id -> live view
	QStringList                        m_lru;          // most-recent id at front
};
