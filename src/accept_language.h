#pragma once

#include <QLocale>
#include <QString>
#include <QStringList>

// What languages this browser asks for.
//
// **It asked for none.** Measured by logging every header of a real request:
// `Accept`, `Accept-Encoding`, `Sec-Fetch-*` and the client hints were all
// there, and `Accept-Language` was simply absent, because nothing ever called
// `setHttpAcceptLanguage` and Qt supplies no default. Every browser sends one.
//
// What it costs is not abstract. A site with more than one language has
// nothing to negotiate against, so it serves whatever it defaults to -- often
// English, sometimes a guess from the address -- and a Swedish reader on a
// Swedish site gets the wrong one with no way to say otherwise short of
// clicking a flag. It is also a fingerprinting signal in its own right: no
// real browser omits this, so omitting it is distinctive.
namespace accept_language {

// `QLocale::system().uiLanguages()` in, an `Accept-Language` value out.
//
// **Pure, because the input is messier than it looks and that is worth
// testing.** Measured on this machine it answers
//
//     en-US, en-Latn-US, en, en, en-Latn-US, en-US
//
// -- duplicated, and carrying script subtags that no browser sends. Chrome
// sends `en-US,en;q=0.9`. So the transformation drops the script, removes
// repeats while keeping the order, and gives each entry after the first a
// descending quality.
inline QString header_for(const QStringList &ui_languages, int max_entries = 4) {
	QStringList out;
	for (const QString &raw : ui_languages) {
		QString tag = raw;
		// xx-Yyyy-ZZ becomes xx-ZZ: the middle part is a script subtag, four
		// letters by definition, and it is the piece browsers leave out.
		const QStringList parts = tag.split('-');
		if (parts.size() == 3 && parts.at(1).size() == 4)
			tag = parts.at(0) + "-" + parts.at(2);
		if (tag.isEmpty() || out.contains(tag, Qt::CaseInsensitive))
			continue;
		out << tag;
		if (out.size() >= max_entries)
			break;
	}
	if (out.isEmpty())
		return QString();

	QString header = out.first();
	// 0.9 downwards, which is what Chrome does, and never below 0.1 -- a
	// quality of zero means "not acceptable" and would say the opposite of
	// what a list of preferences is for.
	double q = 0.9;
	for (int i = 1; i < out.size() && q >= 0.1; ++i, q -= 0.1)
		header += QString(",%1;q=%2").arg(out.at(i)).arg(q, 0, 'g', 2);
	return header;
}

// The one this machine should send.
inline QString system_header() {
	return header_for(QLocale::system().uiLanguages());
}

}  // namespace accept_language
