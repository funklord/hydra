// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web_view_backend.h"
#include "web_view_factory.h"

#include <QUrl>

class QLabel;
class request_filter;

// The Android side of the WebView seam — **a placeholder, not a browser.**
//
// It exists so the Android build links and packages, which is worth having on
// its own: the whole platform-neutral core compiles for arm64 already (fifty-one
// translation units, no errors), and without something behind the seam that fact
// is invisible because the link fails on three symbols `main()` names. With this
// there is an APK, and every later piece of the port is a change to one file
// pair rather than a change to the shell.
//
// **It renders a message saying what it is.** A stub that quietly showed a blank
// page would be indistinguishable from a real backend that is broken, and this
// project has spent enough time on things that look like they work. Nothing here
// pretends: `load()` records the address and shows it, and the honest reading of
// a screenshot is "the backend is not written yet".
//
// What the real one has to do (architecture doc §19.2, §19.5):
//
//   * hold an Android `WebView` through `QAndroidView`/JNI rather than a Qt
//     widget of its own;
//   * answer `shouldInterceptRequest` from the shared `request_filter`, which is
//     already platform-neutral and needs no Android-specific decisions;
//   * carry the content scripts over `addJavascriptInterface`, since there is no
//     QWebChannel transport on this side;
//   * and map `shouldOverrideUrlLoading` onto the external-url handler that
//     `magnet:` links already use on the desktop.
class android_view : public web_view_backend {
	Q_OBJECT
public:
	explicit android_view(QWidget *parent = nullptr);

	QWidget *widget() override;
	QUrl url() const override { return m_url; }
	void load(const QUrl &url) override;
	void back() override {}
	void forward() override {}
	void reload() override {}

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

private:
	void refresh();

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
