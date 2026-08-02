// SPDX-License-Identifier: GPL-3.0-or-later
#include "antiadblock_watch.h"

#include <QUrl>

namespace {

// Scripts whose only purpose is to find out whether a blocker is present. Kept
// short and specific on purpose: a loose pattern here would accuse ordinary
// pages of something they are not doing, and the message this drives tells the
// user to *turn off protection*. A false positive is therefore not a cosmetic
// mistake, which is why nothing matches merely on the word "ad".
const char *k_detectors[] = {
	"fuckadblock",
	"blockadblock",
	"adblock-detector",
	"adblockdetector",
	"detectadblock",
	"antiblock",
	"adbdetect",
};

}  // namespace

antiadblock_watch::antiadblock_watch(QObject *parent) : QObject(parent) {}

bool antiadblock_watch::looks_like_detector(const QUrl &url) {
	// The file name, not the whole address: a query string can contain
	// anything, including an innocent page's search terms.
	const QString name = url.fileName().toLower();
	if (name.isEmpty())
		return false;
	for (const char *d : k_detectors)
		if (name.contains(QLatin1String(d)))
			return true;
	return false;
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
