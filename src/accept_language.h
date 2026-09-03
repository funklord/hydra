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
// The one language tag to tell the engine it is running in, in the same
// vocabulary the header uses.
//
// **Chromium is not told otherwise and guesses badly.** Qt WebEngine passes no
// `--lang`, so Chromium's ICU falls back to a bare language: measured on a
// machine whose `LANG` is `en_US.UTF-8`, `Intl.DateTimeFormat().resolvedOptions()
// .locale` answered "en" in hydra and "en-US" in the Chromium beside it, and
// `Intl.Collator` and `Intl.NumberFormat` agreed with it. Everything else
// matched -- `Accept-Language`, `navigator.language` and `navigator.languages`
// were identical -- so a site comparing what the browser says it is against
// what it resolves to sees a browser disagreeing with itself.
//
// That is not cosmetic: a bare "en" sorts, formats numbers and formats dates by
// generic rules rather than the region's, and a site that stores a locale and
// checks it later reads a change that never happened. Teams shows "Language
// changes detected" on every load because of it.
//
// The script subtag goes for the same reason it goes in the header, and the
// first entry wins because `uiLanguages()` is already in preference order.
inline QString primary_tag(const QStringList &ui_languages) {
	for (const QString &raw : ui_languages) {
		if (raw.isEmpty())
			continue;
		const QStringList parts = raw.split('-');
		if (parts.size() == 3 && parts.at(1).size() == 4)
			return parts.at(0) + "-" + parts.at(2);
		return raw;
	}
	return QString();
}

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
