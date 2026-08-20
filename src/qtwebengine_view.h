// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web_view_backend.h"

class QWebEngineView;
class QWebEnginePage;
class QWebEngineProfile;
class QWebChannel;
// The page subclass, defined in the .cpp: the only reason it exists is to
// override acceptNavigationRequest, which is a virtual and not a signal.
class navigating_page;

// The desktop web_view_backend: a QWebEngineView plus its page, wrapped so the
// shell never names either (architecture doc sec 19.2). Everything
// Qt-WebEngine-specific about rendering a page lives behind this class.
class qtwebengine_view : public web_view_backend {
	Q_OBJECT
public:
	explicit qtwebengine_view(QWebEngineProfile *profile, QWidget *parent = nullptr);

	QWidget *widget() override;
	QUrl url() const override;
	void load(const QUrl &url) override;
	void back() override;
	void forward() override;
	void reload() override;
	void stop() override;
	void exit_fullscreen() override;
	void print() override;
	bool can_print() const override;
	void apply_settings(const view_settings &s) override;
	void set_permission_decider(permission_decider fn) override;
	void set_authenticator(authenticator fn) override;
	void set_navigation_decider(navigation_decider fn) override;
	void set_proxy_authenticator(proxy_authenticator fn) override;
	void set_certificate_chooser(certificate_chooser fn) override;
	void set_zoom_factor(double factor) override;
	double zoom_factor() const override;
	void inject_script(const QString &name, const QString &source,
	                    bool subframes = false) override;
	void inject_main_world_script(const QString &name, const QString &source) override;
	QString page_title() const override;
	void find_text(const QString &text, bool forward, bool fresh) override;
	bool can_go_back() const override;
	bool can_go_forward() const override;
	void set_script_bridge(QObject *object, const QString &name) override;
	QByteArray save_state() const override;
	bool       restore_state(const QByteArray &blob) override;

private:
	authenticator m_authenticator;
	proxy_authenticator m_proxy_authenticator;
	certificate_chooser m_certificate_chooser;
	QWebEngineView *m_view = nullptr;
	QWebEnginePage *m_page = nullptr;
	// The same object as m_page, typed so the decider can be handed over
	// without a downcast. Owned by the view, like m_page.
	navigating_page *m_nav_page = nullptr;
	permission_decider m_decider;
	QWebChannel *m_channel = nullptr;
	bool m_channel_api_injected = false;
};
