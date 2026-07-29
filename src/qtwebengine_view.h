// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web_view_backend.h"

class QWebEngineView;
class QWebEnginePage;
class QWebEngineProfile;

// The desktop web_view_backend: a QWebEngineView plus its page, wrapped so the
// shell never names either (architecture doc §19.2). Everything
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
	void apply_settings(const view_settings &s) override;
	void set_permission_decider(permission_decider fn) override;
	void set_zoom_factor(double factor) override;
	QByteArray save_state() const override;
	bool       restore_state(const QByteArray &blob) override;

private:
	QWebEngineView *m_view = nullptr;
	QWebEnginePage *m_page = nullptr;
	permission_decider m_decider;
};
