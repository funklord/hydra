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
	// A font, named outright by the Accept type, is its own kind: a broad
	// filter-list rule meant for a tracker matches a font URL by substring, and
	// blocking a webfont turns a page's icon glyphs into their ligature names --
	// which is why ad blockers do not block fonts by default.
	if (a.contains(QLatin1String("font/")) ||
	    a.contains(QLatin1String("application/font")) ||
	    a.contains(QLatin1String("application/vnd.ms-fontobject")))
		return resource_kind::font;
	// `text/css`, `text/html` and friends are "other" as far as the rules go,
	// and saying so early keeps the path guessing below from firing on a
	// stylesheet that happens to live under /js/.
	if (a.startsWith(QLatin1String("text/")))
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
	static const QStringList font_suffixes = {
		QStringLiteral(".woff2"), QStringLiteral(".woff"), QStringLiteral(".ttf"),
		QStringLiteral(".otf"),   QStringLiteral(".eot"),
	};
	for (const QString &s : font_suffixes)
		if (path.endsWith(s))
			return resource_kind::font;
	return resource_kind::other;
}

bool request_filter::is_ad_host(const QString &host) const {
	for (const QString &ad : m_ad_hosts)
		if (host == ad || host.endsWith("." + ad))
			return true;
	return false;
}

bool request_filter::blocks_popup(const QUrl &url, const QString &opener_host) const {
	if (!m_engine || m_rules.size() == 0 || !url.isValid())
		return false;
	if (m_engine->is_allowed(feature::ads, opener_host) || m_rules.site_allowed(opener_host))
		return false;
	network_rule_set::request q;
	q.host = url.host();
	q.site = opener_host;
	q.type = request_type::popup;
	q.url = url.toString();
	q.lower = q.url.toLower();
	const network_rule_set::verdict v = m_rules.evaluate(q);
	return v.block && (!v.allow || v.important);
}

QUrl request_filter::clean_navigation(const QUrl &url) const {
	const QString host = url.host();
	if (!m_engine || m_rules.size() == 0 ||
	    m_engine->is_allowed(feature::ads, host) || m_rules.site_allowed(host))
		return QUrl();
	network_rule_set::request q;
	q.url = url.toString(QUrl::FullyEncoded);
	q.lower = q.url.toLower();
	q.host = host;
	q.site = host;
	q.type = request_type::other;
	// A skip first, then the removals on wherever it leads -- Android loads
	// the answer without asking again, so it has to be finished.
	const QString skipped = m_rules.skip(q);
	if (!skipped.isEmpty()) {
		const QUrl to(skipped, QUrl::StrictMode);
		if (m_engine->is_allowed(feature::ads, to.host()) || m_rules.site_allowed(to.host()))
			return to;
		q.url = skipped;
		q.lower = q.url.toLower();
		q.host = q.site = to.host();
	}
	const QString clean = m_rules.clean(q);
	if (clean.isEmpty())
		return skipped.isEmpty() ? QUrl() : QUrl(skipped, QUrl::StrictMode);
	return QUrl(clean, QUrl::StrictMode);
}

request_decision request_filter::decide(const request_context &ctx) const {
	request_decision d;
	if (!m_engine)
		return d;

	// **The subscribed lists, as uBlock reads them** (`network_rules.h`):
	//
	//  1. a `$document` exception exempts the page whole;
	//  2. a `redirect=` rule answers before anything refuses -- which is what
	//     makes `ad_status.js` arrive as uBlock's stand-in rather than as a
	//     failure YouTube reads as an ad blocker;
	//  3. a block -- the ad-host list, the subscribed plain rules, a rule with
	//     options -- is lifted by an exception unless it is `$important`, and
	//     answered by a `redirect-rule=` where one names it.
	//
	// The person's own list stays outside the exceptions: a rule they
	// accepted on purpose is not one a list's exception may overrule.
	const bool ads_here = m_engine->is_allowed(feature::ads, ctx.site_host);
	network_rule_set::request q;
	q.host = ctx.request_host;
	q.site = ctx.site_host;
	q.type = ctx.type;
	bool have_url = false;
	const auto url = [&]() -> const QString & {
		if (!have_url) {
			q.url = ctx.url.toString();
			q.lower = q.url.toLower();
			have_url = true;
		}
		return q.url;
	};
	const bool rules = m_rules.size() > 0;
	const bool blocking = !ads_here && !(rules && m_rules.site_allowed(ctx.site_host));
	// What the rules with options say, asked once and only if needed.
	network_rule_set::verdict v;
	bool evaluated = false;
	const auto verdict = [&]() -> const network_rule_set::verdict & {
		if (!evaluated) {
			url();
			v = m_rules.evaluate(q);
			evaluated = true;
		}
		return v;
	};
	if (blocking && rules && !verdict().redirect.isEmpty() && !verdict().allow) {
		d.block = true;
		d.redirect = verdict().redirect;
		return d;
	}
	// A list's block: lifted by an exception unless `important`, and
	// otherwise answered by a `redirect-rule=` where one names it.
	const auto refused = [&](bool important) {
		if (rules) {
			if (!important && verdict().allow)
				return d;
			d.redirect = verdict().redirect_rule;
		}
		d.block = true;
		return d;
	};

	// Ads / trackers: block known ad hosts unless this site permits ads.
	if (blocking && is_ad_host(ctx.request_host))
		return refused(false);

	// The accepted filter rules, which are ads and annoyances by another name and
	// so answer to the same per-site switch: turning ads back on for a site the
	// shield says is broken has to turn *all* of this off, or the escape hatch
	// only half works and the page still fails for a reason the user was told
	// they had disabled.
	// Fonts are spared the plain rules: a rule with no type option applies to
	// any font URL it matches by substring, and a blocked webfont shows a
	// page's icons as their ligature text. A font from a known ad host is still
	// blocked above by is_ad_host, and a rule that names `font` still applies.
	if (ctx.kind != resource_kind::font && m_list && !ads_here &&
	    m_list->blocks(url(), ctx.site_host)) {
		d.block = true;
		return d;
	}
	if (blocking) {
		if (ctx.kind != resource_kind::font && m_subscribed &&
		    m_subscribed->blocks(url(), ctx.site_host))
			return refused(false);
		if (rules && verdict().block)
			return refused(verdict().important);
	}

	// The investigation's trial rules, for its site only.
	if (ctx.kind != resource_kind::font) {
		QString site;
		{
			QMutexLocker guard(&m_trial_lock);
			site = m_trial_site;
		}
		if (!site.isEmpty() && filter_list::scope_matches(site, ctx.site_host) &&
		    m_trial.blocks(ctx.url.toString(), ctx.site_host)) {
			d.block = true;
			return d;
		}
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

void request_filter::set_trial(const QString &site_host,
                               const QList<filter_rule> &rules) {
	QList<filter_rule> network;
	for (const filter_rule &r : rules)
		if (!r.cosmetic && !r.scriptlet)
			network << r;
	m_trial.replace(network);
	QMutexLocker guard(&m_trial_lock);
	m_trial_site = network.isEmpty() ? QString() : site_host;
}

