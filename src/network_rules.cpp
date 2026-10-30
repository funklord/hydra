#include "network_rules.h"

#include "filter_list.h"
#include "policy_engine.h"

#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace {

struct entry {
	const char *name;
	const char *aliases[3];
	const char *mime;
	const char *body;
	int         size;    // for the binary ones; -1 for text
};

// **Every resource, and nothing else.** uBlock's names and aliases, read from
// its `redirect-resources.js`; the bodies are this project's. The images are
// generated transparent ones; the text ones are empty or as near empty as
// their type allows.
const entry k_entries[] = {
	{ "noop.js", { "noopjs", nullptr, nullptr }, "application/javascript",
	  "(function() {\n    'use strict';\n})();\n", -1 },
	{ "noop.txt", { "nooptext", nullptr, nullptr }, "text/plain", "\n", -1 },
	{ "noop.html", { "noopframe", nullptr, nullptr }, "text/html",
	  "<!DOCTYPE html>\n<html><head><title></title></head><body></body></html>\n", -1 },
	{ "noop.css", { nullptr, nullptr, nullptr }, "text/css", "/* */\n", -1 },
	{ "noop.json", { "noopjson", nullptr, nullptr }, "application/json", "{}\n", -1 },
	{ "empty", { nullptr, nullptr, nullptr }, "text/plain", "", -1 },
	// **The one that has to say something**: what the real file does, and only
	// that. YouTube loads it and reads the flag to decide whether ads can run.
	{ "doubleclick_instream_ad_status.js",
	  { "doubleclick.net/instream/ad_status.js", nullptr, nullptr },
	  "application/javascript", "window.google_ad_status = 1;\n", -1 },
	{ "1x1.gif", { "1x1-transparent.gif", nullptr, nullptr }, "image/gif",
	  "\x47\x49\x46\x38\x39\x61\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\x00\x00"
	  "\x00\x21\xf9\x04\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00\x01\x00\x01\x00"
	  "\x00\x02\x02\x44\x01\x00\x3b", 43 },
	{ "2x2.png", { "2x2-transparent.png", nullptr, nullptr }, "image/png",
	  "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00"
	  "\x00\x02\x00\x00\x00\x02\x08\x06\x00\x00\x00\x72\xb6\x0d\x24\x00\x00\x00"
	  "\x0b\x49\x44\x41\x54\x78\xda\x63\x60\x40\x07\x00\x00\x12\x00\x01\xe4\x55"
	  "\x8d\xe7\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82", 68 },
	{ "32x32.png", { "32x32-transparent.png", nullptr, nullptr }, "image/png",
	  "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00"
	  "\x00\x20\x00\x00\x00\x20\x08\x06\x00\x00\x00\x73\x7a\x7a\xf4\x00\x00\x00"
	  "\x1a\x49\x44\x41\x54\x78\xda\xed\xc1\x01\x01\x00\x00\x00\x82\x20\xff\xaf"
	  "\x6e\x48\x40\x01\x00\x00\x00\xef\x06\x10\x20\x00\x01\xc9\xb5\xc3\xb1\x00"
	  "\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82", 83 },
};

const entry *lookup(const QString &name) {
	for (const entry &e : k_entries) {
		if (name == QLatin1String(e.name))
			return &e;
		for (const char *a : e.aliases)
			if (a && name == QLatin1String(a))
				return &e;
	}
	return nullptr;
}

// uBlock's type option names, and what each covers.
unsigned type_bit(const QString &word) {
	using namespace request_type;
	if (word == QLatin1String("script")) return script;
	if (word == QLatin1String("image")) return image;
	if (word == QLatin1String("stylesheet") || word == QLatin1String("css")) return stylesheet;
	if (word == QLatin1String("xhr") || word == QLatin1String("xmlhttprequest")) return xhr;
	if (word == QLatin1String("subdocument") || word == QLatin1String("frame")) return subdocument;
	if (word == QLatin1String("media")) return media;
	if (word == QLatin1String("font")) return font;
	if (word == QLatin1String("ping") || word == QLatin1String("beacon")) return ping;
	if (word == QLatin1String("other")) return other;
	if (word == QLatin1String("popup")) return popup;
	return 0;
}

// **Text a regular expression cannot match without**, lowercased, or empty:
// the longest run of plain characters at the top level -- outside every
// class and group, with no alternation anywhere at that level, and less any
// character a quantifier makes optional. It is what lets the lists' 534
// regexes cost a substring test on most requests rather than a match, and
// what files them in the trigram index rather than with the rules tried on
// every request. Measured 2026-10-30 over 60,000 requests of mixed type and
// party: 13 us a decision without the regexes, 57 with them and no such
// text, 27 with it. Conservative by construction -- a run it cannot vouch for
// ends -- and a run that every address has (`http`, `://`, `www.`) is
// passed over, being no filter at all.
QString required_text(const QString &re) {
	QStringList runs;
	QString run;
	auto end_run = [&] {
		if (!run.isEmpty())
			runs << run;
		run.clear();
	};
	int depth = 0;
	for (int i = 0; i < re.size(); ++i) {
		const QChar c = re.at(i);
		if (c == QLatin1Char('\\') && i + 1 < re.size()) {
			const QChar n = re.at(++i);
			if (n.isLetterOrNumber()) {          // \d, \w, \b ...: a class, not a letter
				end_run();
				continue;
			}
			if (depth == 0)
				run += n;
			continue;
		}
		if (c == QLatin1Char('[')) {
			end_run();
			for (++i; i < re.size() && re.at(i) != QLatin1Char(']'); ++i)
				if (re.at(i) == QLatin1Char('\\'))
					++i;
			continue;
		}
		if (c == QLatin1Char('(')) {
			end_run();
			++depth;
			continue;
		}
		if (c == QLatin1Char(')')) {
			--depth;
			continue;
		}
		if (c == QLatin1Char('|')) {
			if (depth == 0)
				return QString();             // either side may be the match
			continue;
		}
		if (depth > 0)
			continue;
		if (c == QLatin1Char('?') || c == QLatin1Char('*') || c == QLatin1Char('{')) {
			if (!run.isEmpty())
				run.chop(1);                  // the char before may be absent or many
			end_run();
			if (c == QLatin1Char('{'))
				while (i < re.size() && re.at(i) != QLatin1Char('}'))
					++i;
			continue;
		}
		if (c == QLatin1Char('+')) {
			end_run();                        // at least one: what came before stands
			continue;
		}
		if (c == QLatin1Char('.') || c == QLatin1Char('^') || c == QLatin1Char('$')) {
			end_run();
			continue;
		}
		run += c;
	}
	end_run();
	static const QString common = QStringLiteral("https://www.");
	QString best;
	for (const QString &r : runs) {
		const QString l = r.toLower();
		if (l.size() > best.size() && !common.contains(l))
			best = l;
	}
	return best.size() >= 3 ? best : QString();
}

// Three characters as one key, for the trigram index.
quint64 gram(const QChar *p) {
	return (quint64(p[0].unicode()) << 32) | (quint64(p[1].unicode()) << 16) |
	       quint64(p[2].unicode());
}

bool site_in(const QString &host, const QString &site) {
	return filter_list::site_matches(site, host);
}

}  // namespace

namespace redirect_resources {

bool find(const QString &name, resource *out) {
	const entry *e = lookup(name);
	if (!e)
		return false;
	if (out) {
		out->mime = QByteArray(e->mime);
		out->body = e->size < 0 ? QByteArray(e->body) : QByteArray(e->body, e->size);
	}
	return true;
}

QStringList names() {
	QStringList out;
	for (const entry &e : k_entries)
		out << QString::fromLatin1(e.name);
	return out;
}

}  // namespace redirect_resources

parsed parse_network_rule(const QString &line, network_rule *out, QString *why,
                          QString *target) {
	const QString t = line.trimmed();
	if (t.isEmpty() || t.startsWith(QLatin1Char('!')) || t.startsWith(QLatin1Char('[')) ||
	    t.contains(QLatin1String("##")) || t.contains(QLatin1String("#@#")) ||
	    t.contains(QLatin1String("#?#")) || t.contains(QLatin1String("#$#")))
		return parsed::none;
	const bool allow = t.startsWith(QLatin1String("@@"));
	const QString body = allow ? t.mid(2) : t;
	// **A regular expression is `/.../`, and may itself end in `$`**, so its
	// options start after the closing slash rather than at the last `$`.
	// So: the pattern before the last `$` is `/.../`, or the whole line is and
	// what follows its last `$` is no option list (no `=`, no `,`) but the
	// regex's own tail. A path rule whose options end in a slash --
	// `/theme/app.js|$script,replace=/a/b/` -- is neither.
	int dollar = body.lastIndexOf(QLatin1Char('$'));
	const QString before = dollar < 0 ? body : body.left(dollar);
	bool regex = before.size() > 2 && before.startsWith(QLatin1Char('/')) &&
	             before.endsWith(QLatin1Char('/'));
	if (!regex && body.size() > 2 && body.startsWith(QLatin1Char('/')) &&
	    body.endsWith(QLatin1Char('/'))) {
		const QString tail = dollar < 0 ? QString() : body.mid(dollar + 1);
		if (!tail.contains(QLatin1Char('=')) && !tail.contains(QLatin1Char(','))) {
			regex = true;
			dollar = -1;
		}
	}
	// A rule with no options is the block list's -- unless it is an
	// exception, which nothing else carries, or a regular expression, which
	// the block list cannot read.
	if (dollar < 0 && !allow && !regex)
		return parsed::none;
	const QString options = dollar < 0 ? QString() : body.mid(dollar + 1);
	// `replace=` has its own reader (`filter_subscription`), which turns it
	// into a page patch rather than a rule.
	if (options.contains(QLatin1String("replace=")))
		return parsed::none;
	const auto refuse = [why](const QString &reason) {
		if (why)
			*why = reason;
		return parsed::refused;
	};

	network_rule r;
	r.text = t;
	r.act = allow ? network_rule::action::allow : network_rule::action::block;
	r.pattern = dollar < 0 ? body : body.left(dollar);
	r.regex = regex;
	if (!regex && r.pattern.startsWith(QLatin1String("||"))) {
		QString h = r.pattern.mid(2);
		const int end = h.indexOf(QRegularExpression(QStringLiteral("[/^*?:|]")));
		if (end >= 0)
			h = h.left(end);
		// A host ending in a dot has a wildcard after it, and nothing to file by.
		if (!h.endsWith(QLatin1Char('.')))
			r.host = h.toLower();
	}
	bool badfilter = false, match_case = false, removes = false, skips = false;
	for (const QString &raw : options.split(QLatin1Char(','))) {
		const QString o = raw.trimmed();
		if (o.isEmpty())
			continue;
		if (o.startsWith(QLatin1String("urlskip="))) {
			r.steps = o.mid(8).trimmed();
			if (r.steps.isEmpty())
				return refuse(QStringLiteral("an urlskip with no steps"));
			skips = true;
			continue;
		}
		if (o == QLatin1String("removeparam") || o == QLatin1String("queryprune") ||
		    o.startsWith(QLatin1String("removeparam=")) ||
		    o.startsWith(QLatin1String("queryprune="))) {
			removes = true;
			r.param = o.section(QLatin1Char('='), 1);
			continue;
		}
		if (o.startsWith(QLatin1String("redirect=")) ||
		    o.startsWith(QLatin1String("redirect-rule="))) {
			if (allow)
				return refuse(QStringLiteral("a redirect exception, and this engine "
				                             "has no exception for a redirect"));
			r.act = network_rule::action::redirect;
			r.rule_only = o.startsWith(QLatin1String("redirect-rule="));
			QString name = o.mid(o.indexOf(QLatin1Char('=')) + 1);
			const int colon = name.lastIndexOf(QLatin1Char(':'));
			if (colon > 0) {
				bool ok = false;
				const int p = name.mid(colon + 1).toInt(&ok);
				if (ok) {
					r.priority = p;
					name = name.left(colon);
				}
			}
			if (name == QLatin1String("none"))
				return refuse(QStringLiteral("a redirect exception, and this engine "
				                             "has no exception for a redirect"));
			if (!redirect_resources::find(name))
				return refuse(QString("names a redirect resource this build does "
				                      "not carry (%1)").arg(name));
			r.resource = QString::fromLatin1(lookup(name)->name);
			continue;
		}
		if (o.startsWith(QLatin1String("domain=")) || o.startsWith(QLatin1String("from="))) {
			for (const QString &d : o.mid(o.indexOf(QLatin1Char('=')) + 1)
			                            .split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
				if (d.startsWith(QLatin1Char('~')))
					r.exclude << d.mid(1).toLower();
				else
					r.include << d.toLower();
			}
			continue;
		}
		// `to=`: the request's own host, as `domain=` is the page's.
		if (o.startsWith(QLatin1String("to="))) {
			for (const QString &d : o.mid(3).split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
				if (d.startsWith(QLatin1Char('~')))
					r.to_exclude << d.mid(1).toLower();
				else
					r.to << d.toLower();
			}
			continue;
		}
		if (o.startsWith(QLatin1String("denyallow="))) {
			for (const QString &d : o.mid(10).split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
				if (d.startsWith(QLatin1Char('~')))
					return refuse(QStringLiteral("a negated denyallow host, which uBlock "
					                             "does not define either"));
				r.denyallow << d.toLower();
			}
			continue;
		}
		if (o == QLatin1String("1p") || o == QLatin1String("first-party") ||
		    o == QLatin1String("~third-party") || o == QLatin1String("~3p")) {
			r.party = 1;
			continue;
		}
		if (o == QLatin1String("3p") || o == QLatin1String("third-party") ||
		    o == QLatin1String("~first-party") || o == QLatin1String("~1p")) {
			r.party = 3;
			continue;
		}
		if (o == QLatin1String("important")) {
			r.important = true;
			continue;
		}
		if (o == QLatin1String("badfilter")) {
			badfilter = true;
			continue;
		}
		if (o == QLatin1String("match-case")) {
			match_case = true;
			continue;
		}
		// **A page, not a request.** As an exception it exempts the page
		// whole, which is what the Unbreak list uses it for. As a block it
		// would refuse the page itself, which this engine does not do.
		if (o == QLatin1String("document") || o == QLatin1String("doc")) {
			// A removal or a skip for the page is what either does anyway.
			if (options.contains(QLatin1String("removeparam")) ||
			    options.contains(QLatin1String("queryprune")) ||
			    options.contains(QLatin1String("urlskip=")))
				continue;
			r.document = true;
			continue;
		}
		if (o == QLatin1String("all")) {
			r.all = true;
			r.document = true;
			r.types |= request_type::script | request_type::image | request_type::stylesheet |
			           request_type::xhr | request_type::subdocument | request_type::media |
			           request_type::font | request_type::ping | request_type::other |
			           request_type::popup;
			continue;
		}
		if (o.startsWith(QLatin1Char('~'))) {
			if (const unsigned bit = type_bit(o.mid(1))) {
				r.not_types |= bit;
				continue;
			}
		}
		if (const unsigned bit = type_bit(o)) {
			r.types |= bit;
			continue;
		}
		return refuse(QString("a rule with an option this reader does not know "
		                      "(%1)").arg(o.section(QLatin1Char('='), 0, 0)));
	}
	if (badfilter) {
		// The rule it cancels is the same line without this option.
		if (target) {
			QStringList kept;
			for (const QString &o : options.split(QLatin1Char(',')))
				if (o.trimmed() != QLatin1String("badfilter"))
					kept << o.trimmed();
			*target = (allow ? QStringLiteral("@@") : QString()) + r.pattern +
			          (kept.isEmpty() ? QString() : QLatin1Char('$') + kept.join(QLatin1Char(',')));
		}
		return parsed::badfilter;
	}
	if (r.document && !allow && !removes && !skips &&
	    r.act == network_rule::action::redirect)
		return refuse(QStringLiteral("a page block with a redirect"));
	if (skips) {
		if (removes || r.act == network_rule::action::redirect)
			return refuse(QStringLiteral("urlskip with another action"));
		r.exception = allow;
		r.act = network_rule::action::urlskip;
		for (const QString &step : r.steps.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
			const bool known = step.startsWith(QLatin1Char('?')) ||
			  step.startsWith(QLatin1Char('&')) ||
			  (step.startsWith(QLatin1Char('/')) && step.size() > 2 && step.endsWith(QLatin1Char('/'))) ||
			  step == QLatin1String("-base64") || step == QLatin1String("-safebase64") ||
			  step == QLatin1String("-uri") || step == QLatin1String("-uricomponent") ||
			  step == QLatin1String("+https") || step == QLatin1String("+http");
			if (!known)
				return refuse(QString("an urlskip step this reader does not know (%1)").arg(step));
			if (step.startsWith(QLatin1Char('/')) &&
			    !QRegularExpression(step.mid(1, step.size() - 2)).isValid())
				return refuse(QStringLiteral("an urlskip pattern that does not compile"));
		}
	}
	if (removes) {
		if (r.act == network_rule::action::redirect)
			return refuse(QStringLiteral("removeparam with a redirect"));
		r.exception = allow;
		r.act = network_rule::action::removeparam;
		if (r.param.startsWith(QLatin1Char('/'))) {
			const QString body = r.param.mid(1, r.param.lastIndexOf(QLatin1Char('/')) - 1);
			if (r.param.lastIndexOf(QLatin1Char('/')) < 1 ||
			    !QRegularExpression(body).isValid())
				return refuse(QStringLiteral("a removeparam pattern that does not compile"));
		}
	}
	if (regex) {
		r.match = QRegularExpression(r.pattern.mid(1, r.pattern.size() - 2),
		                             QRegularExpression::CaseInsensitiveOption);
		r.literal = required_text(r.pattern.mid(1, r.pattern.size() - 2));
	} else {
		r.match = filter_list::pattern_regex(r.pattern);
		r.literal = filter_list::literal_of(r.pattern);
	}
	if (match_case)
		r.match.setPatternOptions(QRegularExpression::NoPatternOption);
	if (!r.match.isValid())
		return refuse(QStringLiteral("a rule whose pattern does not compile"));
	// A removal with no pattern is the ordinary kind -- `$removeparam=fbclid`
	// everywhere -- and takes nothing but the parameter it names.
	if (r.pattern.isEmpty() && !r.document && r.include.isEmpty() && r.to.isEmpty() && !removes)
		return refuse(QStringLiteral("a rule with no pattern and no site, which would "
		                             "match every request everywhere"));
	if (out)
		*out = r;
	return parsed::option;
}

void apply_badfilters(const QSet<QString> &cancelled, QList<network_rule> *rules,
                      QStringList *plain) {
	if (cancelled.isEmpty())
		return;
	if (rules)
		for (int i = int(rules->size()) - 1; i >= 0; --i)
			if (cancelled.contains(rules->at(i).text))
				rules->removeAt(i);
	if (plain)
		for (int i = int(plain->size()) - 1; i >= 0; --i)
			if (cancelled.contains(plain->at(i)))
				plain->removeAt(i);
}

void network_rule_set::replace(const QList<network_rule> &rules) {
	QWriteLocker guard(&m_lock);
	m_rules = rules;
	m_by_host.clear();
	m_by_site.clear();
	m_params.clear();
	m_by_token.clear();
	m_by_gram.clear();
	m_rest.clear();
	m_documents.clear();
	m_documents_by_host.clear();
	m_pages.clear();
	m_pages_by_host.clear();
	for (int i = 0; i < m_rules.size(); ++i) {
		const network_rule &r = m_rules[i];
		if (r.act == network_rule::action::removeparam ||
		    r.act == network_rule::action::urlskip) {
			m_params << i;
			continue;
		}
		// A page block apart from the page exceptions: `site_allowed` is
		// asked on every request, and has no business reading thousands.
		if (r.document && r.act != network_rule::action::allow) {
			if (!r.host.isEmpty())
				m_pages_by_host[r.host] << i;
			else if (!r.to.isEmpty())
				for (const QString &d : r.to)
					m_pages_by_host[d] << i;
			else
				m_pages << i;
			// `$all` is a block for every request as well as the page.
			if (!r.all)
				continue;
		} else if (r.document) {
			if (!r.host.isEmpty())
				m_documents_by_host[r.host] << i;
			else
				m_documents << i;
			continue;
		}
		if (!r.host.isEmpty()) {
			m_by_host[r.host] << i;
			continue;
		}
		// `to=` names the request's host, which is what this index is by.
		if (!r.to.isEmpty()) {
			for (const QString &d : r.to)
				m_by_host[d] << i;
			continue;
		}
		if (!r.include.isEmpty()) {
			for (const QString &d : r.include)
				m_by_site[d] << i;
			continue;
		}
		// A regular expression with nothing to file by is tried on every
		// request; its source is not a pattern to take a token from.
		if (r.regex) {
			if (r.literal.size() >= 3)
				m_by_gram[gram(r.literal.constData() + r.literal.size() - 3)] << i;
			else
				m_rest << i;
			continue;
		}
		const QString tok = filter_list::token_of(r.pattern);
		if (!tok.isEmpty())
			m_by_token[tok] << i;
		else if (r.literal.size() >= 3)
			// The last three: the end of a pattern is where it says what it
			// is after (`ad/`, `.js`), where its start is often a TLD.
			m_by_gram[gram(r.literal.constData() + r.literal.size() - 3)] << i;
		else
			m_rest << i;
	}
}

int network_rule_set::size() const {
	QReadLocker guard(&m_lock);
	return int(m_rules.size());
}

int network_rule_set::count(network_rule::action a) const {
	QReadLocker guard(&m_lock);
	int n = 0;
	for (const network_rule &r : m_rules)
		n += r.act == a ? 1 : 0;
	return n;
}

// Every rule that could match this request: by the request's host and its
// parents, by the page's site and its parents, by the URL's tokens, and the
// few with nothing to file by. Called with the read lock held.
//
// **A rule may be visited twice** -- filed under two of a rule's sites, or a
// token the URL repeats -- and that is allowed rather than prevented: every
// question asked of a candidate gives the same answer the second time, and
// the set that prevented it was most of what a decision cost (measured with
// perf, 2026-10-29).
template <typename F>
void network_rule_set::each_candidate(const request &r, F f) const {
	const auto visit = [&](const QList<int> &ids) {
		for (int i : ids)
			if (f(m_rules[i]))
				return true;
		return false;
	};
	const auto by_suffix = [&](const QHash<QString, QList<int>> &index, const QString &host) {
		for (QString h = host; !h.isEmpty();) {
			const auto it = index.constFind(h);
			if (it != index.constEnd() && visit(it.value()))
				return true;
			const int dot = h.indexOf(QLatin1Char('.'));
			if (dot < 0)
				break;
			h = h.mid(dot + 1);
		}
		return false;
	};
	if (by_suffix(m_by_host, r.host.toLower()))
		return;
	if (by_suffix(m_by_site, r.site.toLower()))
		return;
	// `domain=example.*` is filed under the entity, which no suffix of the
	// site spells.
	for (const QString &k : filter_list::entity_keys(r.site.toLower())) {
		const auto it = m_by_site.constFind(k);
		if (it != m_by_site.constEnd() && visit(it.value()))
			return;
	}
	for (const QString &tok : filter_list::tokens_in(r.url)) {
		const auto it = m_by_token.constFind(tok);
		if (it != m_by_token.constEnd() && visit(it.value()))
			return;
	}
	if (!m_by_gram.isEmpty()) {
		const QString lower = r.lower.isEmpty() ? r.url.toLower() : r.lower;
		const QChar *p = lower.constData();
		for (int i = 0; i + 3 <= lower.size(); ++i) {
			const auto it = m_by_gram.constFind(gram(p + i));
			if (it != m_by_gram.constEnd() && visit(it.value()))
				return;
		}
	}
	visit(m_rest);
}

bool network_rule_set::applies(const network_rule &rule, const request &r,
                               bool same_party) const {
	// **Popups apart, both ways** (`request_type::popup`): a popup is
	// decided by `$popup` rules alone, and a `$popup` rule's other types, if
	// it has any, are what it says about everything else.
	if (r.type == request_type::popup) {
		if (!(rule.types & request_type::popup))
			return false;
	} else if (rule.types & request_type::popup) {
		const unsigned rest = rule.types & ~unsigned(request_type::popup);
		if (!rest || (r.type != request_type::unknown && !(rest & r.type)))
			return false;
	} else if (rule.types && r.type != request_type::unknown && !(rule.types & r.type)) {
		return false;
	}
	// A block naming no type spares fonts, as the plain rules do: a blocked
	// webfont shows a page's icons as their ligature text.
	if (!rule.types && rule.act == network_rule::action::block &&
	    r.type == request_type::font)
		return false;
	if (rule.not_types && r.type != request_type::unknown && (rule.not_types & r.type))
		return false;
	if (rule.party == 1 && !same_party)
		return false;
	if (rule.party == 3 && same_party)
		return false;
	const QString site = r.site.toLower();
	if (!rule.include.isEmpty()) {
		bool in = false;
		for (const QString &d : rule.include)
			in = in || site_in(site, d);
		if (!in)
			return false;
	}
	for (const QString &d : rule.exclude)
		if (site_in(site, d))
			return false;
	if (!rule.to.isEmpty() || !rule.to_exclude.isEmpty()) {
		const QString host = r.host.toLower();
		bool in = rule.to.isEmpty();
		for (const QString &d : rule.to)
			in = in || site_in(host, d);
		for (const QString &d : rule.to_exclude)
			in = in && !site_in(host, d);
		if (!in)
			return false;
	}
	if (!rule.denyallow.isEmpty()) {
		const QString host = r.host.toLower();
		for (const QString &d : rule.denyallow)
			if (site_in(host, d))
				return false;
	}
	if (rule.pattern.isEmpty())
		return true;
	// Against the lowercased address, case-sensitively: the same answer as a
	// case-insensitive search, which was most of what a decision cost.
	if (!rule.literal.isEmpty() &&
	    !(r.lower.isEmpty() ? r.url.toLower() : r.lower).contains(rule.literal))
		return false;
	return rule.match.match(r.url).hasMatch();
}

network_rule_set::verdict network_rule_set::evaluate(const request &r) const {
	QReadLocker guard(&m_lock);
	verdict v;
	if (m_rules.isEmpty())
		return v;
	const bool same = !r.site.isEmpty() &&
	  policy_engine::etld_plus_one(r.host.toLower()) ==
	  policy_engine::etld_plus_one(r.site.toLower());
	int best = -1, best_rule = -1;
	each_candidate(r, [&](const network_rule &rule) {
		switch (rule.act) {
			case network_rule::action::redirect: {
				int &slot = rule.rule_only ? best_rule : best;
				if (slot >= rule.priority || !applies(rule, r, same))
					return false;
				slot = rule.priority;
				(rule.rule_only ? v.redirect_rule : v.redirect) = rule.resource;
				return false;
			}
			case network_rule::action::block:
				if (v.important || !applies(rule, r, same))
					return false;
				v.block = true;
				v.important = rule.important;
				return false;
			case network_rule::action::allow:
				if (!v.allow && applies(rule, r, same))
					v.allow = true;
				return false;
			case network_rule::action::removeparam:
			case network_rule::action::urlskip:
				// Kept apart (`m_params`) and asked about navigations only.
				return false;
		}
		return false;
	});
	return v;
}

namespace {
bool same_party_of(const network_rule_set::request &r) {
	return !r.site.isEmpty() &&
	  policy_engine::etld_plus_one(r.host.toLower()) ==
	  policy_engine::etld_plus_one(r.site.toLower());
}
}  // namespace

QString network_rule_set::resource_for(const request &r, bool blocked) const {
	QReadLocker guard(&m_lock);
	const bool same = same_party_of(r);
	const network_rule *best = nullptr;
	each_candidate(r, [&](const network_rule &rule) {
		if (rule.act != network_rule::action::redirect || (rule.rule_only && !blocked))
			return false;
		if (best && best->priority >= rule.priority)
			return false;
		if (applies(rule, r, same))
			best = &rule;
		return false;
	});
	return best ? best->resource : QString();
}

bool network_rule_set::blocks(const request &r, bool *important) const {
	QReadLocker guard(&m_lock);
	const bool same = same_party_of(r);
	bool hit = false, imp = false;
	each_candidate(r, [&](const network_rule &rule) {
		if (rule.act != network_rule::action::block || !applies(rule, r, same))
			return false;
		hit = true;
		imp = imp || rule.important;
		return imp;   // an important one settles it
	});
	if (important)
		*important = imp;
	return hit;
}

bool network_rule_set::allows(const request &r) const {
	QReadLocker guard(&m_lock);
	const bool same = same_party_of(r);
	bool hit = false;
	each_candidate(r, [&](const network_rule &rule) {
		if (rule.act == network_rule::action::allow && applies(rule, r, same))
			hit = true;
		return hit;
	});
	return hit;
}

QString network_rule_set::clean(const request &r) const {
	QReadLocker guard(&m_lock);
	const int q = r.url.indexOf(QLatin1Char('?'));
	if (m_params.isEmpty() || q < 0)
		return QString();
	const bool same = same_party_of(r);
	const auto applies_here = [&](const network_rule &rule) {
		// A rule naming a type other than a page or a frame is not for a
		// navigation; one naming none is.
		const unsigned nav = request_type::subdocument | request_type::other;
		if (rule.types && !(rule.types & nav))
			return false;
		network_rule plain = rule;
		plain.types = 0;
		return applies(plain, r, same);
	};
	QList<const network_rule *> removing;
	QSet<QString> lifted;
	bool all_lifted = false;
	for (int i : m_params) {
		const network_rule &rule = m_rules[i];
		if (rule.act != network_rule::action::removeparam || !applies_here(rule))
			continue;
		if (rule.exception) {
			if (rule.param.isEmpty())
				all_lifted = true;
			lifted.insert(rule.param);
		} else {
			removing << &rule;
		}
	}
	if (all_lifted || removing.isEmpty())
		return QString();

	const int hash = r.url.indexOf(QLatin1Char('#'), q);
	const QString query = r.url.mid(q + 1, hash < 0 ? -1 : hash - q - 1);
	const QString fragment = hash < 0 ? QString() : r.url.mid(hash);
	QStringList kept;
	bool changed = false;
	for (const QString &pair : query.split(QLatin1Char('&'))) {
		if (pair.isEmpty())
			continue;
		const QString name = QUrl::fromPercentEncoding(pair.section(QLatin1Char('='), 0, 0).toUtf8());
		bool drop = false;
		for (const network_rule *rule : removing) {
			if (lifted.contains(rule->param))
				continue;
			const QString &p = rule->param;
			if (p.isEmpty()) {
				drop = true;
			} else if (p.startsWith(QLatin1Char('~'))) {
				drop = name != p.mid(1);
			} else if (p.startsWith(QLatin1Char('/')) && p.lastIndexOf(QLatin1Char('/')) > 0) {
				const int end = p.lastIndexOf(QLatin1Char('/'));
				const QRegularExpression re(
				  p.mid(1, end - 1), p.mid(end + 1).contains(QLatin1Char('i'))
				                       ? QRegularExpression::CaseInsensitiveOption
				                       : QRegularExpression::NoPatternOption);
				drop = re.match(QUrl::fromPercentEncoding(pair.toUtf8())).hasMatch();
			} else {
				drop = name == p;
			}
			if (drop)
				break;
		}
		if (drop)
			changed = true;
		else
			kept << pair;
	}
	if (!changed)
		return QString();
	return r.url.left(q) + (kept.isEmpty() ? QString() : QLatin1Char('?') + kept.join(QLatin1Char('&'))) +
	       fragment;
}

namespace {

// One urlskip's steps, applied to the address. Empty when a step finds
// nothing or the end is not an http or https address.
QString run_steps(const QString &steps, const QString &url) {
	QString v = url;
	for (const QString &step : steps.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
		if (step.startsWith(QLatin1Char('?')) || step.startsWith(QLatin1Char('&'))) {
			const QUrl u(v);
			if (!u.hasQuery())
				return QString();
			const QUrlQuery q(u);
			const auto items = q.queryItems(QUrl::FullyDecoded);
			QString found;
			bool have = false;
			if (step.startsWith(QLatin1Char('?'))) {
				const QString name = step.mid(1);
				for (const auto &it : items)
					if (it.first == name) { found = it.second; have = true; break; }
			} else {
				const int n = step.mid(1).toInt() - 1;
				if (n >= 0 && n < items.size()) { found = items[n].second; have = true; }
			}
			if (!have)
				return QString();
			v = found;
		} else if (step.startsWith(QLatin1Char('/'))) {
			const QRegularExpressionMatch m =
			  QRegularExpression(step.mid(1, step.size() - 2)).match(v);
			if (!m.hasMatch() || m.lastCapturedIndex() < 1)
				return QString();
			v = m.captured(1);
		} else if (step == QLatin1String("-base64") || step == QLatin1String("-safebase64")) {
			const QByteArray d = QByteArray::fromBase64(
			  v.toLatin1(), step == QLatin1String("-safebase64")
			                  ? QByteArray::Base64UrlEncoding : QByteArray::Base64Encoding);
			v = QString::fromUtf8(d);
		} else if (step == QLatin1String("-uri") || step == QLatin1String("-uricomponent")) {
			v = QUrl::fromPercentEncoding(v.toUtf8());
		} else if (step == QLatin1String("+https") || step == QLatin1String("+http")) {
			if (!v.contains(QLatin1String("://")))
				v = step.mid(1) + QStringLiteral("://") + v;
		}
	}
	const QUrl out(v, QUrl::StrictMode);
	if (!out.isValid() || out.host().isEmpty() ||
	    (out.scheme() != QLatin1String("http") && out.scheme() != QLatin1String("https")))
		return QString();
	return out.toString(QUrl::FullyEncoded);
}

}  // namespace

QString network_rule_set::skip(const request &r) const {
	QReadLocker guard(&m_lock);
	if (m_params.isEmpty())
		return QString();
	const bool same = same_party_of(r);
	const unsigned nav = request_type::subdocument | request_type::other;
	bool lifted = false;
	QList<const network_rule *> skips;
	for (int i : m_params) {
		const network_rule &rule = m_rules[i];
		if (rule.act != network_rule::action::urlskip ||
		    (rule.types && !(rule.types & nav)))
			continue;
		network_rule plain = rule;
		plain.types = 0;
		if (!applies(plain, r, same))
			continue;
		if (rule.exception)
			lifted = true;
		else
			skips << &rule;
	}
	if (lifted)
		return QString();
	for (const network_rule *rule : skips) {
		const QString to = run_steps(rule->steps, r.url);
		if (!to.isEmpty() && to != r.url)
			return to;
	}
	return QString();
}

QString network_rule_set::page_blocked(const QString &url, const QString &host) const {
	QReadLocker guard(&m_lock);
	if ((m_pages.isEmpty() && m_pages_by_host.isEmpty()) || host.isEmpty())
		return QString();
	const QString h0 = host.toLower();
	QList<int> candidates = m_pages;
	for (QString h = h0; !h.isEmpty();) {
		const auto it = m_pages_by_host.constFind(h);
		if (it != m_pages_by_host.constEnd())
			candidates += it.value();
		const int dot = h.indexOf(QLatin1Char('.'));
		if (dot < 0)
			break;
		h = h.mid(dot + 1);
	}
	for (int i : candidates) {
		const network_rule &rule = m_rules[i];
		if (!rule.include.isEmpty()) {
			bool in = false;
			for (const QString &d : rule.include)
				in = in || site_in(h0, d);
			if (!in)
				continue;
		}
		bool out = false;
		for (const QString &d : rule.exclude)
			out = out || site_in(h0, d);
		// For a page, the request's host and the page's are one.
		bool to = rule.to.isEmpty();
		for (const QString &d : rule.to)
			to = to || site_in(h0, d);
		for (const QString &d : rule.to_exclude)
			out = out || site_in(h0, d);
		if (out || !to)
			continue;
		if (rule.pattern.isEmpty() || rule.match.match(url).hasMatch())
			return rule.text;
	}
	return QString();
}

bool network_rule_set::site_allowed(const QString &site) const {
	QReadLocker guard(&m_lock);
	if ((m_documents.isEmpty() && m_documents_by_host.isEmpty()) || site.isEmpty())
		return false;
	const QString host = site.toLower();
	const QString url = QStringLiteral("https://") + host + QLatin1Char('/');
	QList<int> candidates = m_documents;
	for (QString h = host; !h.isEmpty();) {
		const auto it = m_documents_by_host.constFind(h);
		if (it != m_documents_by_host.constEnd())
			candidates += it.value();
		const int dot = h.indexOf(QLatin1Char('.'));
		if (dot < 0)
			break;
		h = h.mid(dot + 1);
	}
	for (int i : candidates) {
		const network_rule &rule = m_rules[i];
		if (!rule.include.isEmpty()) {
			bool in = false;
			for (const QString &d : rule.include)
				in = in || site_in(site.toLower(), d);
			if (!in)
				continue;
		}
		if (rule.pattern.isEmpty() || rule.match.match(url).hasMatch())
			return true;
	}
	return false;
}
