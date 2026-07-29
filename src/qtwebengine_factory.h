// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web_view_factory.h"

class QWebEngineProfile;
class request_filter;
class qtwebengine_interceptor;

// The desktop web_view_factory. Owns the shared QWebEngineProfile and installs
// the profile-wide machinery on it once: the request interceptor and the
// cookie filter, both deciding through the shared request_filter
// (architecture doc §6/§7.3). The download handler attaches here too in step 6.
class qtwebengine_factory : public web_view_factory {
public:
	explicit qtwebengine_factory(request_filter *filter);
	~qtwebengine_factory() override;

	web_view_backend *create_view(QWidget *parent) override;

private:
	QWebEngineProfile  *m_profile     = nullptr;
	request_filter     *m_filter      = nullptr;
	qtwebengine_interceptor *m_interceptor = nullptr;
};
