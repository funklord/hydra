#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QUrl>

class policy_engine;
class filter_list;

// The kinds of request every engine can distinguish. Qt WebEngine reports far
// more resource types and Android's WebView reports fewer; this is the subset
// the policy rules actually key off, so both can map onto it.
enum class resource_kind { script, image, other };

// What the shell wants done with a request, as flags rather than an action, so
// a backend can honour the parts it supports.
struct request_decision {
	bool block         = false;
	bool strip_referer = false;
};

// One request, reduced to what a decision needs -- plus the full URL, which
// the decision ignores but the media detector (sec 11.1) reads for extensions
// and manifest paths.
struct request_context {
	QString       request_host;   // host the request goes to
	QString       site_host;      // host of the page making it
	QUrl          url;            // the full request URL
	resource_kind kind = resource_kind::other;
};

// What a request is, worked out from the `Accept` header and the URL.
//
// Qt WebEngine states the resource type outright; Android's `WebResourceRequest`
// does not, and offers only headers and a url. So the Android interceptor has to
// infer it, and this is where that inference lives -- shared and testable rather
// than buried in a platform file, because a wrong guess here silently turns a
// per-origin script rule into no rule at all.
//
// **Deliberately cautious.** A script request sends `Accept: */*`, but so does
// every `fetch()` and XHR, so `*/*` alone is not taken as evidence: only an
// explicit javascript media type or a `.js`/`.mjs` path counts. The cost of
// guessing low is that some scripts load on a site whose scripts are blocked;
// the cost of guessing high would be blocking a page's data requests under a
// rule the user set for scripts, which looks like the site being broken.
resource_kind kind_from_hints(const QString &accept, const QUrl &url);

// The interceptor is a shared *sensor*, not just a gate (architecture doc sec 10):
// ad-blocking, media detection, and filter-evolution signal collection all ride
// the same stream of requests. Observers see every request and its decision.
//
// Thread note: on_request() is called from wherever the platform's interceptor
// runs, which on Qt WebEngine is not the UI thread. Implementations must be
// safe to call concurrently and must not touch widgets directly.
class request_observer {
public:
	virtual ~request_observer() = default;
	virtual void on_request(const request_context &ctx, const request_decision &d) = 0;
};

// The platform-neutral half of request interception (architecture doc sec 7.3,
// sec 19.5). Deciding what to block is identical on every platform because it is
// just policy plus a host list; only the plumbing that delivers requests
// differs, so that plumbing stays in the per-platform interceptor and this
// stays shared.
//
// Thread note: decide() may be called off the UI thread. It only reads the
// policy engine, which is mutated on the UI thread and tolerates a stale
// snapshot.
class request_filter {
public:
	explicit request_filter(policy_engine *engine);

	// The AI/user-authored list (sec 12), consulted on every request.
	//
	// **This was the gap.** The filter-evolution loop proposed rules, the dry-run
	// checked them, the user accepted them and they were written to
	// `filters-ai.txt` and listed in settings -- and nothing ever asked them about
	// a request. `filter_list::blocks()` existed with no caller in the request
	// path, so the whole loop's output was decoration. The architecture puts
	// filter enforcement on spine 1, the interceptor, which is here.
	//
	// Optional: a filter with no list behaves exactly as before.
	void set_filter_list(const filter_list *list) { m_list = list; }

	request_decision decide(const request_context &ctx) const;

	// Cookie decisions are the same shape: policy plus the first-party host.
	bool allow_cookie(const QString &site_host, bool third_party) const;

	bool is_ad_host(const QString &host) const;

	// Observers are registered on the UI thread before browsing starts and are
	// not removed while requests are in flight.
	// A null here is a construction-order mistake at the call site, and keeping
	// it turns that into undefined behaviour on some later request rather than a
	// mistake where it was made. It has happened once: an observer registered
	// before it was constructed was harmless for as long as the function it
	// landed in touched no members, then segfaulted every live driver the day
	// one did.
	void add_observer(request_observer *o) { if (o) m_observers.push_back(o); }
	void remove_observer(request_observer *o) { m_observers.removeAll(o); }
	// **How many are registered, so the pairing can be asserted.** For most of
	// this class's life `add_observer` had a counterpart that nothing called,
	// and the only way to notice was to read both. A filter that outlives a
	// window then kept that window's observers after they were destroyed, and
	// `notify` dereferences every entry unconditionally.
	//
	// Nothing about the list was observable from outside, so the invariant --
	// a window hands back exactly what it registered -- could not be checked
	// by anything but a person reading two files. It can now.
	int observer_count() const { return int(m_observers.size()); }
	void notify(const request_context &ctx, const request_decision &d) const;

private:
	policy_engine     *m_engine;
	const filter_list *m_list = nullptr;
	QSet<QString>  m_ad_hosts;
	QList<request_observer *> m_observers;
};
