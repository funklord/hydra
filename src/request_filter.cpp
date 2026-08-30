#include "request_filter.h"
#include "filter_list.h"
#include "policy_engine.h"

#include <QStringList>

using policy::feature;

request_filter::request_filter(policy_engine *engine) : m_engine(engine) {
	// Seed list only. The filter-evolution loop (step 6) grows this and imports
	// EasyList; kept tiny here on purpose.
	m_ad_hosts = {
		"doubleclick.net",
		"googlesyndication.com",
		"googleadservices.com",
		"google-analytics.com",
		"adservice.google.com",
		"ads.yahoo.com",
		"adnxs.com",
		"scorecardresearch.com",
		"moatads.com",
		"taboola.com",
		"outbrain.com",
	};
}

resource_kind kind_from_hints(const QString &accept, const QUrl &url) {
	// The header first: it is what the engine actually asked for, and it is
	// right even when the path carries no extension, which is most of the
	// modern web.
	const QString a = accept.toLower();
	if (a.contains(QLatin1String("image/")))
		return resource_kind::image;
	if (a.contains(QLatin1String("javascript")) ||
	    a.contains(QLatin1String("application/ecmascript")))
		return resource_kind::script;
	// `text/css`, `text/html`, `font/*` and friends are all "other" as far as
	// the rules go, and saying so early keeps the path guessing below from
	// firing on a stylesheet that happens to live under /js/.
	if (a.startsWith(QLatin1String("text/")) || a.contains(QLatin1String("font/")))
		return resource_kind::other;

	// Then the path, lowercased and without a query string: `?v=3` cache
	// busters are on nearly every script tag and would defeat a suffix match.
	const QString path = url.path().toLower();
	if (path.endsWith(QLatin1String(".js")) || path.endsWith(QLatin1String(".mjs")))
		return resource_kind::script;
	static const QStringList image_suffixes = {
		QStringLiteral(".png"),  QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
		QStringLiteral(".gif"),  QStringLiteral(".webp"), QStringLiteral(".svg"),
		QStringLiteral(".avif"), QStringLiteral(".ico"),  QStringLiteral(".bmp"),
	};
	for (const QString &s : image_suffixes)
		if (path.endsWith(s))
			return resource_kind::image;
	return resource_kind::other;
}

bool request_filter::is_ad_host(const QString &host) const {
	for (const QString &ad : m_ad_hosts)
		if (host == ad || host.endsWith("." + ad))
			return true;
	return false;
}

request_decision request_filter::decide(const request_context &ctx) const {
	request_decision d;
	if (!m_engine)
		return d;

	// Ads / trackers: block known ad hosts unless this site permits ads.
	if (is_ad_host(ctx.request_host) &&
	    !m_engine->is_allowed(feature::ads, ctx.site_host)) {
		d.block = true;
		return d;
	}

	// The accepted filter rules, which are ads and annoyances by another name and
	// so answer to the same per-site switch: turning ads back on for a site the
	// shield says is broken has to turn *all* of this off, or the escape hatch
	// only half works and the page still fails for a reason the user was told
	// they had disabled.
	if (m_list && !m_engine->is_allowed(feature::ads, ctx.site_host) &&
	    m_list->blocks(ctx.url.toString(), ctx.site_host)) {
		d.block = true;
		return d;
	}

	// Per-origin scripts: block scripts served from a host whose JS is blocked.
	if (ctx.kind == resource_kind::script &&
	    !m_engine->is_allowed(feature::javascript, ctx.request_host)) {
		d.block = true;
		return d;
	}

	// Images: block per site.
	if (ctx.kind == resource_kind::image &&
	    !m_engine->is_allowed(feature::images, ctx.site_host)) {
		d.block = true;
		return d;
	}

	// Referer: strip when the site's Referer policy is block.
	d.strip_referer = !m_engine->is_allowed(feature::referer, ctx.site_host);
	return d;
}

bool request_filter::allow_cookie(const QString &site_host, bool third_party) const {
	if (!m_engine)
		return true;
	if (third_party &&
	    !m_engine->is_allowed(feature::third_party_cookies, site_host))
		return false;
	return m_engine->is_allowed(feature::cookies, site_host);
}

void request_filter::notify(const request_context &ctx, const request_decision &d) const {
	for (request_observer *o : m_observers)
		o->on_request(ctx, d);
}
