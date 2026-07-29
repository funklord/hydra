// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWebEngineUrlRequestInterceptor>

class request_filter;

// The Qt WebEngine half of request interception: unpack Qt's request info into
// a neutral request_context, ask the shared request_filter, apply the answer.
// All of the actual policy lives in request_filter, so Android's
// shouldInterceptRequest can reuse it verbatim (architecture doc §19.5).
class qtwebengine_interceptor : public QWebEngineUrlRequestInterceptor {
	Q_OBJECT
public:
	explicit qtwebengine_interceptor(request_filter *filter, QObject *parent = nullptr);

	void interceptRequest(QWebEngineUrlRequestInfo &info) override;

private:
	request_filter *m_filter;
};
