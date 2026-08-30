#pragma once

#include <QRegularExpression>
#include <QString>

// What this browser says it is.
//
// **Qt's default gets it turned away.** The string announces the Chromium it
// embeds and a token naming itself:
//
//     ... QtWebEngine/6.8.2 Chrome/122.0.6261.171 Safari/537.36
//
// Chrome 122 shipped in February 2024. A site that gates on a version reads
// that as a browser years out of date and refuses -- which is what a Swedish
// bank did, with "we no longer support the version of Google Chrome you are
// using". The `QtWebEngine/6.8.2` token is the other half: no real browser
// sends it, so a checker working from a list of known browsers has an unknown
// one in front of it.
//
// **Derived from Qt's own string rather than written out**, because the
// platform part -- `X11; Linux x86_64` here, something else elsewhere -- has
// to stay true, and a hardcoded user agent freezes it and is wrong on the next
// platform this builds for.
//
// **A claim about a version we do not have, and that is the trade.** The
// engine really is Chromium 122. A site relying on something only 140 has will
// now fail later rather than turning us away at the door, which is the worse
// failure in general and the better one here, because being refused up front
// is unconditional and cannot be worked around from inside the page.
//
// **What it does not fix, measured rather than assumed:** `sec-ch-ua` still
// reports `"Chromium";v="122"`. Chromium builds client hints from its real
// version and Qt exposes no override, so a site reading `navigator.
// userAgentData` instead of the string sees through this. A known limit.
namespace user_agent {

// The Chrome major to claim. **It has to be maintained**: nothing here can
// work out what today's Chrome is, and a number left alone for two years
// recreates the bug this exists to fix.
constexpr int k_claimed_chrome = 140;

// Qt's string in, the one to send out. Pure, so it can be tested without an
// engine -- the edge cases are what make it worth testing at all.
inline QString corrected(const QString &qt_default,
                          int claimed = k_claimed_chrome) {
	QString ua = qt_default;
	// The self-naming token, with its trailing space so nothing is left
	// doubled. Absent on a string that has already been through here, which is
	// why this is a remove rather than a required match.
	ua.remove(QRegularExpression(QStringLiteral("QtWebEngine/[0-9.]+ ")));

	// **The whole version, not just the major.** Replacing only the major left
	// `Chrome/140.0.6261.171` -- 140 wearing Chromium 122's build numbers, a
	// combination that has never shipped and is a worse fingerprint than the
	// honest string. Real Chrome has sent a *reduced* user agent since version
	// 101, freezing everything after the major to zero, so `Chrome/140.0.0.0`
	// is what a current browser actually says.
	ua.replace(QRegularExpression(QStringLiteral("Chrome/[0-9.]+")),
	            QStringLiteral("Chrome/%1.0.0.0").arg(claimed));
	return ua;
}

}  // namespace user_agent
