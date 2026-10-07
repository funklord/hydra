#include "subscription_updater.h"

#include <QDateTime>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>

subscription_updater::subscription_updater(const QString &index_path,
                                            const QString &dir,
                                            QObject *parent)
  : QObject(parent), m_index(index_path), m_dir(dir) {
	m_subs = filter_subscription::load_index(m_index);
}

bool subscription_updater::set_subscriptions(const QList<subscription> &subs) {
	m_subs = subs;
	return filter_subscription::save_index(m_index, m_subs);
}

int subscription_updater::update(bool force, int max_age_hours) {
	if (busy())
		return 0;
	m_queue.clear();
	m_promoted = 0;
	m_refused  = 0;
	const QDateTime now = QDateTime::currentDateTime();
	for (int i = 0; i < m_subs.size(); ++i) {
		const subscription &s = m_subs.at(i);
		if (!s.enabled || s.url.isEmpty())
			continue;
		// **Due, or forced.** A browser that re-fetches every list on every
		// launch is doing it for nobody: these change over a day and the
		// cached copy is in force meanwhile. `force` is the Update now button,
		// which exists because somebody who has just subscribed should not
		// have to wait a day to find out whether the url was right.
		if (!force && s.fetched.isValid() &&
		    s.fetched.secsTo(now) < qint64(max_age_hours) * 3600)
			continue;
		m_queue.push_back(i);
	}
	if (m_queue.isEmpty())
		return 0;
	start_next();
	return m_queue.size() + 1;   // the one now in flight, plus the rest
}

void subscription_updater::start_next() {
	if (m_queue.isEmpty()) {
		m_at = -1;
		// Written once at the end of a pass rather than after each fetch: the
		// index is small, and a half-written one is the thing `save_index`
		// takes care to avoid.
		if (m_promoted > 0 || m_refused > 0)
			filter_subscription::save_index(m_index, m_subs);
		emit updated(m_promoted, m_refused);
		return;
	}
	const int which = m_queue.takeFirst();
	m_at = which;
	if (!m_net)
		m_net = new QNetworkAccessManager(this);

	const subscription &s = m_subs.at(which);
	emit note(QString("Fetching %1...").arg(s.name.isEmpty() ? s.url.host()
	                                                          : s.name));

	QNetworkRequest req(s.url);
	// **Bounded from the outside as well as the inside.** The size check below
	// needs bytes to arrive to fire at all; a server that accepts the
	// connection and then says nothing is stopped by this instead.
	req.setTransferTimeout(30000);
	req.setMaximumRedirectsAllowed(5);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                  QNetworkRequest::NoLessSafeRedirectPolicy);
	// Identified as what it is. A list server deciding to refuse an unknown
	// client is entitled to, and a request that hides what it is makes that
	// its own problem to diagnose.
	req.setHeader(QNetworkRequest::UserAgentHeader,
	               QStringLiteral("hydra/filter-subscriptions"));

	QNetworkReply *reply = m_net->get(req);
	connect(reply, &QNetworkReply::downloadProgress, reply,
	         [reply](qint64 got, qint64) {
		if (got > k_max_bytes)
			reply->abort();
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, which] {
		finish(reply, which);
	});
}

void subscription_updater::finish(QNetworkReply *reply, int which) {
	subscription &s = m_subs[which];
	const QByteArray body = reply->readAll();
	const int status =
	  reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QNetworkReply::NetworkError err = reply->error();
	const QString err_text = reply->errorString();
	reply->deleteLater();

	const auto refuse = [&](const QString &why) {
		s.note = why;
		++m_refused;
		emit note(QString("%1: %2").arg(s.name, why));
	};

	if (err != QNetworkReply::NoError) {
		// **The cache is untouched: a list that cannot be reached is not a
		// list that has changed**, which is the whole reason the body is
		// cached rather than held in memory.
		//
		// The status first where there is one, because Qt calls a 404 an
		// error and its words for one are "Error transferring <url>" -- which
		// tells a person nothing they did not know. A list answering 404 has
		// moved, and that is the one thing worth reading. Measured: the test
		// below asked for the status and got the transfer message.
		refuse(status > 0 ? QString("the server answered %1").arg(status)
		                   : err_text);
	} else if (status != 200) {
		// Not an error as far as Qt is concerned: a 204, or a 200-shaped 404
		// page, arrives as a successful reply with the wrong thing in it.
		refuse(QString("the server answered %1").arg(status));
	} else {
		const subscription_read rep = filter_subscription::read(
		  QString::fromUtf8(body), s.rules, s.trusted);
		if (!rep.ok()) {
			refuse(rep.refusal);
		} else if (!QDir().mkpath(m_dir)) {
			refuse(QStringLiteral("the cache directory could not be made"));
		} else {
			if (s.file.isEmpty())
				s.file = filter_subscription::mint_cache_name(m_dir, s.name);
			QSaveFile out(QDir(m_dir).filePath(s.file));
			if (!out.open(QIODevice::WriteOnly) ||
			    out.write(body) != body.size() || !out.commit()) {
				// **Fetched and not promoted, said plainly.** The rules could
				// be put into the live list from memory and would then vanish
				// at the next launch, with the index claiming a fetch that
				// left nothing behind. A subscription whose cache cannot be
				// written is a subscription that is not working, and saying so
				// is worth more than one session of blocking.
				refuse(QStringLiteral("fetched, but the copy could not be "
				                       "written"));
			} else {
				s.fetched = QDateTime::currentDateTime();
				s.rules   = rep.accepted;
				s.note    = rep.summary();
				++m_promoted;
				emit note(QString("%1: %2").arg(s.name, rep.summary()));
			}
		}
	}
	start_next();
}
