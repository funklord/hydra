#pragma once

#include "filter_list.h"
#include "scriptlets.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

// Subscribing to an upstream filter list (architecture doc sec 12.5).
//
// **The list this produces is kept apart from the user's own**, which
// `filter_list`'s own header asked for before anything could subscribe: "the
// AI/user-authored filter list, kept deliberately apart from any imported
// EasyList so a scheduled upstream update never clobbers custom rules". Two
// `filter_list` instances, two files, and `request_filter` consults both.
//
// This half is deliberately free of the network. Reading a list's text,
// deciding what of it this build can enforce, and refusing a body that is not
// a filter list at all are the parts that have to be right, and they are the
// parts a test can reach without a server.
struct subscription {
	QString   name;            // "EasyList"
	QUrl      url;
	bool      enabled = true;
	QDateTime fetched;         // when a fetch was last promoted
	int       rules   = 0;     // how many this build kept from it
	QString   note;            // the last outcome, for the settings line
	// **Whether the trusted scriptlets in this list may run.** Off unless
	// somebody says otherwise, per subscription rather than per build: a
	// trusted scriptlet writes a cookie or replaces a response body on the
	// page's behalf, and a subscribed list is tens of thousands of lines
	// nobody here read one at a time. Trusting the publisher is a thing a
	// person can decide; trusting a line is not.
	bool      trusted = false;
	// **The cached body's filename, minted when the subscription is added and
	// then never derived again.** Deriving it from the name would move the
	// cache every time somebody renamed a subscription, and deriving it from
	// the url needs a collision rule for two lists on one host. Storing it
	// needs neither, and a rename costs nothing.
	QString   file;
};

// What reading one list's text produced.
//
// **The counts are reported rather than summed into one number**, because
// "14,000 rules" and "38,000 lines this build cannot enforce" are different
// facts and a person deciding whether a subscription is worth having needs the
// second one. A single total would read as coverage this does not have.
struct subscription_read {
	int lines       = 0;   // candidate lines: not blank, not a comment or header
	int accepted    = 0;   // rules this build can actually enforce
	int unsupported = 0;   // syntax this build does not implement
	int unsafe      = 0;   // cosmetic selectors refused outright
	// **Counted apart from `accepted`, because a scriptlet is not a rule.**
	// One is matched against a request or a selector; the other is a patch
	// applied to the page's own globals from a closed catalog. Summing them
	// would hide the number that says how much of a list is reaching the one
	// place a network rule cannot -- an ad served from the content's host.
	int scriptlets  = 0;
	// **Scriptlets dropped for want of trust, counted rather than ignored.**
	// A list whose useful half is in its trusted rules and a list that
	// genuinely has none look identical from a silent drop, and the second
	// is the only one where turning trust on would change anything. The
	// number is what the settings line needs in order to say so.
	int needs_trust = 0;
	QString refusal;       // non-empty: the body was not a usable filter list
	QList<filter_rule> rules;
	QList<scriptlet_call> calls;

	bool ok() const { return refusal.isEmpty(); }
	// One line for the settings list and the log, naming all three numbers.
	QString summary() const;
};

namespace filter_subscription {

// Read one list's text. `previous_rules` is how many the last promoted fetch
// of the same subscription produced, or 0 when there is none; it is used only
// by the shrink guard below.
//
// **A refusal is the whole body refused, and the cache is then left alone.**
// The failure this is written against is not a corrupt rule, it is a 200 with
// the wrong thing in it -- a captive portal's login page, a CDN error page, a
// repository that moved and now serves HTML. Those parse as nothing and would
// otherwise promote an empty list over a working one, which turns ad blocking
// off silently and looks exactly like an upstream that got quieter.
// `trusted` is the subscription's own flag. It decides only whether the
// trusted class of scriptlets is kept: everything else reads the same either
// way, so turning it on never changes what a network rule does.
subscription_read read(const QString &text, int previous_rules = 0,
                        bool trusted = false);

// The subscription list itself, as JSON beside the cached bodies.
//
// **Only the index is written here; the bodies are kept as fetched.** Caching
// the accepted rules instead would be smaller and would put the gate on the
// fetch path only -- so a build that later learns to read a rule's options
// could not use what is already on disk without re-fetching every list, and
// nothing could recount what a list offered against what was taken from it.
// Re-reading the body through `read()` on every load costs a parse and keeps
// both.
QList<subscription> load_index(const QString &path);
bool save_index(const QString &path, const QList<subscription> &subs);

// A filename for a new subscription's cached body, free in `dir`.
QString mint_cache_name(const QString &dir, const QString &name);

// **What a fresh install subscribes to, and why these two.** Until this
// existed a new install enforced nothing at all: there was no default, and
// Add asked for an address somebody had to already know.
//
// Two rather than one, because they do different jobs and the measurement
// says so. Read through `read()` on 2026-10-08:
//
//     EasyList            80142 candidate lines -> 58400 rules,     0 scriptlets
//     uBlock filters.txt   6124 candidate lines ->  1802 rules,  1680 scriptlets
//
// **EasyList carries no scriptlets at all** -- not one `##+js(` line in
// 80418 -- so on its own it cannot touch an ad served from the content's own
// host, which is the YouTube case. uBlock's list is where those rules live.
// Neither covers the other, so shipping one would have left a gap nobody
// could see from the settings page.
//
// Enabled, because a default that is off is the same as no default: the
// thing being fixed is that a fresh install blocked nothing. Not trusted --
// trust is a statement about a publisher that only the person can make, and
// shipping a URL pre-trusted would make it on their behalf.
QList<subscription> default_subscriptions();

// Which of the four buckets a single line falls in. Exposed for the tests,
// because the classification is the part with the judgements in it.
enum class line_kind {
	comment,       // blank, `!` or `[Adblock...]`
	network,       // a rule this build enforces
	cosmetic,      // a scoped element-hiding rule with a usable selector
	scriptlet,     // `##+js(name, ...)` naming a scriptlet in the catalog
	unsupported,   // real syntax this build does not implement
	unsafe,        // a cosmetic selector refused by why_selector_unsafe
};
line_kind classify(const QString &line, filter_rule *out = nullptr,
                    QString *why = nullptr, scriptlet_call *call = nullptr);

}  // namespace filter_subscription
