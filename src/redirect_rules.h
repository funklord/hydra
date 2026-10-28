#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QReadWriteLock>
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
};
}

struct redirect_rule {
	QString     pattern;     // the network half, as the list wrote it
	// Compiled once, by `filter_list::pattern_regex`.
	QRegularExpression match;
	QString     host;        // for `||host...` patterns, the index key
	QString     resource;    // canonical name
	int         priority = 0;
	bool        rule_only = false;   // `redirect-rule=`: only where blocked anyway
	unsigned    types = 0;           // 0: any
	QStringList include;             // domain= sites, empty: any
	QStringList exclude;             // domain=~ sites
	int         party = 0;           // 0 any, 1 first-party only, 3 third-party only
};

// **Parses one line as a redirect rule.** Returns false with `why` empty for
// a line that is not one (no `redirect=` option), and false with `why` set
// for one that is but cannot be honoured: an option this reader does not
// know, or a resource this build does not carry. Refused rather than
// approximated, since an option narrows a rule and dropping it widens it.
bool parse_redirect_rule(const QString &line, redirect_rule *out, QString *why);

// The rules in force, swapped whole under a lock: the interceptor reads them
// on its own thread while a fetch replaces them on the UI thread.
class redirect_set {
public:
	void replace(const QList<redirect_rule> &rules);
	int size() const;

	// The resource to answer this request with, or empty. `blocked` says
	// whether something else would block the request anyway, which is the
	// only case a `redirect-rule=` applies to; a `redirect=` rule applies on
	// its own, since in uBlock it blocks as well. Highest priority wins.
	QString resource_for(const QString &url, const QString &request_host,
	                      const QString &site_host, unsigned type,
	                      bool blocked) const;

private:
	QList<redirect_rule>       m_rules;
	QHash<QString, QList<int>> m_by_host;
	QList<int>                 m_unhosted;
	mutable QReadWriteLock     m_lock;
};
