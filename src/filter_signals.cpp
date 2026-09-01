#include "filter_signals.h"
#include <QUrl>
#include <QDateTime>
#include "policy_engine.h"

#include <QMutexLocker>

namespace {

// URL shapes that ad and tracking endpoints reach for.
const char *k_ad_shapes[] = {
	"/ads/", "/ad/", "/adserver", "/adservice", "/adsystem", "/advert",
	"/banner", "/pagead", "/doubleclick", "/track", "/tracker", "/telemetry",
	"/beacon", "/pixel", "/analytics", "/collect", "/impression", "/sponsor",
};

constexpr int k_max_per_site = 400;

}  // namespace

filter_signals::filter_signals(QObject *parent) : QObject(parent) {}

bool filter_signals::looks_ad_shaped(const QString &url, const QString &request_host,
                                      const QString &site_host) {
	// First-party requests are almost never the ad, and proposing a rule
	// against one is how a filter list breaks the page it was meant to fix.
	if (request_host.isEmpty() || site_host.isEmpty())
		return false;
	if (request_host == site_host || request_host.endsWith("." + site_host))
		return false;

	const QString lower = url.toLower();
	for (const char *shape : k_ad_shapes)
		if (lower.contains(QLatin1String(shape)))
			return true;
	return false;
}

void filter_signals::on_request(const request_context &ctx, const request_decision &d) {
	if (ctx.site_host.isEmpty())
		return;
	const QString url = ctx.url.toString();
	if (url.isEmpty())
		return;

	QMutexLocker guard(&m_lock);

	QStringList &seen = m_observed[ctx.site_host];
	if (seen.size() < k_max_per_site && !seen.contains(url))
		seen << url;

	// Only requests that got *through* are evidence of a filter gap. One that
	// was blocked is the system working.
	if (d.block)
		return;
	if (!looks_ad_shaped(url, ctx.request_host, ctx.site_host))
		return;

	QStringList &sus = m_suspects[ctx.site_host];
	if (sus.size() < k_max_per_site && !sus.contains(url))
		sus << url;
}

QStringList filter_signals::suspects_for(const QString &site_host) const {
	QMutexLocker guard(&m_lock);
	return m_suspects.value(site_host);
}

QStringList filter_signals::observed_for(const QString &site_host) const {
	QMutexLocker guard(&m_lock);
	return m_observed.value(site_host);
}

int filter_signals::count_for(const QString &site_host) const {
	QMutexLocker guard(&m_lock);
	return m_suspects.value(site_host).size();
}

void filter_signals::note_capability(const QString &site_host,
                                      const QString &feature,
                                      const QString &origin,
                                      const QString &answer) {
	if (site_host.isEmpty() || feature.isEmpty())
		return;
	// One line, readable by a person and by a model, with the time because the
	// order things happened in is most of the diagnosis: a page that asks twice
	// and is answered differently is a different fault from one asked once.
	//
	// The origin is included only when it differs from the site: a request from
	// an embedded frame is the interesting case -- it is how a page can be
	// granted a camera it never asked for, and how one can ask from a place the
	// per-site rule was never written about -- and repeating the host when they
	// match is noise.
	QString line = QDateTime::currentDateTime().toString("HH:mm:ss") + "  " +
	                feature + ": " + answer;
	const QString from = QUrl(origin).host();
	if (!from.isEmpty() && from != site_host)
		line += "  (asked by " + from + ")";

	QMutexLocker guard(&m_lock);
	QStringList &list = m_capabilities[site_host];
	list.prepend(line);
	// **Bounded, and the bound is small on purpose.** This is evidence for the
	// moment a person noticed something, not an audit trail; twenty entries is
	// more than anybody reads and far less than a page asking in a loop can
	// produce in a minute.
	while (list.size() > 20)
		list.removeLast();
}

QStringList filter_signals::capabilities_for(const QString &site_host) const {
	QMutexLocker guard(&m_lock);
	return m_capabilities.value(site_host);
}

void filter_signals::clear_site(const QString &site_host) {
	QMutexLocker guard(&m_lock);
	m_capabilities.remove(site_host);
	m_suspects.remove(site_host);
	m_observed.remove(site_host);
}
