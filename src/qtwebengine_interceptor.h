#pragma once

#include <QSet>
#include <QUrl>
#include <QWebEngineUrlRequestInterceptor>

class request_filter;

// The Qt WebEngine half of request interception: unpack Qt's request info into
// a neutral request_context, ask the shared request_filter, apply the answer.
// All of the actual policy lives in request_filter, so Android's
// shouldInterceptRequest can reuse it verbatim (architecture doc sec 19.5).
class qtwebengine_interceptor : public QWebEngineUrlRequestInterceptor {
	Q_OBJECT
public:
	explicit qtwebengine_interceptor(request_filter *filter, QObject *parent = nullptr);

	void interceptRequest(QWebEngineUrlRequestInfo &info) override;

	// Whether a frame's last request for `url` was anything but a GET: a
	// download request does not say how it was fetched, and this does.
	bool was_posted(const QUrl &url) const { return m_posted.contains(url); }

private:
	request_filter *m_filter;
	// Bounded by forgetting: only the moment after a form is sent matters.
	QSet<QUrl>      m_posted;
};
