// SPDX-License-Identifier: GPL-3.0-or-later
#include "request_filter.h"
#include "policy_engine.h"

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
