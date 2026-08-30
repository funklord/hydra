#include "address_input.h"

#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace {

// A scheme, in the sense the address bar cares about: letters, then a colon.
// Deliberately not a list of known schemes -- `magnet:`, `mailto:` and
// whatever a page invents all mean "this is an address" here, and which of
// them the shell can actually open is a later question that scheme_rules
// answers. What this rejects is the accident: `note: buy milk` has a colon and
// is not a scheme, because a scheme cannot contain a space and must start with
// a letter.
bool carries_scheme(const QString &text) {
	const int colon = text.indexOf(':');
	if (colon <= 0)
		return false;
	const QString scheme = text.left(colon);
	if (!scheme.at(0).isLetter())
		return false;
	for (const QChar c : scheme)
		if (!c.isLetterOrNumber() && c != '+' && c != '-' && c != '.')
			return false;
	// **And something has to follow it that a url could start with.** Checking
	// only the left of the colon called `note: buy milk` a scheme, because
	// `note` is a perfectly good scheme name -- the give-away is the space
	// after the colon, which no url has. Written as "not whitespace, and not
	// nothing" rather than as a url parse, because the shell routes schemes it
	// has never heard of and must not require them to look like http.
	const QString rest = text.mid(colon + 1);
	return !rest.isEmpty() && !rest.at(0).isSpace();
}

bool is_path(const QString &text) {
	for (const char *p : { "/", "./", "../", "~/" })
		if (text.startsWith(QLatin1String(p)))
			return true;
	return false;
}

// Split a trailing `:port` off, so `example.com:8080` and `[::1]:8080` are
// judged on the host. A port is digits and nothing else; `foo:bar` keeps its
// colon and fails the host tests below, which is correct -- it is not a host.
QString without_port(const QString &text) {
	const int colon = text.lastIndexOf(':');
	if (colon < 0 || text.indexOf(']') > colon)
		return text;
	const QString tail = text.mid(colon + 1);
	if (tail.isEmpty())
		return text;
	for (const QChar c : tail)
		if (!c.isDigit())
			return text;
	return text.left(colon);
}

// **QHostAddress on its own is far too generous here.** It accepts the classic
// inet_aton short forms, where a missing octet is filled from the last one --
// so `3.14` parses happily as 3.0.0.14, and a version number typed into the
// address bar becomes an address instead of a search. That is not a fault in
// QHostAddress, which is answering a different question: `ping 3.14` really
// does reach 3.0.0.14.
//
// So a dotted address has to be a full quad before it is believed. Anything
// carrying a colon is IPv6 and QHostAddress can be trusted with it, because
// nothing anybody searches for is shaped like one.
bool is_ip_literal(const QString &host) {
	QString bare = host;
	if (bare.startsWith('[') && bare.endsWith(']'))
		bare = bare.mid(1, bare.size() - 2);
	if (!bare.contains(':') && bare.split('.').size() != 4)
		return false;
	return QHostAddress(bare).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol;
}

// The last label, and whether it reads as a top-level domain: two or more
// characters, all letters. This is the whole reason `3.14` is a search and
// `example.com` is not.
bool has_tld(const QString &host) {
	const QStringList labels = host.split('.');
	if (labels.size() < 2)
		return false;
	const QString last = labels.last();
	if (last.size() < 2)
		return false;
	for (const QChar c : last)
		if (!c.isLetter())
			return false;
	// A label may not be empty: `example..com` and a trailing dot are typos
	// rather than hosts, and treating them as addresses produces a load error
	// where a search would have produced an answer.
	for (const QString &l : labels)
		if (l.isEmpty())
			return false;
	return true;
}

}  // namespace

bool looks_like_address(const QString &text) {
	const QString t = text.trimmed();
	if (t.isEmpty())
		return false;

	// Before the whitespace test, both of them: somebody who typed a scheme or
	// a path meant an address, and a space inside one is the engine's problem
	// rather than a reason to search for it.
	if (carries_scheme(t) || is_path(t))
		return true;

	// Any whitespace at all, and it is not a host. Hostnames cannot contain
	// spaces, so this is not a heuristic.
	for (const QChar c : t)
		if (c.isSpace())
			return false;

	// **Path first, then port.** These were the other way round, and the order
	// is the whole bug: `without_port` only strips a trailing `:port` when what
	// follows the colon is digits and nothing else, so in `127.0.0.1:8753/admin`
	// the candidate was `8753/admin`, nothing was stripped, and the host came
	// out as `127.0.0.1:8753` -- which is not an address, so it was searched
	// for instead.
	//
	// A hostname survived that by accident (`example.com:8080/path` still looks
	// domain-shaped with the port attached) so only bare IPs fell through, which
	// is the worst case to lose: `192.168.1.1:631/printers` and every router
	// admin page are exactly the addresses somebody types with a port and a
	// path -- and this file exists because sending one of those to a search
	// engine is a privacy failure, not a missed navigation.
	const QString host = without_port(t.split('/').first());
	if (host.isEmpty())
		return false;
	if (is_ip_literal(host))
		return true;
	if (host.compare("localhost", Qt::CaseInsensitive) == 0)
		return true;
	return has_tld(host);
}

QUrl search_url(const QString &terms, const QString &tmpl) {
	// A template with no placeholder would otherwise search for nothing, every
	// time, with no sign that the setting is wrong -- the request would go out
	// and a results page for the empty string would come back.
	if (!tmpl.contains("%1"))
		return QUrl();
	const QString encoded =
	  QString::fromLatin1(QUrl::toPercentEncoding(terms.trimmed()));
	return QUrl(QString(tmpl).replace("%1", encoded));
}
