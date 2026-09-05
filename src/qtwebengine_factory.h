#pragma once

#include "web_view_factory.h"

#include <QStringList>

class QWebEngineProfile;
class magnet_scheme_handler;
class request_filter;
class qtwebengine_interceptor;

// The desktop web_view_factory. Owns the shared QWebEngineProfile and installs
// the profile-wide machinery on it once: the request interceptor and the
// cookie filter, both deciding through the shared request_filter
// (architecture doc sec 6/sec 7.3). The download handler attaches here too in step 6.
class qtwebengine_factory : public web_view_factory {
public:
	explicit qtwebengine_factory(request_filter *filter);
	~qtwebengine_factory() override;

	web_view_backend *create_view(QWidget *parent) override;
	QString user_agent() const override;
	void set_external_url_handler(external_url_handler fn) override;
	void set_download_handler(download_note fn) override;
	void clear_browsing_data(const browsing_data &what,
	                          clear_note done) override;

	// Must be called before QApplication exists -- Qt requires custom schemes to
	// be registered before the engine initialises, and it is a fatal warning
	// otherwise. main() is therefore the only possible caller, which is also
	// where the concrete backend is already named.
	//
	// Registering a scheme is what makes Chromium route it to a handler instead
	// of discarding it as an external protocol. Only register schemes something
	// can actually take: a registered scheme with no handler behind it would
	// swallow the link silently, which is worse than the error page.
	static void register_url_schemes(const QStringList &schemes);

	// **The profile the app actually uses**, which since it gained a storage
	// name is no longer `QWebEngineProfile::defaultProfile()`. A live driver
	// that wants the cookie store must ask here: reaching for the default gets
	// a profile nothing in the shell has ever loaded a page into, and every
	// check against it fails while looking like a finding about cookies.
	QWebEngineProfile *profile() const { return m_profile; }

private:
	QWebEngineProfile  *m_profile     = nullptr;
	request_filter     *m_filter      = nullptr;
	qtwebengine_interceptor *m_interceptor = nullptr;
	magnet_scheme_handler   *m_scheme_handler = nullptr;
	external_url_handler     m_external;
	download_note            m_download_note;
};
