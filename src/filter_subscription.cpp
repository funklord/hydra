#include "filter_subscription.h"

#include "site_rules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

QString subscription_read::summary() const {
	if (!ok())
		return refusal;
	QString s = QString("%1 rule(s) in use").arg(accepted);
	if (scriptlets > 0)
		s += QString(", %1 scriptlet(s)").arg(scriptlets);
	if (needs_trust > 0)
		s += QString(", %1 needing trust this list has not been given")
		         .arg(needs_trust);
	if (unsupported > 0)
		s += QString(", %1 line(s) this build cannot enforce").arg(unsupported);
	if (unsafe > 0)
		s += QString(", %1 refused as unsafe").arg(unsafe);
	return s;
}

namespace {

enum class rewrite { none, carried, refused };

// The url half of a `||host/path` pattern as a regex the page can test: the
// host or any subdomain of it, `*` as anything, `^` as a separator or the end.
QString url_regex(const QString &host, const QString &rest) {
	QString re = QStringLiteral("^https?:\\/\\/([^\\/]*\\.)?")
	           + QRegularExpression::escape(host).replace("/", "\\/");
	for (const QChar c : rest) {
		if (c == QLatin1Char('*'))
			re += QStringLiteral(".*");
		else if (c == QLatin1Char('^'))
			re += QStringLiteral("(?:[^\\w.%-]|$)");
		else if (c == QLatin1Char('/'))
			re += QStringLiteral("\\/");
		else
			re += QRegularExpression::escape(QString(c));
	}
	return QLatin1Char('/') + re + QLatin1Char('/');
}

// **`replace=` on a script request, carried in the page.** uBlock rewrites a
// response body with
//
//     ||youtube.com/youtubei/v1/get_watch?$xhr,1p,replace=/"adPlacements"/"no_ads"/
//
// and that is how its list keeps the ad payload out of the request YouTube
// makes when you move from one video to the next. The interceptor here cannot
// touch a response body, but a rule limited to `xhr` only ever applies to a
// request the page's own script made -- which is exactly what
// `trusted-replace-fetch-response` and its XHR twin already rewrite. So the
// rule becomes those two calls, scoped to its host and matched on its url.
//
// Narrow on purpose, and refused rather than widened otherwise: `||host`
// anchored, `1p` so the host is also the page it runs on, `xhr` as the only
// type, and `replace=` last, since its value may hold a comma. The value's
// regex and the url regex both get the backtracking refusal any pattern from
// a list gets. One difference from uBlock is stated rather than hidden: the
// shared rewriter always replaces every match, where uBlock honours a missing
// `g`.
rewrite response_rewrite(const QString &t, scriptlet_call *out, QString *why) {
	const int dollar = t.indexOf(QLatin1Char('$'));
	if (dollar < 0)
		return rewrite::none;
	const QString options = t.mid(dollar + 1);
	const int at = options.indexOf(QLatin1String("replace="));
	if (at < 0)
		return rewrite::none;
	const auto refuse = [why](const char *text) {
		if (why)
			*why = QString::fromLatin1(text);
		return rewrite::refused;
	};
	const QString pattern = t.left(dollar);
	if (!pattern.startsWith(QLatin1String("||")))
		return refuse("a response rewrite not anchored to a host");
	bool first_party = false, script_only = false;
	const QString head = options.left(at);
	for (const QString &o : head.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		if (o == QLatin1String("1p") || o == QLatin1String("first-party"))
			first_party = true;
		else if (o == QLatin1String("xhr") ||
		         o == QLatin1String("xmlhttprequest"))
			script_only = true;
		else
			return refuse("a response rewrite with an option this build "
			               "does not read");
	}
	if (!first_party || !script_only)
		return refuse("a response rewrite that is not first-party and "
		               "limited to script requests");

	// `/pattern/replacement/flags`, with `\/` for a slash inside either half.
	const QString value = options.mid(at + 8);
	QStringList parts;
	QString cur;
	for (int i = 0; i < value.size(); ++i) {
		const QChar c = value.at(i);
		if (c == QLatin1Char('\\') && i + 1 < value.size() &&
		    value.at(i + 1) == QLatin1Char('/')) {
			cur += QLatin1Char('/');
			++i;
		} else if (c == QLatin1Char('/')) {
			parts << cur;
			cur.clear();
		} else {
			cur += c;
		}
	}
	parts << cur;
	if (parts.size() != 4 || !parts.at(0).isEmpty() || parts.at(1).isEmpty())
		return refuse("a response rewrite whose value is not "
		               "/pattern/replacement/flags");
	const QString flags = parts.at(3);
	for (const QChar f : flags)
		if (!QStringLiteral("gimsu").contains(f))
			return refuse("a response rewrite with a flag this build does "
			               "not take");

	const QString path = pattern.mid(2);
	int end = 0;
	while (end < path.size() && path.at(end) != QLatin1Char('/') &&
	       path.at(end) != QLatin1Char('^') && path.at(end) != QLatin1Char('*'))
		++end;
	const QString host = path.left(end);
	if (host.isEmpty() || !host.contains(QLatin1Char('.')))
		return refuse("a response rewrite with no host to scope it to");

	const QString search = QLatin1Char('/') + parts.at(1) + QLatin1Char('/') +
	                       flags;
	const QString url = url_regex(host, path.mid(end));
	for (const QString &re : { parts.at(1), url.mid(1, url.size() - 2) }) {
		const QString bad = site_rules::why_pattern_backtracks(re);
		if (!bad.isEmpty()) {
			if (why)
				*why = QString("its pattern %1").arg(bad);
			return rewrite::refused;
		}
	}
	if (out) {
		out->scope = host;
		out->name  = QStringLiteral("trusted-replace-fetch-response");
		out->args  = QStringList{ search, parts.at(2), url };
	}
	return rewrite::carried;
}

}  // namespace

namespace filter_subscription {

line_kind classify(const QString &line, filter_rule *out, QString *why,
                    scriptlet_call *call) {
	const auto reason = [why](const char *text) {
		if (why)
			*why = QString::fromLatin1(text);
	};
	const QString t = line.trimmed();
	if (t.isEmpty() || t.startsWith('!') || t.startsWith('['))
		return line_kind::comment;

	// **uBO's separators are different features, not spellings of one**, so
	// each is named rather than lumped into "not a rule". A person looking at
	// a subscription that contributed a tenth of its lines is owed which tenth.
	//
	//   #?#  procedural cosmetic -- `:has()`, `:upward()`, which are not CSS
	//   #@#  a cosmetic exception, and there is nothing here to except from
	//   #$#  a style or scriptlet injection
	//   #%#  JS injection
	for (const char *marker : { "#?#", "#@#", "#$#", "#%#" }) {
		if (!t.contains(QLatin1String(marker)))
			continue;
		reason("a procedural, exception or injecting cosmetic rule, which this "
		        "build does not implement");
		return line_kind::unsupported;
	}

	const int hash = t.indexOf("##");
	if (hash >= 0) {
		const QString selector = t.mid(hash + 2).trimmed();
		// **A scriptlet, which is the only kind that can reach an ad served
		// from the content's own host.** The catalog is closed: a vetted name
		// becomes a call, and anything else is unsupported with the reason
		// `scriptlets::parse_call` gave -- so a list can ask for a patch from
		// a fixed set and cannot supply a new one.
		if (selector.startsWith(QLatin1String("+js("))) {
			const int close = selector.lastIndexOf(QLatin1Char(')'));
			const QString inside = close > 4 ? selector.mid(4, close - 4)
			                                  : QString();
			scriptlet_call parsed;
			QString said;
			if (!scriptlets::parse_call(inside, &parsed, &said)) {
				if (why)
					*why = said;
				return line_kind::unsupported;
			}
			// The scope is the site it applies on, as it is for a cosmetic
			// rule -- and an unscoped scriptlet is refused for the same
			// reason: a patch to every page's globals is not something a
			// subscribed list gets to ask for.
			parsed.scope = t.left(hash);
			if (parsed.scope.isEmpty()) {
				reason("an unscoped scriptlet, which would patch every page");
				return line_kind::unsupported;
			}
			if (call)
				*call = parsed;
			return line_kind::scriptlet;
		}
		// `##^` is an HTML filter, which removes nodes as the parser sees
		// them. Different feature, and not implemented.
		if (selector.startsWith(QLatin1Char('^'))) {
			reason("an HTML filter, which this build does not run");
			return line_kind::unsupported;
		}
		// Unscoped, which `cosmetic_filters::selectors_for` declines to apply
		// anywhere -- "a rule with no scope is not applied anywhere", because
		// accepting one unbounded is what the review exists to stop. Storing it
		// would be a rule in the list and nothing on the page.
		if (hash == 0) {
			reason("a generic cosmetic rule, which is applied on no site");
			return line_kind::unsupported;
		}
		const QString unsafe_why = filter_list::why_selector_unsafe(selector);
		if (!unsafe_why.isEmpty()) {
			if (why)
				*why = unsafe_why;
			return line_kind::unsafe;
		}
		if (out && !filter_list::parse_rule(t, out)) {
			reason("not parseable as a rule");
			return line_kind::unsupported;
		}
		return line_kind::cosmetic;
	}

	// **An exception rule has nothing to except.** `@@||host^` tells a blocker
	// not to block something, and this engine has no unblock path: `blocks()`
	// answers yes or no from the index and never consults a second set. Stored
	// as written, `@@||ads.example^` becomes a substring pattern that matches
	// no URL ever -- a rule in the list, in the count, doing nothing. The
	// honest answer is to say the list has them and that they are not read.
	if (t.startsWith(QLatin1String("@@"))) {
		reason("an exception rule, and this engine has no unblock path");
		return line_kind::unsupported;
	}
	// A regular-expression rule, `/pattern/`. `filter_list::matches` would take
	// it as a literal substring including the slashes.
	if (t.size() > 2 && t.startsWith(QLatin1Char('/')) &&
	    t.endsWith(QLatin1Char('/'))) {
		reason("a regular-expression rule, which this engine does not compile");
		return line_kind::unsupported;
	}
	// **Options make a rule NARROWER, and ignoring them makes it wider.**
	// `||cdn.example^$script` asks for scripts from that host; enforced with
	// the options dropped it takes the stylesheet and the images too, and
	// `request_filter` already carries a font exemption written for exactly
	// this -- "this engine does not read a rule's resource-type option, so a
	// rule written for a tracker or a script applies to any font URL it
	// matches".
	//
	// On a list the user accepted one rule at a time that was a tolerable
	// trade. On a subscription of tens of thousands it is a page-breaking one,
	// and in the direction that is hardest to diagnose: the page half-loads
	// and the filter that did it was never written to apply there. So a rule
	// carrying options is not enforced, and is counted so the gap is visible
	// rather than inferred. Reading the options is the single change that
	// would unlock most of a real list, and it is its own piece of work.
	//
	// The test is `$` anywhere in the pattern, which is an approximation: a
	// literal `$` in a URL pattern would be misread as an option separator.
	// It is the right way round -- such a rule is skipped rather than
	// enforced too broadly -- and the alternative is a parser for the option
	// grammar, which is the work this defers.
	{
		scriptlet_call rewritten;
		QString said;
		switch (response_rewrite(t, &rewritten, &said)) {
			case rewrite::carried:
				if (call)
					*call = rewritten;
				return line_kind::scriptlet;
			case rewrite::refused:
				if (why)
					*why = said;
				return line_kind::unsupported;
			case rewrite::none:
				break;
		}
	}
	if (t.contains(QLatin1Char('$'))) {
		reason("carries options, which this engine does not read");
		return line_kind::unsupported;
	}
	if (out && !filter_list::parse_rule(t, out)) {
		reason("not parseable as a rule");
		return line_kind::unsupported;
	}
	return line_kind::network;
}

subscription_read read(const QString &text, int previous_rules,
                        bool trusted) {
	subscription_read rep;

	// **The failure this gate is written against is a 200 with the wrong body
	// in it**, not a malformed rule. A captive portal's login page, a CDN
	// error page, a repository that moved and now answers with HTML: each
	// parses as nothing, and promoting it would replace a working list with an
	// empty one. Ad blocking would then be off, silently, looking exactly like
	// an upstream that had got quieter.
	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty()) {
		rep.refusal = QStringLiteral("the body is empty.");
		return rep;
	}
	if (trimmed.startsWith(QLatin1Char('<'))) {
		rep.refusal = QStringLiteral(
		  "the body begins with '<', so this is a web page rather than a "
		  "filter list -- a login page or an error page answered with 200.");
		return rep;
	}

	const QStringList lines = text.split(QLatin1Char('\n'));
	for (const QString &line : lines) {
		filter_rule r;
		scriptlet_call call;
		switch (classify(line, &r, nullptr, &call)) {
			case line_kind::comment:
				continue;   // not a candidate line; headers are not a gap
			case line_kind::network:
			case line_kind::cosmetic:
				++rep.lines;
				++rep.accepted;
				rep.rules.push_back(r);
				break;
			case line_kind::scriptlet:
				++rep.lines;
				// **The trust gate, and the first of the two places it is
				// applied.** `scriptlets::source_for` refuses the same call
				// again where the script is written, so one built any other
				// way cannot carry the power either -- but dropping it here
				// is what makes the count sayable, and a call that never
				// enters the set cannot be injected by a later caller that
				// forgot to ask.
				if (scriptlets::requires_trust(call.name) && !trusted) {
					++rep.needs_trust;
					break;
				}
				// Carried on the call rather than looked up later: by the
				// time a script is written, which list a call came from is
				// out of reach.
				call.trusted = trusted;
				++rep.scriptlets;
				rep.calls.push_back(call);
				// **A network `replace=` rule is carried as two calls**, the
				// fetch rewrite `classify` returned and its XHR twin, because
				// uBlock's `xhr` type covers both transports. Told apart by
				// the line: a scriptlet written as one always has `##`.
				if (!line.contains(QLatin1String("##"))) {
					scriptlet_call twin = call;
					twin.name = QStringLiteral("trusted-replace-xhr-response");
					++rep.scriptlets;
					rep.calls.push_back(twin);
				}
				break;
			case line_kind::unsupported:
				++rep.lines;
				++rep.unsupported;
				break;
			case line_kind::unsafe:
				++rep.lines;
				++rep.unsafe;
				break;
		}
	}

	// **A list of nothing but scriptlets is a usable list.** An annoyance
	// list can be exactly that, and refusing it for having no network rule
	// would refuse the half of the ecosystem this build has just learned to
	// read.
	// A list of nothing but trusted scriptlets, untrusted, has nothing in it
	// this build will run -- and `needs_trust` is deliberately not counted
	// towards usability here, because saying "no rule this build can enforce"
	// while the summary names the dropped ones is the honest pair.
	if (rep.accepted == 0 && rep.scriptlets == 0) {
		rep.refusal = QString("no rule this build can enforce, out of %1 "
		                       "candidate line(s).").arg(rep.lines);
		return rep;
	}

	// **The shrink guard, and it is a judgement rather than a measurement.** A
	// body that is truncated mid-download, or served from a half-migrated
	// mirror, parses cleanly and is simply short -- so rule count is the only
	// signal that the fetch was not the list. A quarter is the threshold
	// because a real list does not lose three quarters of itself between two
	// fetches, and a bad fetch usually loses nearly all of it; nothing finer
	// is defensible without data this does not have. It refuses rather than
	// warns, because the cached copy that stays is a working one.
	if (previous_rules > 0 && rep.accepted * 4 < previous_rules) {
		rep.refusal = QString("%1 rule(s) where the copy in hand has %2. A "
		                       "list does not usually lose three quarters of "
		                       "itself, so the fetch is the likelier fault; "
		                       "the copy in hand is kept.")
		                  .arg(rep.accepted).arg(previous_rules);
		return rep;
	}
	return rep;
}

namespace {

// A name `includes_in` will follow: one file, in the list's own directory.
bool includable(const QString &name) {
	static const QRegularExpression ok(
	  QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._-]*$"));
	return ok.match(name).hasMatch() && !name.contains(QLatin1String(".."));
}

// The included name on an `!#include` line outside any `!#if` block, or
// empty. `depth` carries the block nesting from line to line.
QString include_on(const QString &line, int *depth) {
	const QString t = line.trimmed();
	if (t.startsWith(QLatin1String("!#if"))) {
		++*depth;
		return QString();
	}
	if (t.startsWith(QLatin1String("!#endif"))) {
		if (*depth > 0)
			--*depth;
		return QString();
	}
	if (*depth > 0 || !t.startsWith(QLatin1String("!#include ")))
		return QString();
	const QString name = t.mid(10).trimmed();
	return includable(name) ? name : QString();
}

}  // namespace

QStringList includes_in(const QString &body) {
	QStringList out;
	int depth = 0;
	for (const QString &line : body.split(QLatin1Char('\n'))) {
		const QString name = include_on(line, &depth);
		if (!name.isEmpty() && !out.contains(name))
			out << name;
	}
	return out;
}

QString assemble(const QString &body, const QHash<QString, QString> &included) {
	QStringList out;
	int depth = 0;
	for (const QString &line : body.split(QLatin1Char('\n'))) {
		const QString name = include_on(line, &depth);
		if (!name.isEmpty() && included.contains(name))
			out << included.value(name);
		else
			out << line;
	}
	return out.join(QLatin1Char('\n'));
}

QList<subscription> default_subscriptions() {
	struct seed { const char *name; const char *url; };
	// Fetched and read through this parser before being written here, which
	// is the same rule a README's build line follows: a URL shipped without
	// having been tried is a false claim in the place somebody trusts most.
	static const seed seeds[] = {
		{ "EasyList",       "https://easylist.to/easylist/easylist.txt" },
		{ "uBlock filters",
		   "https://ublockorigin.github.io/uAssets/filters/filters.txt" },
		{ "uBlock filters - Badware risks",
		   "https://ublockorigin.github.io/uAssets/filters/badware.txt" },
		{ "uBlock filters - Privacy",
		   "https://ublockorigin.github.io/uAssets/filters/privacy.txt" },
		{ "uBlock filters - Quick fixes",
		   "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt" },
		{ "uBlock filters - Unbreak",
		   "https://ublockorigin.github.io/uAssets/filters/unbreak.txt" },
	};
	QList<subscription> out;
	for (const seed &s : seeds) {
		subscription sub;
		sub.name    = QString::fromLatin1(s.name);
		sub.url     = QUrl(QString::fromLatin1(s.url));
		sub.enabled = true;
		// Trusted because this file names it; see the header for why that is
		// not the same claim as trusting whatever somebody pastes into Add.
		sub.trusted = true;
		sub.note    = QStringLiteral("not fetched yet");
		out.push_back(sub);
	}
	return out;
}

// Whether this project ships a list at `url`, and with what trust.
//
// **Keyed on the url, not the name.** The name is the list's own title and a
// person may rename a subscription without changing what it is; the url is
// what identifies the thing being trusted.
static bool shipped_trust_for(const QUrl &url) {
	for (const subscription &d : default_subscriptions())
		if (d.url == url)
			return d.trusted;
	return false;
}

QList<subscription> load_index(const QString &path) {
	QList<subscription> out;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return out;   // no subscriptions yet is the ordinary first run
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isArray()) {
		// **Refused rather than repaired, and loudly.** A half-read index
		// would silently drop subscriptions, and the next save would write the
		// shortened list back over the file -- the tree loader's own fault, in
		// a smaller file. The cached bodies are still on disk either way.
		qWarning("subscriptions: %s is not a list (%s); leaving it alone",
		          qPrintable(path), qPrintable(err.errorString()));
		return out;
	}
	for (const QJsonValue &v : doc.array()) {
		const QJsonObject o = v.toObject();
		subscription s;
		s.name    = o.value("name").toString();
		s.url     = QUrl(o.value("url").toString());
		s.enabled = o.value("enabled").toBool(true);
		// **False when the key is absent for a list somebody added**, which is
		// the inverse of `enabled` just above and not a slip: defaulting it
		// the other way would turn the power on retroactively for every list
		// anybody had ever subscribed to, as an upgrade side effect.
		//
		// **But absent is not false for a list this project ships**, and the
		// difference decides whether 22 of uBlock's rules run -- five of them
		// youtube's `trusted-replace-xhr-response` and `trusted-rpfr`, which
		// reach the ad payload rather than hiding a box. An index written
		// before this key existed says nothing about trust, and reading its
		// silence as a refusal left the shipped lists crippled for ever:
		// nothing re-seeds an index that already exists, so the state was
		// permanent and silent. Measured against uBlock's own filters.txt --
		// 2441 of 2451 scriptlet rules accepted, 2419 of them without trust.
		//
		// Settled by the copyright holder 2026-10-09: the lists are handed to
		// the person in the same binary as the code that trusts them, so
		// trusting them is the same act as shipping them, and no security is
		// given away that shipping had not already given. An explicit `false`
		// is still obeyed -- that is somebody's decision rather than an older
		// build's silence -- and a url this project does not ship keeps the
		// old answer.
		s.trusted = o.contains("trusted")
		                ? o.value("trusted").toBool(false)
		                : shipped_trust_for(s.url);
		s.file    = o.value("file").toString();
		s.rules   = o.value("rules").toInt();
		s.note    = o.value("note").toString();
		const QString when = o.value("fetched").toString();
		if (!when.isEmpty())
			s.fetched = QDateTime::fromString(when, Qt::ISODate);
		// A subscription with no url cannot be fetched and a subscription with
		// no file has nothing cached; either is a line somebody edited by
		// hand, and dropping it silently is how an index loses an entry
		// nobody meant to remove. Kept, and the fetcher reports it.
		if (!s.name.isEmpty() || !s.url.isEmpty())
			out.push_back(s);
	}
	return out;
}

bool save_index(const QString &path, const QList<subscription> &subs) {
	QJsonArray arr;
	for (const subscription &s : subs) {
		QJsonObject o;
		o.insert("name", s.name);
		o.insert("url", s.url.toString());
		o.insert("enabled", s.enabled);
		o.insert("trusted", s.trusted);
		o.insert("file", s.file);
		o.insert("rules", s.rules);
		o.insert("note", s.note);
		if (s.fetched.isValid())
			o.insert("fetched", s.fetched.toString(Qt::ISODate));
		arr.append(o);
	}
	// Atomic and able to fail, for the reason `filter_list::save` is: this is
	// the record of what the person subscribed to, and a half-written index is
	// a list that loads with some of them missing.
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
	return f.commit();
}

QString mint_cache_name(const QString &dir, const QString &name) {
	// A readable stem, so somebody looking in the directory can tell which
	// file is which, and a digit on the end when that stem is taken. The name
	// is not trusted as a path: everything but letters, digits, dash and
	// underscore goes, which also takes `/` and `..` with it.
	QString stem;
	for (const QChar c : name) {
		if (c.isLetterOrNumber())
			stem += c.toLower();
		else if (c == QLatin1Char('-') || c == QLatin1Char('_'))
			stem += c;
		else if (!stem.isEmpty() && !stem.endsWith(QLatin1Char('-')))
			stem += QLatin1Char('-');
	}
	while (stem.endsWith(QLatin1Char('-')))
		stem.chop(1);
	if (stem.isEmpty())
		stem = QStringLiteral("list");
	stem = stem.left(40);
	QString candidate = stem + QStringLiteral(".txt");
	for (int n = 2; QFileInfo::exists(QDir(dir).filePath(candidate)); ++n)
		candidate = QString("%1-%2.txt").arg(stem).arg(n);
	return candidate;
}

}  // namespace filter_subscription
