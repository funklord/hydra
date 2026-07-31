// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_fetcher.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>

// Lives on the worker thread, and owns the only QNetworkAccessManager here.
// A QNAM belongs to the thread that created it, which is the reason this is a
// separate object rather than a lambda.
class network_fetcher::worker : public QObject {
	Q_OBJECT
public:
	explicit worker(stream_context ctx) : m_ctx(std::move(ctx)) {}

public slots:
	fetch_result run(const QUrl &url, qint64 max_bytes, int timeout_ms) {
		if (!m_net)
			m_net = new QNetworkAccessManager(this);

		fetch_result out;
		QNetworkRequest req(url);
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                  QNetworkRequest::NoLessSafeRedirectPolicy);
		if (max_bytes > 0)
			req.setRawHeader("Range", QByteArray("bytes=0-") +
			                            QByteArray::number(max_bytes - 1));
		if (!m_ctx.referer.isEmpty())
			req.setRawHeader("Referer", m_ctx.referer.toUtf8());
		if (!m_ctx.user_agent.isEmpty())
			req.setRawHeader("User-Agent", m_ctx.user_agent.toUtf8());
		if (!m_ctx.cookies.isEmpty())
			req.setRawHeader("Cookie", m_ctx.cookies.toUtf8());
		for (auto it = m_ctx.extra.cbegin(); it != m_ctx.extra.cend(); ++it)
			req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

		QNetworkReply *reply = m_net->get(req);

		// Nested, but on this thread, where there is nothing to re-enter: no
		// widgets, no user input, and no script. On the calling thread the same
		// loop would deliver paint and input events in the middle of a running
		// extractor.
		QEventLoop loop;
		QTimer deadline;
		deadline.setSingleShot(true);
		bool timed_out = false;
		QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
			timed_out = true;
			reply->abort();
			loop.quit();
		});
		QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		deadline.start(timeout_ms > 0 ? timeout_ms : 10000);
		loop.exec();

		out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (timed_out) {
			out.error = QString("timed out after %1 ms").arg(timeout_ms);
		} else if (reply->error() != QNetworkReply::NoError && out.status == 0) {
			out.error = reply->errorString();
		} else {
			out.reached = true;
			out.content_type =
				reply->header(QNetworkRequest::ContentTypeHeader)
				    .toString().section(';', 0, 0).trimmed();
			// A server that ignores Range sends the whole body, so the cap is
			// applied here as well as asked for.
			out.body = max_bytes > 0 ? reply->read(max_bytes) : reply->readAll();
		}
		reply->deleteLater();
		return out;
	}

private:
	stream_context m_ctx;
	QNetworkAccessManager *m_net = nullptr;
};

network_fetcher::network_fetcher(stream_context ctx, QObject *parent)
	: QObject(parent) {
	qRegisterMetaType<fetch_result>("fetch_result");
	m_thread = new QThread;
	m_thread->setObjectName("hydra-helper-fetch");
	m_worker = new worker(std::move(ctx));
	m_worker->moveToThread(m_thread);
	QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
	m_thread->start();
}

network_fetcher::~network_fetcher() {
	if (!m_thread)
		return;
	m_thread->quit();
	m_thread->wait();
	delete m_thread;
}

fetch_result network_fetcher::fetch(const QUrl &url, qint64 max_bytes,
                                     int timeout_ms) {
	fetch_result out;
	if (!m_worker || !m_thread) {
		out.error = "the fetcher is not running";
		return out;
	}
	// Blocking across a thread to itself is a deadlock, and a deadlock inside a
	// review dialog looks exactly like a hang. Refuse instead.
	if (QThread::currentThread() == m_thread) {
		out.error = "refusing to fetch from the fetcher's own thread";
		return out;
	}
	QMetaObject::invokeMethod(m_worker, "run", Qt::BlockingQueuedConnection,
	                           Q_RETURN_ARG(fetch_result, out),
	                           Q_ARG(QUrl, url), Q_ARG(qint64, max_bytes),
	                           Q_ARG(int, timeout_ms));
	return out;
}

helper_fetcher network_fetcher::as_function() {
	return [this](const QUrl &u, qint64 cap, int ms) { return fetch(u, cap, ms); };
}

#include "network_fetcher.moc"
