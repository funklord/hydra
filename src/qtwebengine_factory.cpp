// SPDX-License-Identifier: GPL-3.0-or-later
#include "qtwebengine_factory.h"
#include "qtwebengine_view.h"
#include "request_filter.h"
#include "qtwebengine_interceptor.h"

#include <QWebEngineProfile>
#include <QWebEngineCookieStore>

qtwebengine_factory::qtwebengine_factory(request_filter *filter)
	: m_filter(filter) {
	// One shared profile for every view (architecture doc §6).
	m_profile = QWebEngineProfile::defaultProfile();

	m_interceptor = new qtwebengine_interceptor(m_filter);
	m_profile->setUrlRequestInterceptor(m_interceptor);

	// The cookie filter is the same decision as any other request, so it goes
	// through the same shared filter; only unpacking Qt's FilterRequest is
	// specific to this backend.
	request_filter *f = m_filter;
	m_profile->cookieStore()->setCookieFilter(
		[f](const QWebEngineCookieStore::FilterRequest &r) {
			return f->allow_cookie(r.firstPartyUrl.host(), r.thirdParty);
		});
}

qtwebengine_factory::~qtwebengine_factory() {
	// The profile outlives us (it is Qt's default one), so take our hooks back
	// off it before the interceptor goes away.
	if (m_profile) {
		m_profile->setUrlRequestInterceptor(nullptr);
		m_profile->cookieStore()->setCookieFilter(nullptr);
	}
	delete m_interceptor;
}

web_view_backend *qtwebengine_factory::create_view(QWidget *parent) {
	return new qtwebengine_view(m_profile, parent);
}
