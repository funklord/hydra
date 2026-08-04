// SPDX-License-Identifier: GPL-3.0-or-later
#include "antiadblock_watch.h"

#include <QUrl>

antiadblock_watch::antiadblock_watch(QObject *parent)
  : QObject(parent), m_detectors(site_rules::defaults().detectors()) {}

void antiadblock_watch::set_rules(const site_rules &r) {
	QMutexLocker lock(&m_lock);
	m_detectors = r.detectors();
}

bool antiadblock_watch::looks_like_detector(const QUrl &url,
                                             const QStringList &names) {
	// The file name, not the whole address: a query string can contain anything,
	// including an innocent page's own search terms.
	const QString name = url.fileName().toLower();
	if (name.isEmpty())
		return false;
	for (const QString &d : names)
		if (!d.isEmpty() && name.contains(d, Qt::CaseInsensitive))
			return true;
	return false;
}

bool antiadblock_watch::looks_like_detector(const QUrl &url) const {
	QStringList names;
	{
		QMutexLocker lock(&m_lock);
		names = m_detectors;
	}
	return looks_like_detector(url, names);
}

void antiadblock_watch::on_request(const request_context &ctx,
                                    const request_decision &d) {
	Q_UNUSED(d)   // blocked or not, asking the question is the finding
	if (ctx.site_host.isEmpty() || !looks_like_detector(ctx.url))
		return;

	QString what;
	{
		QMutexLocker lock(&m_lock);
		QStringList &seen = m_by_site[ctx.site_host];
		const QString name = ctx.url.fileName();
		if (seen.contains(name))
			return;
		// Bounded: a page could fetch a hundred differently-named detectors and
		// this is a status message, not an archive.
		if (seen.size() >= 5)
			return;
		seen << name;
		what = name;
		// Only the first sighting says anything. The user needs to be told once
		// that this page is checking, not once per script.
		if (seen.size() != 1)
			return;
	}
	emit detected(ctx.site_host, what);
}

bool antiadblock_watch::checked_for_blocker(const QString &site_host) const {
	QMutexLocker lock(&m_lock);
	return !m_by_site.value(site_host).isEmpty();
}

QStringList antiadblock_watch::evidence_for(const QString &site_host) const {
	QMutexLocker lock(&m_lock);
	return m_by_site.value(site_host);
}

void antiadblock_watch::clear_site(const QString &site_host) {
	QMutexLocker lock(&m_lock);
	m_by_site.remove(site_host);
}
