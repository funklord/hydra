#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QReadWriteLock>
#include <QSet>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// **A refused request answered with something harmless, rather than refused.**
//
// uBlock's `redirect=` option. A blocked request is visible to the page as a
// failure, and checking for that failure is the oldest way a site notices an
// ad blocker: load something an ad stack would load, and see whether it
// arrived. YouTube does exactly this with `ad_status.js`, which sets
// `window.google_ad_status` -- and on 2026-10-25 the holder's report showed
// this browser refusing it while YouTube said "Ad blockers are not allowed".
// uBlock's own list answers it with
//
//     ||doubleclick.net/instream/ad_status.js^$script,xhr,redirect=doubleclick_instream_ad_status.js:5
//
// a stand-in that does what the real file does and nothing else. This build
// dropped the rule, having no way to answer a request, and its blanket
// `doubleclick.net` block refused it outright.
//
// **The resources are this project's and the set is closed**, as with the
// scriptlet catalog: a rule names one; it cannot supply one. A rule naming
// any other is refused and counted. They are deliberately dull -- an empty
// script, an empty text, a transparent image -- except the one that has to
// say something, and that says only what the real file says.
namespace redirect_resources {

struct resource {
	QByteArray mime;
	QByteArray body;
};

// The resource a rule names, by uBlock's name or one of its aliases. False
// for one this build does not carry.
bool find(const QString &name, resource *out = nullptr);

// The canonical names, for a test to check against.
QStringList names();

}  // namespace redirect_resources

// What a request is, as uBlock's type options name it. A request a backend
// can only describe coarsely is `unknown`, which any type option accepts --
// the alternative is a rule that never fires where the type cannot be told.
namespace request_type {
enum : unsigned {
	unknown     = 0,
	script      = 1u << 0,
	image       = 1u << 1,
	stylesheet  = 1u << 2,
	xhr         = 1u << 3,
	subdocument = 1u << 4,
	media       = 1u << 5,
	font        = 1u << 6,
	ping        = 1u << 7,
	other       = 1u << 8,
	// A new window the page opens. Only a rule that names `$popup` decides
	// one, and such a rule decides nothing else: a plain block is about
	// requests, and Android describes many requests as `unknown`, which a
	// popup rule must not reach.
	popup       = 1u << 9,
};
}

// **Rules with options, which the block list cannot carry.** `filter_list`
// matches a URL and nothing else, so a rule that says what kind of request,
// on which sites, or first- or third-party had no home and was dropped -- 15
// thousand lines of the holder's lists, measured 2026-10-29. Three actions
// live here, sharing one reader of the options:
//
//  * **block**: `||cdn.example^$script,third-party`;
//  * **allow**: `@@...`, an exception. None was read before, so uBlock's
//    Unbreak list -- 1283 exceptions written to repair pages blockers break
//    -- did nothing here, and every page it fixes stayed broken. An exception
//    outranks every block that is not `$important`, the ad-host list and the
//    plain lists included. `@@...$document` exempts a whole site;
//  * **redirect**: `$redirect=` and `$redirect-rule=`, above;
//  * **removeparam**: `$removeparam=utm_source` takes a parameter out of a
//    page's address -- by name, `/regex/` against `name=value`, `~name` for
//    all but that one, or bare for all. 269 lines of the default lists,
//    almost all of them trackers' parameters. An `@@` line with it lifts
//    the same removal (bare: every removal) and nothing else, which is why
//    it is its own action: read as an ordinary exception it would have
//    lifted blocks. Applied to navigations only -- the page and its frames,
//    uBlock's default when no type is named -- since a request a page makes
//    for itself was built with its parameters on purpose, and Android can
//    change only where a navigation goes. A rule naming another type is
//    kept and not applied.
//
// `$badfilter` cancels the rule it repeats and is applied across every list
// by whoever assembles them (`apply_badfilters`), since the rule it cancels
// is usually in another list.
struct network_rule {
	enum class action { block, allow, redirect, removeparam };
	action      act = action::block;
	QString     text;        // the line, for $badfilter to name it by
	QString     pattern;     // the network half, as the list wrote it
	QString     host;        // for `||host...` patterns, the index key
	QString     resource;    // for a redirect: the canonical name
	QString     param;       // for removeparam: name, /regex/, ~name, or empty for all
	bool        exception = false;   // removeparam: an `@@` line, lifting one
	int         priority = 0;
	bool        rule_only = false;   // `redirect-rule=`: only where blocked anyway
	bool        important = false;   // a block no exception reaches
	bool        document = false;    // an allow for the whole page (`$document`)
	unsigned    types = 0;           // 0: any
	unsigned    not_types = 0;       // `~type`
	QStringList include;             // domain= sites, empty: any
	QStringList exclude;             // domain=~ sites
	int         party = 0;           // 0 any, 1 first-party only, 3 third-party only
	// Compiled once, by `filter_list::pattern_regex`.
	QRegularExpression match;
	QString            literal;   // `filter_list::literal_of`, tested first
};

// The parse of one line. `option` is a rule this file carries; `badfilter`
// names a rule to cancel, in `target`; `none` is a line that is not one of
// these at all (plain, cosmetic, a comment) and is someone else's.
// `refused` is one that is, but cannot be honoured, with `why` saying so:
// an option this reader does not know, or a resource not in the catalog.
// Refused rather than approximated, since an option narrows a rule and
// dropping it widens it.
enum class parsed { none, option, badfilter, refused };
parsed parse_network_rule(const QString &line, network_rule *out, QString *why,
                          QString *target = nullptr);

// **Drops every rule a `$badfilter` names**, by text, from both kinds of
// list: the plain rules `filter_list` keeps and the ones here.
void apply_badfilters(const QSet<QString> &cancelled, QList<network_rule> *rules,
                      QStringList *plain);

// The rules in force, swapped whole under a lock: the interceptor reads them
// on its own thread while a fetch replaces them on the UI thread.
class network_rule_set {
public:
	void replace(const QList<network_rule> &rules);
	int size() const;
	int count(network_rule::action a) const;

	// One request, as the rules see it.
	struct request {
		QString  url;
		QString  lower;     // `url` lowercased, once, for the literal checks
		QString  host;      // the request's
		QString  site;      // the page's
		unsigned type = 0;  // request_type, or unknown
	};
	// **Everything the rules say about one request, in one pass** over its
	// candidates: what `decide` needs, asked once rather than three times.
	struct verdict {
		QString redirect;        // a `redirect=` rule's resource
		QString redirect_rule;   // a `redirect-rule=` rule's, for a block
		bool    block = false;
		bool    important = false;
		bool    allow = false;
	};
	verdict evaluate(const request &r) const;

	// The resource to answer this request with, or empty. `blocked` says
	// whether something else would block it anyway, the only case a
	// `redirect-rule=` applies to. Highest priority wins.
	QString resource_for(const request &r, bool blocked) const;
	// Whether a block rule here matches; `important` says one that no
	// exception may lift.
	bool blocks(const request &r, bool *important = nullptr) const;
	// Whether an exception lifts a block for this request.
	bool allows(const request &r) const;
	// The address with what `removeparam` rules take out of it removed, or
	// empty when they take nothing. For a navigation: see the action.
	QString clean(const request &r) const;
	// Whether a `$document` exception exempts this page whole.
	bool site_allowed(const QString &site) const;

private:
	template <typename F> void each_candidate(const request &r, F f) const;
	bool applies(const network_rule &rule, const request &r, bool same_party) const;

	QList<network_rule>        m_rules;
	QHash<QString, QList<int>> m_by_host;    // `||host` patterns
	QHash<QString, QList<int>> m_by_site;    // no host, but domain= sites
	QList<int>                 m_params;     // removeparam rules, asked apart
	QHash<QString, QList<int>> m_by_token;   // neither: by a token of the pattern
	// **No token either, filed by three characters of its literal.** Patterns
	// like `.com/ad/` have no run of four to file by, and as one list tried
	// on every request they were 185 of the 212 rules a decision looked at
	// (measured 2026-10-29). A request looks up each three characters of its
	// address instead, as one integer each.
	QHash<quint64, QList<int>> m_by_gram;
	QList<int>                 m_rest;       // nothing to file by: always tried
	// `@@...$document`, by host where the pattern names one -- asked on every
	// request, so not a list to walk -- and the rest.
	QHash<QString, QList<int>> m_documents_by_host;
	QList<int>                 m_documents;
	mutable QReadWriteLock     m_lock;
};
