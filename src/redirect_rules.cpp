#include "redirect_rules.h"

#include "policy_engine.h"

#include <QRegularExpression>
#include <QUrl>

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
	return 0;
}

// A network pattern as a regex: `||` any scheme and any subdomain boundary,
// a leading or trailing `|` an anchor, `*` anything, `^` a separator
// character or the end of the address. Case-insensitive, as uBlock reads
// patterns unless told otherwise.
QRegularExpression compile_pattern(const QString &pattern) {
	QString p = pattern, re;
	if (p.startsWith(QLatin1String("||"))) {
		re = QStringLiteral("^[a-z][a-z0-9+.-]*://([^/?#]*\\.)?");
		p = p.mid(2);
	} else if (p.startsWith(QLatin1Char('|'))) {
		re = QStringLiteral("^");
		p = p.mid(1);
	}
	bool end = false;
	if (p.endsWith(QLatin1Char('|'))) {
		end = true;
		p.chop(1);
	}
	for (const QChar c : p) {
		if (c == QLatin1Char('*'))
			re += QStringLiteral(".*");
		else if (c == QLatin1Char('^'))
			re += QStringLiteral("(?:[^a-z0-9_.%-]|$)");
		else
			re += QRegularExpression::escape(QString(c));
	}
	if (end)
		re += QLatin1Char('$');
	return QRegularExpression(re, QRegularExpression::CaseInsensitiveOption);
}

bool site_in(const QString &host, const QString &site) {
	return host == site || host.endsWith(QLatin1Char('.') + site);
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

bool parse_redirect_rule(const QString &line, redirect_rule *out, QString *why) {
	const QString t = line.trimmed();
	const int dollar = t.lastIndexOf(QLatin1Char('$'));
	if (dollar <= 0 || t.startsWith(QLatin1String("@@")))
		return false;
	const QString options = t.mid(dollar + 1);
	if (!options.contains(QLatin1String("redirect=")) &&
	    !options.contains(QLatin1String("redirect-rule=")))
		return false;
	const auto refuse = [why](const QString &reason) {
		if (why)
			*why = reason;
		return false;
	};

	redirect_rule r;
	r.pattern = t.left(dollar);
	// A regex rule is `/.../`; one that only starts with a slash is a path.
	if (r.pattern.isEmpty() ||
	    (r.pattern.size() > 2 && r.pattern.startsWith(QLatin1Char('/')) &&
	     r.pattern.endsWith(QLatin1Char('/'))))
		return refuse(QStringLiteral("a redirect rule whose pattern is a regex or "
		                             "empty, which this engine does not match"));
	if (r.pattern.startsWith(QLatin1String("||"))) {
		QString h = r.pattern.mid(2);
		const int end = h.indexOf(QRegularExpression(QStringLiteral("[/^*?:]")));
		if (end >= 0)
			h = h.left(end);
		r.host = h.toLower();
	}
	for (const QString &raw : options.split(QLatin1Char(','))) {
		const QString o = raw.trimmed();
		if (o.isEmpty())
			continue;
		if (o.startsWith(QLatin1String("redirect=")) ||
		    o.startsWith(QLatin1String("redirect-rule="))) {
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
				                             "has no exception path"));
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
		if (o == QLatin1String("important"))
			continue;   // it outranks exceptions, and this engine reads none
		if (const unsigned bit = type_bit(o)) {
			r.types |= bit;
			continue;
		}
		return refuse(QString("a redirect rule with an option this reader does "
		                      "not know (%1)").arg(o));
	}
	if (r.resource.isEmpty())
		return refuse(QStringLiteral("a redirect rule naming no resource"));
	r.match = compile_pattern(r.pattern);
	if (!r.match.isValid())
		return refuse(QStringLiteral("a redirect rule whose pattern does not compile"));
	if (out)
		*out = r;
	return true;
}

void redirect_set::replace(const QList<redirect_rule> &rules) {
	QWriteLocker guard(&m_lock);
	m_rules = rules;
	m_by_host.clear();
	m_unhosted.clear();
	for (int i = 0; i < m_rules.size(); ++i) {
		if (m_rules[i].host.isEmpty())
			m_unhosted << i;
		else
			m_by_host[m_rules[i].host] << i;
	}
}

int redirect_set::size() const {
	QReadLocker guard(&m_lock);
	return int(m_rules.size());
}

QString redirect_set::resource_for(const QString &url, const QString &request_host,
                                   const QString &site_host, unsigned type,
                                   bool blocked) const {
	QReadLocker guard(&m_lock);
	if (m_rules.isEmpty())
		return QString();
	QList<int> candidates = m_unhosted;
	const QString host = request_host.toLower();
	for (QString h = host; !h.isEmpty();) {
		const auto it = m_by_host.constFind(h);
		if (it != m_by_host.constEnd())
			candidates += it.value();
		const int dot = h.indexOf(QLatin1Char('.'));
		if (dot < 0)
			break;
		h = h.mid(dot + 1);
	}
	const QString site = site_host.toLower();
	const bool same_party = !site.isEmpty() &&
	  policy_engine::etld_plus_one(host) == policy_engine::etld_plus_one(site);
	const redirect_rule *best = nullptr;
	for (int i : candidates) {
		const redirect_rule &r = m_rules[i];
		if (r.rule_only && !blocked)
			continue;
		if (r.types && type != request_type::unknown && !(r.types & type))
			continue;
		if (r.party == 1 && !same_party)
			continue;
		if (r.party == 3 && same_party)
			continue;
		if (!r.include.isEmpty()) {
			bool in = false;
			for (const QString &d : r.include)
				in = in || site_in(site, d);
			if (!in)
				continue;
		}
		bool out = false;
		for (const QString &d : r.exclude)
			out = out || site_in(site, d);
		if (out)
			continue;
		if (best && best->priority >= r.priority)
			continue;
		if (!r.match.match(url).hasMatch())
			continue;
		best = &r;
	}
	return best ? best->resource : QString();
}
