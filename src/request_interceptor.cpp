// SPDX-License-Identifier: GPL-3.0-or-later
#include "request_interceptor.h"
#include "policy_engine.h"

#include <QWebEngineUrlRequestInfo>

using policy::feature;

request_interceptor::request_interceptor(policy_engine *engine, QObject *parent)
	: QWebEngineUrlRequestInterceptor(parent), m_engine(engine) {
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

bool request_interceptor::is_ad_host(const QString &host) const {
	for (const QString &ad : m_ad_hosts)
		if (host == ad || host.endsWith("." + ad))
			return true;
	return false;
}

void request_interceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
	if (!m_engine)
		return;

	const QString req_host  = info.requestUrl().host();
	const QString site_host = info.firstPartyUrl().host();  // the page's site
	const auto    type      = info.resourceType();

	using RT = QWebEngineUrlRequestInfo;

	// Ads / trackers: block known ad hosts unless this site permits ads.
	if (is_ad_host(req_host) && !m_engine->is_allowed(feature::ads, site_host)) {
		info.block(true);
		return;
	}

	// Per-origin scripts: block scripts served from a host whose JS is blocked.
	if (type == RT::ResourceTypeScript &&
	    !m_engine->is_allowed(feature::javascript, req_host)) {
		info.block(true);
		return;
	}

	// Images: block per site.
	if (type == RT::ResourceTypeImage &&
	    !m_engine->is_allowed(feature::images, site_host)) {
		info.block(true);
		return;
	}

	// Referer: strip when the site's Referer policy is block.
	if (!m_engine->is_allowed(feature::referer, site_host)) {
		info.setHttpHeader(QByteArray("Referer"), QByteArray());
	}
}
