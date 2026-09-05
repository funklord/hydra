#pragma once

#include "bridge_invoker.h"
#include "web_view_backend.h"
#include "web_view_factory.h"

#include <QHash>
#include <QUrl>

#include <functional>

class QLabel;
class request_filter;

// The Android side of the WebView seam -- **a System WebView, driven over JNI.**
//
// Pages load, links navigate, back returns, and the address bar follows along.
// The WebView itself lives outside Qt's widget tree: Qt for Widgets draws into
// its own surface and cannot host a native Android view, so `HydraWebView.java`
// adds one to the Activity *on top* of Qt's surface and this class keeps it
// glued to wherever the page-area widget is, in device pixels. The cost of that
// arrangement is that this view sits above everything Qt draws -- **including
// Qt's own dialogs**, which is not a caveat but a bug: tapping "Media" depressed
// the button and showed nothing, because the dialog opened behind the page.
//
// The view therefore hides while any dialog is visible, counted rather than
// inferred. The first attempt used `WindowBlocked`, which Qt sends to a window
// covered by a *modal* dialog -- and that missed the downloads dialog, which is
// shown rather than exec'd and came up with the page drawn through the middle of
// it. Counting needs no assumption about modality, and "none of the shell's
// windows are non-modal" was exactly the kind of claim that was false already.
//
// **A QLabel stands in when there is no WebView to talk to**, which happens when
// `HYDRA_ANDROID_WEBVIEW=0` is set or an APK was built without the `android/`
// package source directory. It says which, because a blank page that quietly
// means "turned off" is indistinguishable from one that means "broken".
//
// **Requests go through the same `request_filter` as the desktop.**
// `shouldInterceptRequest` asks it and returns an empty response for anything it
// blocks, so ad hosts, per-origin script rules and per-site image rules apply
// here without a line of Android-specific policy. Two things do not carry over,
// and both are limits of the platform rather than choices:
//
//   * **Referer cannot be stripped.** `shouldInterceptRequest` may replace a
//     response but not edit the outgoing request, so honouring it would mean
//     re-issuing every request from Java. `request_decision` is flags precisely
//     so a backend can honour the parts it supports; this one honours `block`.
//   * **The resource type is not reported**, so it is inferred from the `Accept`
//     header and the url by `kind_from_hints()`, which is shared and tested.
//
// **Content scripts run, and can call back.** There is no QWebChannel here, so
// `addJavascriptInterface` carries a two-method native object and a shim builds
// the same `window.hydraChannel(cb)` the desktop scripts are written against --
// they run unmodified. `bridge_invoker` does the marshalling, and is where the
// rules about what a page may call are written down.
//
// Two more platform limits, stated rather than papered over:
//
//   * **No isolated world.** Qt WebEngine can run a script in a world the page
//     cannot see; Android has one world, so an injected script shares globals
//     with the page and a hostile page can read or replace it. The bridges are
//     written on the assumption that every argument is hostile, which was
//     already true on the desktop and is merely load-bearing here.
//   * **Injection is at `onPageStarted`,** which is early but not guaranteed to
//     beat a page's own inline script. `addDocumentStartJavaScript` from
//     androidx.webkit is the real answer and is a dependency decision, not a
//     line of code.
//
// **The per-site settings are applied here too, and one of them is not.**
// `apply_settings()` maps four of `view_settings`' five fields onto
// `WebSettings` calls that mean what the desktop's `QWebEngineSettings`
// attributes mean, and the fifth -- scrollbars -- onto the View's own
// scrollbar flags. What is *not* set is `setSupportMultipleWindows`: it routes
// `window.open` to a `WebChromeClient.onCreateWindow` this class does not
// implement, and an unhandled one drops the request silently, so switching it
// on would make "popups allowed" mean "popups discarded". Left off, an allowed
// `window.open` navigates this same WebView -- not a new tab under the page
// that asked, but visibly something. `HydraWebView.applySettings` carries the
// detail.
//
// **File inputs open the system picker**, through Qt's own `QFileDialog` -- on
// Android that is the document picker, and what comes back is a `content:` url
// the WebView can read because the picker granted *this* app access to it. No
// storage permission is asked for and none is needed, which is the point of the
// Storage Access Framework.
//
// **Links that are not pages go where they go on the desktop.**
// `shouldOverrideUrlLoading` asks about every navigation and takes silence as
// consent, so anything `renders_as_page()` does not claim is handed to the
// shell's external-url handler -- the same one `magnet:` links already use, and
// the same shared rule about which schemes those are.
class android_view : public web_view_backend {
	Q_OBJECT
public:
	explicit android_view(request_filter *filter, QWidget *parent = nullptr);
	~android_view() override;

	// Called from JNI, on Android's UI thread. Static because JNI has nowhere
	// to put a `this`; the id is how it finds its way back.
	static void report_url(qint64 id, const QString &url);
	static void report_nav_state(qint64 id, bool back, bool forward);
	// The renderer for `id` died and Java has already put a fresh WebView in
	// its place. See the Java override for why it must not simply be left
	// dead: returning false from `onRenderProcessGone` kills the application
	// process, and the desktop backend treats the same event as a page that
	// stopped responding.
	static void report_render_gone(qint64 id, bool crashed);
	// What the page calls itself. The shell titles a tree row from this and
	// falls back to the host when nothing arrives -- which, until this
	// existed, was every row on Android.
	static void report_title(qint64 id, const QString &title);
	// How far the current load has got, and that it ended. The shell believes
	// a load is in flight only from these, so without them its progress bar,
	// its Stop button and its "could not be loaded" message were all
	// unreachable on this backend.
	static void report_progress(qint64 id, int percent);
	static void report_load_finished(qint64 id, bool ok);
	// A page took the screen, or gave it back. The shell answers by entering
	// or leaving kiosk, which is what hides its own chrome.
	static void report_fullscreen(qint64 id, bool on);

	// Called from JNI, on the WebView's *network* thread -- not the UI thread and
	// not Qt's. It consults the shared filter and touches nothing else, which is
	// exactly what `request_filter::decide()` documents itself as safe for.
	//
	// There is one filter for the process, so it is held statically rather than
	// looked up by id: the interceptor fires for requests belonging to a page
	// whose view may already be gone, and a decision that depends on the view
	// still existing would be a race for no benefit.
	static bool should_block(const QString &url, const QString &accept,
	                          const QString &page_url);

	// **Whether this page may set or send a third-party cookie.** The same
	// per-site policy the desktop applies, reached the same way -- through the
	// one shared filter, statically, for the reason above.
	//
	// It answers a coarser question than the desktop's, and cannot do better.
	// Qt hands `qtwebengine_factory` a filter callback per cookie, carrying the
	// first-party url and a third-party flag, so the desktop decides each one.
	// Android's `CookieManager` offers a single boolean per WebView, so the
	// most this can express is "third-party cookies, on this page, yes or no".
	// The first-party half of the policy is not asked at all here: Android has
	// no hook for it, and pretending otherwise by refusing all cookies when
	// first-party ones are disallowed would break the page far past what the
	// setting says.
	static bool allow_third_party_cookies(const QString &page_url);

	// Called from JNI on a binder thread, for one view. The Android side hops to
	// the Qt thread first, so these run where the bridges live.
	static QString describe_bridge(qint64 id, const QString &name);
	static QString call_bridge(qint64 id, const QString &name, const QString &method,
	                            const QString &args_json);
	// Everything to run at the start of a page, shim first.
	static QString injected_scripts(qint64 id);
	// The shim alone, for a destination that has not been navigated to yet.
	// Registered as a document-start script, which is the only injection point
	// on Android that runs before a page's own inline scripts -- `onPageStarted`
	// does not, and Teams reads `navigator.permissions` before it fires.
	static QString document_start_script(qint64 id, const QUrl &url);

	// Whether any WebView exists in this process. `android_factory` asks
	// before clearing the cache, which Android does through a view rather than
	// through a manager -- so with none open the honest answer is `refused`
	// and not a quiet success.
	static bool any_view_open();

	// A navigation the WebView is about to attempt. Returns true when the shell
	// took it instead -- `magnet:` and anything else that is not a page.
	//
	// Static, and the handler with it, because there is one shell: the factory is
	// told the handler once, after the views exist on the desktop and before them
	// here, and threading it through every view would make the order matter.
	// **Split in two, because the answer and the action need different threads.**
	//
	// Java asks this on Android's *UI* thread, and the whole call used to hop to
	// the Qt thread and wait. That deadlocked: Qt's thread blocks on the UI
	// thread inside ordinary repainting -- `QOpenGLContext::makeCurrent` waits
	// for a surface the UI thread services -- so a navigation arriving while Qt
	// was mid-flush left each thread waiting for the other. Ten seconds later
	// Android killed the input queue and put up "Hydra isn't responding".
	// Diagnosed from the ANR trace; it is a race, which is why it survived a
	// dozen navigations before it bit.
	//
	// `claims_external_url` is the decision and touches nothing but the url, so
	// it answers on whatever thread asks. `hand_to_external` is the part that
	// needs the shell, and is posted rather than waited on.
	static bool claims_external_url(const QString &url);
	static void hand_to_external(const QString &url);

	// Whether the view behind `id` may navigate to `url` (architecture doc
	// sec 5.5). Static for the same reason the others are: JNI has nowhere to put
	// a `this`, so the id is the handle back.
	//
	// **True when nothing is listening**, which is what a view without a
	// decider did before this existed and has to keep doing: a refusal that
	// nobody asked for would be a browser that will not browse.
	static bool allow_navigation(qint64 id, const QString &url,
	                              bool user_initiated);
	static void set_external_handler(std::function<void(const QUrl &)> fn);

	// A page called getUserMedia. Two refusals stand between it and the
	// camera, and they are not the same refusal: this browser's own site
	// policy, which is the engine the desktop asks through the same
	// `permission_decider`, and the operating system's grant, which a user
	// may have withheld from the application entirely. Either one is final.
	//
	// Answers through `HydraWebView.onCaptureDecision`, not by returning,
	// because the second question can put a dialog on the screen.
	static void request_capture(qint64 id, const QString &origin,
	                             bool video, bool audio, qint64 token);

	// The same question for geolocation: the shield, then the operating system.
	//
	// Separate from `request_capture` rather than a flag on it, because the two
	// differ in every particular that matters -- one feature rather than two, a
	// different Android permission, and an answer the WebView takes back by
	// origin rather than by request object. Sharing them would be a function
	// whose body is two functions behind a boolean.
	static void request_geolocation(qint64 id, const QString &origin,
	                                 qint64 token);

	// A page asked for a file. Runs on the Qt thread, shows Qt's file dialog --
	// which on Android *is* the system document picker -- and hands the chosen
	// urls back to Java, which is the only place that may answer the WebView.
	//
	// Answered exactly once, cancel included: a WebView whose chooser callback
	// is dropped never opens another one, so a silent early return here breaks
	// every file input on every later page.
	static void choose_file(qint64 id, bool multiple, const QString &accept);

	QWidget *widget() override;
	// The shell says something is over the page. Ored with `m_blocked`, which
	// is the same condition arriving from a QDialog.
	void set_obscured(bool on) override;

	// **Answered from what Java last pushed, not asked for on demand.** The
	// base class returns true for both, which left Back and Forward
	// permanently enabled on Android whatever the page could actually do.
	// Asking the WebView directly would mean blocking the Qt thread on
	// Android's UI thread, which is how this tree met its first deadlock.
	bool can_go_back() const override { return m_can_back; }
	bool can_go_forward() const override { return m_can_forward; }
	QUrl url() const override { return m_url; }
	void load(const QUrl &url) override;
	void back() override;
	void forward() override;
	void reload() override;

	// Both of these were `Q_UNUSED` stubs, which is the worst shape a setting
	// can have: the dialog offered the toggle, the shell recorded it, and the
	// page carried on exactly as before with nothing said anywhere. They are
	// real now, over JNI, and what each `view_settings` field maps to is
	// written down beside the definitions and in `HydraWebView.applySettings`
	// -- including the one that is deliberately still not honoured.
	void apply_settings(const view_settings &s) override;
	QString page_title() const override { return m_title; }
	void stop() override;
	void exit_fullscreen() override;
	void set_permission_decider(permission_decider fn) override { m_decider = std::move(fn); }
	// **Accepted and never called, which is honest rather than lazy.** Android's
	// WebView has no `getDisplayMedia` to answer, so there is nothing to choose
	// between; storing it keeps the shell's wiring identical on both platforms
	// instead of making `main_window` know which backend it has. If the platform
	// grows the capability, the seam is already here.
	void set_capture_chooser(capture_chooser fn) override { m_capture_chooser = std::move(fn); }
	// The policy peek, readable by the script builder.
	//
	// A plain accessor rather than making the member public: the shim needs to
	// *ask* the shield, and nothing outside this class should be able to hand it
	// a different answer.
	const capability_peek &peek() const { return m_capability_peek; }

	void set_desktop_site(bool on) override;
	bool desktop_site() const override { return m_desktop_site; }
	void set_navigation_decider(navigation_decider fn) override {
		m_navigation_decider = std::move(fn);
	}
	void set_zoom_factor(double factor) override;
	// Answered from what was last asked for, not read back from the WebView --
	// `getScale()` is UI-thread only, so asking would block the Qt thread on
	// Android's UI thread. Overridden rather than left to the base class,
	// which answers 1.0: the shell steps zoom *relative* to this, so a backend
	// that always said 1.0 would move one step and then stay there whichever
	// way the user pressed.
	double zoom_factor() const override { return m_zoom; }

	void inject_script(const QString &name, const QString &source,
	                    bool subframes = false) override;
	void inject_main_world_script(const QString &name,
	                               const QString &source) override;
	void set_script_bridge(QObject *object, const QString &name) override;

	QByteArray save_state() const override;
	bool       restore_state(const QByteArray &blob) override;

protected:
	// The native view lives outside Qt's widget tree, so it has to be told
	// where to be, every time this widget moves, resizes, or is covered.
	bool eventFilter(QObject *o, QEvent *e) override;

private:
	void refresh();
	void sync_geometry();
	void on_url_from_java(const QString &url);
	void on_nav_state_from_java(bool back, bool forward);

	static QHash<qint64, android_view *> s_views;
	static request_filter *s_filter;   // one per process; see should_block()
	static std::function<void(const QUrl &)> s_external;
	qint64 m_id = 0;
	bool   m_native = false;   // false when there is no WebView to talk to
	bool   m_blocked = false;  // a modal dialog is over the window
	bool   m_obscured = false; // the shell says something else is (the drawer)
	bool   m_can_back = false;
	bool   m_can_forward = false;
	double m_zoom = 1.0;       // what set_zoom_factor() was last asked for

	QLabel *m_widget = nullptr;
	QUrl    m_url;
	QString m_title;   // last reported by onReceivedTitle; see page_title()
	permission_decider m_decider;
	capture_chooser    m_capture_chooser;
	bool               m_desktop_site = false;
	navigation_decider m_navigation_decider;
	bridge_invoker m_bridges;
	QStringList    m_script_names;   // named, so a test can see what was asked for
	QStringList    m_script_sources; // in registration order, which is load order
};

// The factory half. Same shape as the desktop one, so `main()` differs by two
// lines rather than by a structure.
class android_factory : public web_view_factory {
public:
	explicit android_factory(request_filter *filter) : m_filter(filter) {}

	web_view_backend *create_view(QWidget *parent) override;
	void set_external_url_handler(external_url_handler fn) override {
		m_external = fn;
		android_view::set_external_handler(fn);
	}
	// Not wired: Android's downloads go through the platform `DownloadManager`
	// in `android_downloads`, which the WebView's own `setDownloadListener`
	// would feed. Taking the handler and never calling it would be worse than
	// saying so here.
	void set_download_handler(download_note) override {}

	// **What the phone can and cannot forget**, answered honestly rather than
	// stubbed. Android is better than the desktop on one of these and worse on
	// another, and the report says which is which instead of averaging them
	// into a claim that clearing worked.
	//
	// Cookies go through `CookieManager.removeAllCookies`, whose callback says
	// whether anything was removed but never how many -- so `cookies_removed`
	// stays -1 here, which is the value that means nobody counted rather than
	// the value that means none. Site data goes with them, which the desktop
	// cannot do at all: `WebStorage.deleteAllData()` takes localStorage,
	// IndexedDB and WebSQL for every origin, where Qt 6.8 wraps no equivalent.
	// The cache is cleared with no completion signal to wait on, so it is
	// reported `unconfirmed`. Visited links are `refused`: Android exposes no
	// visited-link store, and `WebView.clearHistory()` is the back/forward
	// list, which belongs to the shell -- mapping one onto the other would
	// delete the tab history while claiming to have done something else.
	void clear_browsing_data(const browsing_data &what, clear_note done) override;

private:
	request_filter      *m_filter = nullptr;
	external_url_handler m_external;
};
