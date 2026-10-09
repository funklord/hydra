#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class filter_list;
class policy_engine;

// The cosmetic half of the filter-evolution loop (architecture doc sec 12).
//
// A `##` rule hides an element rather than blocking a request, so it cannot ride
// the interceptor the way `||host^` does -- it has to reach the page. This is the
// piece that takes it there: the shell tells it which host is on screen, an
// injected script asks it for that host's selectors, and the script writes them
// into a stylesheet.
//
// **It was missing entirely.** Accepted cosmetic rules were stored, listed and
// re-loaded, and hid nothing -- the same gap the network half had, in the same
// loop, found by writing down that the network half was fixed and the cosmetic
// half was not.
//
// The page never names the host, exactly as with the consent bridge: the shell
// sets it on navigation, and `selectors_json()` answers for whatever that is.
// Otherwise any page could ask what rules exist for any site, which is a small
// leak of what the user has been doing.
class cosmetic_filters : public QObject {
	Q_OBJECT
public:
	// **The policy, because the shield's escape hatch has to reach here too.**
	// `request_filter` states the rule this class was breaking: "turning ads
	// back on for a site the shield says is broken has to turn *all* of this
	// off, or the escape hatch only half works and the page still fails for a
	// reason the user was told they had disabled." Network rules stopped for
	// such a site and cosmetic ones kept hiding elements, so somebody who
	// allowed ads to un-break a page got a page that was still broken -- by us,
	// after they had turned off the thing that broke it.
	//
	// Optional, so a test or a driver can build one with no policy and get the
	// old behaviour: with none, every host is filtered.
	explicit cosmetic_filters(const filter_list *list,
	                           const policy_engine *policy = nullptr,
	                           QObject *parent = nullptr)
	    : QObject(parent), m_list(list), m_policy(policy) {}

	// The object name the injected script expects on the bridge.
	static const char *bridge_name() { return "hydraCosmetic"; }
	static QString script_source();

	// Set by the shell on navigation. Never by the page.
	void set_page_host(const QString &host);

	// **The subscribed lists' cosmetic rules**, which nothing applied: the
	// shell built this from the person's own list alone, so EasyList's
	// site-scoped hiding -- `dailymotion.com##div[class^="DisplayAd"]` and
	// thousands like it -- was read, counted as in use, and never reached a
	// page. Same name and shape as `request_filter`'s, which had it from the
	// start. Null means none.
	void set_subscription_list(const filter_list *list) { m_subscribed = list; }

	// Both lists' selectors for the current host, own rules first.
	QStringList selectors() const;

	// The selectors for one host, without the bridge. Shared with the tests, and
	// with anything that wants to know what would be hidden without asking a
	// live page.
	// `policy` may be null, which means "no per-site opinion" and filters
	// everything the list covers -- the behaviour before the shield reached here.
	static QStringList selectors_for(const filter_list *list, const QString &host,
	                                  const policy_engine *policy = nullptr);

	// For HYDRA_FILTER_DEBUG only.
	QString debug_state() const;

public slots:
	// A JSON array of CSS selectors for the host currently on screen.
	QString selectors_json() const;

private:
	const filter_list   *m_list   = nullptr;
	const filter_list   *m_subscribed = nullptr;
	const policy_engine *m_policy = nullptr;
	QString              m_host;
};
