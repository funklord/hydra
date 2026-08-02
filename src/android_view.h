// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bridge_invoker.h"
#include "web_view_backend.h"
#include "web_view_factory.h"

#include <QHash>
#include <QUrl>

class QLabel;
class request_filter;

// The Android side of the WebView seam — **a System WebView, driven over JNI.**
//
// Pages load, links navigate, back returns, and the address bar follows along.
// The WebView itself lives outside Qt's widget tree: Qt for Widgets draws into
// its own surface and cannot host a native Android view, so `HydraWebView.java`
// adds one to the Activity *on top* of Qt's surface and this class keeps it
// glued to wherever the page-area widget is, in device pixels. The cost of that
// arrangement is stated rather than hidden — this view sits above everything Qt
// draws, so anything Qt wants to show over the page has to hide it first.
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
// the same `window.hydraChannel(cb)` the desktop scripts are written against —
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
// What is still missing (architecture doc §19.2, §19.5): mapping
// `shouldOverrideUrlLoading` onto the external-url handler that `magnet:` links
// already use on the desktop.
class android_view : public web_view_backend {
	Q_OBJECT
public:
	explicit android_view(request_filter *filter, QWidget *parent = nullptr);
	~android_view() override;

	// Called from JNI, on Android's UI thread. Static because JNI has nowhere
	// to put a `this`; the id is how it finds its way back.
	static void report_url(qint64 id, const QString &url);

	// Called from JNI, on the WebView's *network* thread — not the UI thread and
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

	QWidget *widget() override;
	QUrl url() const override { return m_url; }
	void load(const QUrl &url) override;
	void back() override;
	void forward() override;
	void reload() override;

	void apply_settings(const view_settings &s) override { Q_UNUSED(s) }
	void set_permission_decider(permission_decider fn) override { m_decider = std::move(fn); }
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
	qint64 m_id = 0;
	bool   m_native = false;   // false when there is no WebView to talk to

	QLabel *m_widget = nullptr;
	QUrl    m_url;
	permission_decider m_decider;
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
		m_external = std::move(fn);
	}

private:
	request_filter      *m_filter = nullptr;
	external_url_handler m_external;
};
