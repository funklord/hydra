#pragma once

#include "tree_diff.h"   // for the sec 9.4 undo snapshot
#include "site_extractor.h"
#include "site_rules.h"
#include "session_mirror.h"   // for the mirror and imported_tab

#include <QWidget>
#include <QHash>
#include <QElapsedTimer>
#include <QUrl>
#include <QSet>
#include <QStringList>
#include <QString>

class annoyance_log;
class tab_tree_view;
class QSplitter;
class QStackedWidget;
class QLineEdit;
class QComboBox;
class QLabel;
class QProgressBar;
class find_bar;
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
// chrome-less web views on the right (architecture doc sec 6). Tabs follow a
// lifecycle -- unopened -> open (live view) -> suspended (history blob) -- with a
// small cap on how many views stay live at once (sec 5.4).
//
// A plain QWidget rather than a QMainWindow, stacking the classic desktop
// furniture itself in one QVBoxLayout: menu bar, toolbar, splitter, status bar.
// QMenuBar and QStatusBar are ordinary widgets and work fine outside a
// QMainWindow, so laying them out directly costs nothing and keeps the window's
// structure explicit -- which kiosk mode (sec 8) and the Android drawer layout
// (sec 19.3) both need to rearrange later.
class main_window : public QWidget {
	Q_OBJECT
public:
	// The factory and policy engine are injected rather than constructed here:
	// that is what keeps this class free of any engine-specific type, and it
	// leaves main() as the single place naming a concrete backend
	// (architecture doc sec 19.2). Both must outlive the window.
	main_window(web_view_factory *factory, policy_engine *policy,
	             request_filter *filter, QWidget *parent = nullptr);
	~main_window() override;

	bool load_tree(const QString &path);

	// Open an address as a tab, for a url handed to us from outside: the
	// desktop entry declares `Exec=hydra %U` and registers http and https, so
	// every link clicked in another application arrives here as argv[1].
	//
	// Before this existed that argument was read as a *tree path*, which does
	// not exist, so the window came up empty and the link was silently
	// dropped -- a browser that installs as the default browser and then
	// discards what it is asked to open.
	void open_url(const QUrl &url);

	// Write everything that must survive the process, and suspend the live
	// views doing it. Called by `closeEvent` and by a shutdown signal, which
	// are the two ways this browser ends; see the definition for why one list
	// serves both. Not a checkpoint -- it tears the views down.
	// Whether the tab tree reached the disk. **The tree is the document**, so
	// a caller that can still ask a question -- `closeEvent` -- has to know,
	// and one that cannot -- the signal handler, mid-logout -- has at least
	// something to log. Everything else it writes is recoverable state; the
	// tree is not.
	bool save_everything();

protected:
	void closeEvent(QCloseEvent *event) override;

	// **The view's own state, which nothing used to persist.** `closeEvent`
	// wrote the model and the policy and stopped there, so which folders were
	// open, which tab was in front and where the window sat were all discarded
	// every time -- and `load_tree` then called `expandAll()`, which would have
	// flattened them even if they had been kept.
	void save_view_state() const;
	void restore_view_state();
	void resizeEvent(QResizeEvent *event) override;
	void moveEvent(QMoveEvent *event) override;
	bool event(QEvent *e) override;   // routes status tips to the status bar

private slots:
	void on_tree_activated(const QModelIndex &proxy_index);
	void open_url_externally(const QUrl &url);
	void forget_subtree(node *n);
	// Put the password key back to its resting state: present, greyed, and
	// saying why. Shared by creation and by every navigation.
	void reset_key_action();
	void save_tree_soon();
	// The view state's equivalent, on a timer of its own. See the definition
	// for why it is not the same timer as the tree's.
	void save_view_soon();
	// And the tabs' navigation histories, which are the third thing a crash
	// used to take. `flush_blobs` writes only the tabs that have navigated
	// since it last ran, which is what `m_blobs_dirty` is for.
	void save_blobs_soon(const QString &id);
	void flush_blobs();
	// The imported back/forward records, which live beside the tree file
	// rather than in it. Both walk the whole tree from the root when called
	// with no argument.
	void persist_histories(node *from = nullptr);
	void restore_histories(node *from = nullptr);
	void new_tab();
	void new_folder();
	node *selected_parent() const;
	node *selected_node() const;
	void import_firefox_tabs();
	void import_chromium_tabs();
	void show_mirror_tabs(const QString &source, const QString &label,
	                       const QList<session_import::imported_tab> &tabs,
	                       bool from_poll);
	void on_sort_mode_changed(int combo_index);
	void on_search_changed(const QString &text);
	void navigate_to_address();
	// Drop the caches this window keeps that no backend knows about. Called
	// whenever browsing data is cleared, from the settings page or from
	// kiosk mode's own forgetting.
	void forget_shell_caches();
#ifdef Q_OS_ANDROID
	// Open a url another application handed this one, if there is one waiting.
	void open_handed_url();
#endif
	// Downloads that are publicly observable (sec 11.4) get an explanation before
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
	// The one place that resolves which backend to use (sec 9.1). Returns null
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
	// One click, at the moment something got through. See annoyance_log.h for
	// why this is worth a toolbar slot of its own rather than a menu item.
	void report_annoyance();
	// The other half: did the rules just applied break this page? Reachable
	// only while there is something to confirm. See main_window.cpp.
	void confirm_rules();
	void offer_confirmation(const QStringList &added, const QString &host);
	void undo_reorganize();
	void on_media_found(const QString &site_host, int count);

private:
	QMenuBar *build_menu_bar();
	// Report a write that did not happen. Every persistent writer here returns
	// whether it succeeded and every caller used to drop the answer, so a full
	// disk or an unwritable profile lost the change in silence. Not a slot: it
	// is called from the handlers that save, and returns what it was told so a
	// caller can still branch on it.
	bool saved_or_said(bool ok, const QString &what);
	// **Give up the path to a file that is there and would not load.**
	//
	// Every store in this window loads a file at startup and saves it back
	// later, and each of them answers "there is nothing here yet" and "I could
	// not read what is here" with the same `false`. The first is an ordinary
	// first run. The second, treated as the first, means the store sits empty
	// and the next save writes that emptiness over the file -- which is how a
	// read failure becomes data loss.
	//
	// So the caller decides, using the one fact the store does not have: does
	// the file exist. When it does and the load refused, the path is cleared
	// and the writers guarded by `isEmpty()` -- all of them -- leave it alone
	// for the session. Returns whether the path survived.
	bool keep_or_disown(bool loaded, QString *path, const QString &what);
	void update_status();
	// Tell the page-scoped bridges which site is on screen. Called whenever that
	// can change: a navigation in the current view, and a switch to another one.
	// Grey the navigation buttons to match what the page can actually do.
	void update_navigation();

	// Put the page in the window title, so a task switcher says which one.
	void update_window_title();

	// Show or hide the loading bar, and say so when a page does not arrive.
	void open_find();

	// Page zoom: step through a fixed ladder, and remember it per tab.
	void step_zoom(int direction);          // -1, 0 to reset, +1
	void apply_zoom(web_view_backend *view, const QString &node_id);

	QHash<QString, double> m_zoom;          // node id -> factor, 1.0 omitted

	// Show where a hovered link would go, or clear it when the pointer leaves.
	// Public so a test can drive the presentation without an engine: what is
	// worth checking is the eliding and the clearing, not Qt's own signal.
public:
	void show_link_target(const QUrl &url);

	// A page whose renderer died. Public for the same reason as the one above:
	// the wiring is one line and the part worth checking is what gets said.
	void report_render_crash(const QString &host);

	// A certificate was refused. Public for the reason the others are: the
	// wiring is one line and the wording is the part that matters.
	void report_certificate_rejected(const QUrl &url, const QString &reason);

	// A page asked for another window. Returns the node it made, or nullptr if
	// the request was refused -- public so a driver can ask for one without an
	// engine, since the decision is the part worth checking.
	// `adopt`, when given, is filled with the backend that should take over
	// the engine's pending window request -- see `new_window_requested`. The
	// view is then created *without* loading anything, because the request
	// carries the navigation and doing it twice would replay a one-time OAuth
	// url.
	node *open_new_window(const QUrl &url, bool user_initiated,
	                       web_view_backend **adopt = nullptr);
	void view_page_source();
	void present_fullscreen(web_view_backend *view, bool on);
	node *open_child_tab(node *parent, const QUrl &url);
	void  toggle_lock(node *n);
	bool  allow_navigation(web_view_backend *view, const QUrl &url,
	                        bool in_main_frame, bool user_initiated);

private:
	// **Everything the chrome says about the page in front of you.** Five
	// separate pieces of state -- the title, the navigation buttons, the
	// loading bar, the find count and the link target -- each needed hooking
	// into the same four places, and the window title was already added to
	// three of the four, going stale on every tab switch until that was found.
	// One call, so the sixth is right by construction.
	void page_changed();
	bool m_link_shown = false;
	// Which url a certificate was refused for, so the generic failure message
	// that follows can be suppressed *for that url only*. Held as identity
	// rather than as a flag: a flag needs clearing, every event available to
	// clear it can fail to fire on a failing load, and it then silences the
	// next failure instead of its own.
	QUrl m_cert_url;
	// While a load runs, the Reload button is a Stop button.
	void set_loading(bool loading);
	bool m_loading = false;

	void on_load_progress(int percent);
	void on_load_finished(bool ok);

	void sync_page_context();          // refresh the status bar's permanent counts
	web_view_backend *current_view() const;
	// `load_now` is false only when something else is about to drive the
	// navigation: a window request adopted through `openIn` performs its own,
	// and a page loaded here first would be thrown away or, worse, would spend
	// a single-use url.
	void open_node(node *n, bool load_now = true);            // create/restore a live view and show it
	void suspend_node(node *n);         // serialize + tear down the live view
	void touch_lru(const QString &id);
	void enforce_live_cap(const QString &keep_id);
	void mark_dirty();
	void update_address(const QString &url);
	void apply_policy(web_view_backend *view, const QString &host);

	// sec 19.3's adaptive layout. A horizontal splitter is right on a desktop and
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

	// Android's Back button, answered the way a browser has to answer it.
	//
	// **Nothing handled it, and Qt's default is to finish the activity** -- so
	// Back closed the whole browser, from anywhere, including with a menu open.
	// Reported from the phone as menus that cannot be dismissed without losing
	// the app.
	//
	// Returns true when it was consumed. The order is the one every mobile
	// browser uses and each step is the least destructive thing still available:
	// close what is in front of the page, then close the drawer, then go back in
	// history, and only then let Android have it.
	bool handle_back();

protected:
	// Installed on the application on Android, so it sees the Back key even
	// while a popup holds the grab -- which is exactly the case that was
	// broken, and a filter on this window alone would never have seen it.
	bool eventFilter(QObject *watched, QEvent *event) override;

public:

	static constexpr int k_drawer_threshold = 620;   // logical px

	static constexpr int k_max_live_views = 4;

	tab_tree_model  *m_model = nullptr;
	tree_sort_proxy *m_proxy = nullptr;
	tab_tree_view   *m_tree  = nullptr;
	QWidget         *m_sidebar = nullptr;      // tree + search + sort
	QSplitter       *m_splitter = nullptr;
	QAction         *m_drawer_action = nullptr;
	class QPropertyAnimation *m_drawer_anim = nullptr;
	bool             m_drawer_mode = false;    // narrow: sidebar is an overlay
	// The empty-content message. It has to change with the layout: in drawer
	// mode the tree it points at is not on screen.
	QLabel          *m_placeholder = nullptr;
	// What the empty page says, which depends on the layout mode and on
	// whether the drawer is covering it. One place decides, because the two
	// callers used to disagree about whether the drawer was open.
	void refresh_placeholder_text();
	bool             m_drawer_open = false;
	QStackedWidget  *m_stack = nullptr;

	QLineEdit       *m_address  = nullptr;
	QComboBox       *m_sort_box = nullptr;
	QString          m_view_path;   // view.ini, beside the tree and the policy
	QLineEdit       *m_search   = nullptr;

	QStatusBar      *m_status     = nullptr;
	QLabel          *m_tab_counts = nullptr;   // permanent widget, right side
	QProgressBar    *m_progress   = nullptr;   // beside it, only while loading
	find_bar        *m_find       = nullptr;   // above the status bar, hidden

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
	// **Named once, because it was spelled out twice.** The load in the
	// constructor and the save in the extractor dialog each built this path
	// from `AppDataLocation` independently, so there was nothing for a guard
	// to clear -- and two copies of a path is two things to be wrong.
	QString             m_extractors_path;
	int                 m_capture_job   = 0;
	local_proxy        *m_local_proxy   = nullptr;
	element_picker     *m_picker        = nullptr;
	filter_signals     *m_signals       = nullptr;
	annoyance_log      *m_annoyances    = nullptr;
	QString             m_annoyances_path;
	QAction            *m_annoyed_action = nullptr;
	QAction            *m_annoyed_menu_action = nullptr;
	QAction            *m_confirm_action = nullptr;
	// Rule texts added by the last accepted proposal, and where. Held until
	// somebody says whether the page still works, because that is the only
	// window in which "undo" means anything specific.
	QStringList         m_unconfirmed_rules;
	QString             m_unconfirmed_host;
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
	QAction            *m_back_action   = nullptr;
	QAction            *m_fwd_action    = nullptr;
	QAction            *m_reload_action = nullptr;
	QAction            *m_print_action  = nullptr;
	QAction            *m_source_action = nullptr;
	QAction            *m_desktop_site_action = nullptr;
	bool                m_page_fullscreen = false;
	web_view_backend   *m_kiosk_view      = nullptr;
	QAction            *m_key_action    = nullptr;
	session_mirror     *m_fx_mirror     = nullptr;
	session_mirror     *m_cr_mirror     = nullptr;
	QString             m_filters_path;
	QAction            *m_kiosk_action  = nullptr;
	QAction            *m_undo_action   = nullptr;
	tree_snapshot       m_undo;
	QTimer             *m_save_timer    = nullptr;
	// Whether the last debounced tree write failed, so a disk that has filled
	// says so once instead of once per keystroke.
	bool                m_tree_save_failed = false;
	QTimer             *m_view_timer    = nullptr;
	QTimer             *m_blob_timer    = nullptr;
	// Node ids whose history has moved since the last blob flush. Ids rather
	// than views, because a view can be suspended between the navigation and
	// the timer firing, and an id survives that -- suspending writes the blob
	// itself, so the entry is simply found to have no live view and dropped.
	QSet<QString>       m_blobs_dirty;
	QString             m_tree_path;
	QString             m_policy_path;

	// Answers to permission prompts, for this run only: "host\nfeature" ->
	// granted. **Not persistence -- the opposite of it.** A site set to `ask`
	// is asked once, and a page that asks again (a reload, a second call, a
	// frame that retries in a loop) is answered from here without anybody
	// seeing a second dialog. Nothing is written down, so it is gone when the
	// browser is, which is what makes the prompt's "remember" checkbox able to
	// default to unchecked without condemning anyone to answering forever.
	QHash<QString, bool> m_session_permissions;

	QHash<QString, web_view_backend *> m_views_by_id;  // node id -> live view
	QStringList                        m_lru;          // most-recent id at front
};
