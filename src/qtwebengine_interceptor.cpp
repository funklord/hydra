// SPDX-License-Identifier: GPL-3.0-or-later
#include "qtwebengine_interceptor.h"
#include "request_filter.h"

#include <QWebEngineUrlRequestInfo>

qtwebengine_interceptor::qtwebengine_interceptor(request_filter *filter, QObject *parent)
	: QWebEngineUrlRequestInterceptor(parent), m_filter(filter) {
}

void qtwebengine_interceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
	if (!m_filter)
		return;

	using RT = QWebEngineUrlRequestInfo;
	request_context ctx;
	ctx.request_host = info.requestUrl().host();
	ctx.site_host    = info.firstPartyUrl().host();   // the page's site
	ctx.url          = info.requestUrl();
	switch (info.resourceType()) {
		case RT::ResourceTypeScript: ctx.kind = resource_kind::script; break;
		case RT::ResourceTypeImage:  ctx.kind = resource_kind::image;  break;
		default:                     ctx.kind = resource_kind::other;  break;
	}

	const request_decision d = m_filter->decide(ctx);
	// Every request reaches the observers, blocked or not: the media detector
	// wants what loaded, and filter-evolution wants what slipped through.
	m_filter->notify(ctx, d);
	if (d.block) {
		info.block(true);
		return;
	}
	if (d.strip_referer)
		info.setHttpHeader(QByteArray("Referer"), QByteArray());
}
