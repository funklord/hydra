// SPDX-License-Identifier: GPL-3.0-or-later
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

	// Called from JNI on a binder thread, for one view. The Android side hops to
	// the Qt thread first, so these run where the bridges live.
	static QString describe_bridge(qint64 id, const QString &name);
	static QString call_bridge(qint64 id, const QString &name, const QString &method,
	                            const QString &args_json);
	// Everything to run at the start of a page, shim first.
	static QString injected_scripts(qint64 id);

	// A navigation the WebView is about to attempt. Returns true when the shell
	// took it instead -- `magnet:` and anything else that is not a page.
	//
	// Static, and the handler with it, because there is one shell: the factory is
	// told the handler once, after the views exist on the desktop and before them
	// here, and threading it through every view would make the order matter.
	static bool take_external_url(const QString &url);

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

	// A page asked for a file. Runs on the Qt thread, shows Qt's file dialog --
	// which on Android *is* the system document picker -- and hands the chosen
	// urls back to Java, which is the only place that may answer the WebView.
	//
	// Answered exactly once, cancel included: a WebView whose chooser callback
	// is dropped never opens another one, so a silent early return here breaks
	// every file input on every later page.
	static void choose_file(qint64 id, bool multiple, const QString &accept);

	QWidget *widget() override;
	QUrl url() const override { return m_url; }
	void load(const QUrl &url) override;
	void back() override;
	void forward() override;
	void reload() override;

	void apply_settings(const view_settings &s) override { Q_UNUSED(s) }
	void set_permission_decider(permission_decider fn) override { m_decider = std::move(fn); }
	void set_navigation_decider(navigation_decider fn) override {
		m_navigation_decider = std::move(fn);
	}
	void set_zoom_factor(double factor) override { Q_UNUSED(factor) }

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

	static QHash<qint64, android_view *> s_views;
	static request_filter *s_filter;   // one per process; see should_block()
	static std::function<void(const QUrl &)> s_external;
	qint64 m_id = 0;
	bool   m_native = false;   // false when there is no WebView to talk to
	bool   m_blocked = false;  // a modal dialog is over the window

	QLabel *m_widget = nullptr;
	QUrl    m_url;
	permission_decider m_decider;
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

private:
	request_filter      *m_filter = nullptr;
	external_url_handler m_external;
};
