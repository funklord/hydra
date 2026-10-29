#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QHash>

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
// **Generic hiding from the subscribed lists**, and what lifts it.
//
// `##sel` with no site hides `sel` everywhere. A rule a person -- or the
// filter-evolution loop -- proposes is never allowed that reach: a rule
// with no scope is refused at accept time and not applied here, because an
// unbounded rule is what the review exists to stop. A subscribed list is a
// different source. Its generic rules are its ordinary mechanism -- EasyList
// carries 13,634 -- and they come with their own limits, which are honoured:
// `site#@#sel` lifts one selector on a site (no site: everywhere),
// `$generichide` turns generic hiding off on a site, and `$elemhide` turns
// every cosmetic rule off there. Without any of this a blocked ad left its
// reserved space standing: fourteen blank screens down Expressen's front
// page on a phone (2026-10-29).
//
// Built once per load of the lists and shared by every view, since it is
// the same for all of them; `for_host` is what one page gets.
struct generic_cosmetics {
	QStringList                     selectors;   // global exceptions removed
	QHash<QString, QSet<QString>>   excepted;    // host -> selectors lifted there
	QSet<QString>                   generichide; // hosts with generic hiding off
	QSet<QString>                   elemhide;    // hosts with all hiding off

	void add_exception(const QString &host, const QString &selector);
	// The rule hosts apply to their subdomains, as network rules do.
	static bool covers(const QSet<QString> &hosts, const QString &host);
	bool hides_nothing_on(const QString &host) const { return covers(elemhide, host); }
	// The generic selectors that apply on `host`: none where generic hiding is
	// off, and the rest minus whatever is lifted on the host or a parent.
	QStringList for_host(const QString &host) const;
};

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
	// The subscribed lists' generic rules (`generic_cosmetics`). Null: none.
	void set_generic(const generic_cosmetics *generic) { m_generic = generic; }

	// An investigation's trial rules for this tab, served after the rest
	// until it ends. Null means none.
	void set_trial_list(const filter_list *list) { m_trial = list; }

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
	const generic_cosmetics *m_generic = nullptr;
	const filter_list   *m_trial = nullptr;
	const policy_engine *m_policy = nullptr;
	QString              m_host;
};
