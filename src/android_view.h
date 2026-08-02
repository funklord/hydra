// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
// What is still missing (architecture doc §19.2, §19.5):
//
//   * answer `shouldInterceptRequest` from the shared `request_filter`, which is
//     already platform-neutral and needs no Android-specific decisions;
//   * carry the content scripts over `addJavascriptInterface`, since there is no
//     QWebChannel transport on this side;
//   * and map `shouldOverrideUrlLoading` onto the external-url handler that
//     `magnet:` links already use on the desktop.
//
// Until the first of those lands, the policy engine and the filter lists decide
// nothing here: they run, and nothing asks them. That is the next piece.
class android_view : public web_view_backend {
	Q_OBJECT
public:
	explicit android_view(QWidget *parent = nullptr);
	~android_view() override;

	// Called from JNI, on Android's UI thread. Static because JNI has nowhere
	// to put a `this`; the id is how it finds its way back.
	static void report_url(qint64 id, const QString &url);

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
	qint64 m_id = 0;
	bool   m_native = false;   // false when there is no WebView to talk to

	QLabel *m_widget = nullptr;
	QUrl    m_url;
	permission_decider m_decider;
	QStringList m_scripts;   // named, so a test can see what was asked for
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
