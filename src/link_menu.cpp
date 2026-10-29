#include "link_menu.h"

#include <QStringList>

namespace link_menu {

namespace {

bool web(const QUrl &u) {
	const QString s = u.scheme().toLower();
	return u.isValid() && !u.host().isEmpty() &&
	       (s == QLatin1String("http") || s == QLatin1String("https"));
}

const char *word(action a) {
	switch (a) {
		case action::open:            return "open";
		case action::open_background: return "background";
		case action::copy:            return "copy";
		case action::save:            return "save";
	}
	return "open";
}

}  // namespace

QList<entry> entries(const QUrl &link, const QUrl &image) {
	QList<entry> out;
	const QString scheme = link.scheme().toLower();
	if (web(link)) {
		const QString a = link.toString(QUrl::FullyEncoded);
		out << entry{ "Open in New Tab", action::open, a }
		    << entry{ "Open in Background Tab", action::open_background, a }
		    << entry{ "Copy Link Address", action::copy, a }
		    << entry{ "Save Link", action::save, a };
	} else if (scheme == QLatin1String("mailto") && !link.path().isEmpty()) {
		// A tap already hands these to the mail app; what a hold adds is the
		// address itself, without the scheme nobody wants pasted.
		out << entry{ "Copy Email Address", action::copy, link.path() };
	} else if (scheme == QLatin1String("tel") && !link.path().isEmpty()) {
		out << entry{ "Copy Phone Number", action::copy, link.path() };
	}
	// An image that is its own link is offered once, as a link.
	if (web(image) && image != link) {
		const QString a = image.toString(QUrl::FullyEncoded);
		out << entry{ "Open Image in New Tab", action::open, a }
		    << entry{ "Copy Image Address", action::copy, a }
		    << entry{ "Save Image", action::save, a };
	}
	return out;
}

QString encode(const QList<entry> &entries) {
	QStringList lines;
	for (const entry &e : entries) {
		// A tab or newline cannot come from an encoded address, and a label
		// is ours; the copy of a mailto path is the one value that could
		// carry one, and it is flattened rather than trusted.
		QString v = e.value;
		v.replace(QLatin1Char('\t'), QLatin1Char(' '));
		v.replace(QLatin1Char('\n'), QLatin1Char(' '));
		lines << QString("%1\t%2\t%3").arg(QLatin1String(word(e.what)), e.label, v);
	}
	return lines.join(QLatin1Char('\n'));
}

bool parse_action(const QString &w, action *out) {
	for (action a : { action::open, action::open_background, action::copy,
	                  action::save })
		if (w == QLatin1String(word(a))) {
			*out = a;
			return true;
		}
	return false;
}

}  // namespace link_menu
