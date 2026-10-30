#include "extractor_signals.h"

extractor_signals::extractor_signals(QObject *parent) : QObject(parent) {}

void extractor_signals::on_request(const request_context &ctx,
                                    const request_decision &d) {
	Q_UNUSED(d)   // blocked requests are evidence too: the page still asked
	if (ctx.site_host.isEmpty() || !ctx.url.isValid())
		return;

	QMutexLocker lock(&m_lock);
	QList<evidence_request> &list = m_by_site[ctx.site_host];

	evidence_request r;
	r.url   = ctx.url;
	r.page  = ctx.page.adjusted(QUrl::RemoveFragment);
	r.order = m_next_order[ctx.site_host]++;
	switch (ctx.kind) {
		case resource_kind::script: r.kind = "script"; break;
		case resource_kind::image:  r.kind = "image";  break;
		default:                    r.kind = "other";  break;
	}
	list.push_back(r);

	// Keep the newest. A stream's segment requests would otherwise bury the
	// handful of requests that actually matter, and the manifest is usually
	// near the end anyway -- it is fetched when the player starts.
	while (list.size() > k_limit)
		list.removeFirst();
}

QList<evidence_request> extractor_signals::evidence_for(const QString &site_host,
                                                        const QUrl &page) const {
	QMutexLocker lock(&m_lock);
	const QList<evidence_request> all = m_by_site.value(site_host);
	if (!page.isValid())
		return all;
	const QUrl want = page.adjusted(QUrl::RemoveFragment);
	QList<evidence_request> out;
	for (const evidence_request &r : all)
		// A request that reported no page -- one racing a tab's first
		// navigation on Android -- cannot be put on any page, so it is
		// kept for every page of its site rather than lost.
		if (r.page == want || r.page.isEmpty())
			out << r;
	return out;
}

int extractor_signals::count_for(const QString &site_host) const {
	QMutexLocker lock(&m_lock);
	return m_by_site.value(site_host).size();
}

int extractor_signals::clear_all() {
	QMutexLocker lock(&m_lock);
	const int sites = m_by_site.size();
	m_by_site.clear();
	// The counter too, or the next page's first request is numbered from where
	// the last session left off -- and the prompt tells the model that `order`
	// is this visit's position in the list.
	m_next_order.clear();
	return sites;
}

void extractor_signals::clear_site(const QString &site_host) {
	QMutexLocker lock(&m_lock);
	m_by_site.remove(site_host);
	m_next_order.remove(site_host);
}
