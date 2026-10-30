#include "qtwebengine_interceptor.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"

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
		case RT::ResourceTypeFontResource: ctx.kind = resource_kind::font; break;
		default:                     ctx.kind = resource_kind::other;  break;
	}
	// uBlock's finer type, for a rule's type options: the engine says it.
	switch (info.resourceType()) {
		case RT::ResourceTypeScript:
		case RT::ResourceTypeWorker:
		case RT::ResourceTypeSharedWorker:
		case RT::ResourceTypeServiceWorker: ctx.type = request_type::script; break;
		case RT::ResourceTypeImage:
		case RT::ResourceTypeFavicon:       ctx.type = request_type::image; break;
		case RT::ResourceTypeStylesheet:    ctx.type = request_type::stylesheet; break;
		case RT::ResourceTypeXhr:           ctx.type = request_type::xhr; break;
		case RT::ResourceTypeSubFrame:      ctx.type = request_type::subdocument; break;
		case RT::ResourceTypeMedia:         ctx.type = request_type::media; break;
		case RT::ResourceTypeFontResource:  ctx.type = request_type::font; break;
		case RT::ResourceTypePing:
		case RT::ResourceTypeCspReport:     ctx.type = request_type::ping; break;
		default:                            ctx.type = request_type::other; break;
	}

	request_decision d = m_filter->decide(ctx);
	// **A page the person chose to open past a list's warning** is not then
	// refused as a request: a `$all` rule blocks the page's own document as
	// well as everything else it names, and the warning's way through
	// (`request_filter::proceed`) is for the page.
	if (d.block && info.resourceType() == RT::ResourceTypeMainFrame &&
	    m_filter->proceeded(ctx.request_host))
		d = request_decision();
	// Every request reaches the observers, blocked or not: the media detector
	// wants what loaded, and filter-evolution wants what slipped through.
	m_filter->notify(ctx, d);
	if (d.block) {
		// Answered rather than refused, where a redirect rule names a
		// stand-in and the scheme that serves it is in place.
		if (!d.redirect.isEmpty() && qtwebengine_factory::resource_scheme_registered())
			info.redirect(qtwebengine_factory::resource_url(d.redirect));
		else
			info.block(true);
		return;
	}
	if (d.strip_referer)
		info.setHttpHeader(QByteArray("Referer"), QByteArray());
	// **A page or a frame arriving with a tracker's parameters** goes to the
	// address without them (`request_filter::clean_navigation`). A redirect
	// is asked about again, and the clean address has nothing left to take.
	if (info.resourceType() == RT::ResourceTypeMainFrame ||
	    info.resourceType() == RT::ResourceTypeSubFrame) {
		const QUrl clean = m_filter->clean_navigation(ctx.url);
		if (clean.isValid() && clean != ctx.url)
			info.redirect(clean);
	}
}
