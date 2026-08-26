// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web_view_backend.h"
#include "webauth_dialog.h"

// **Included rather than forward-declared, and only for `QPointer`.** A
// `QPointer<T>` has to see that T derives from QObject at the point the member
// is declared, so an incomplete type does not compile. It is worth the include:
// the passkey conversation is the one request here that outlives the signal
// carrying it, so the engine is free to destroy the request object while a
// window is still on screen, and a raw pointer to it is a use-after-free
// waiting for a timeout to happen.
#include <QWebEngineWebAuthUxRequest>

#include <QPointer>

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
	// The passkey conversation, driven from `webAuthUxRequested`. Both are
	// QPointers because neither object is this class's to own: the dialog
	// belongs to the view widget and the request belongs to the engine, and
	// either can go away while the other is still being talked to.
	void show_webauth_state(QWebEngineWebAuthUxRequest::WebAuthUxState state);
	// Take the window down without answering. Used when the engine says the
	// request is over, which is the one case where closing must *not* be
	// reported back as a cancellation.
	void drop_webauth_dialog();

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
	// A print is on the way to a printer. `printRequested` is a page calling
	// window.print(), which a page may do in a loop, and Qt warns and drops a
	// second QWebEngineView::print() while the first is unfinished.
	bool m_printing = false;
	QPointer<webauth_dialog> m_webauth_dialog;
	QPointer<QWebEngineWebAuthUxRequest> m_webauth_request;
	// A modal question is already on screen. The two requests that ask one --
	// registering a protocol handler, and file system access -- are both things
	// a page can ask for in a loop, and each question runs a nested event loop
	// the page keeps running underneath. Without this, a page that asks twice
	// stacks two modal boxes, and one that asks in a loop stacks as many as it
	// likes over a window nobody can reach. Same guard, same reason, as
	// `m_printing` above.
	bool m_prompting = false;
};
