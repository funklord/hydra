#pragma once
// **A name for this browser's own page messages that no page can predict.**
// The consent blocker and sponsor skip talk between frames with
// `postMessage`, and every `message` listener on the page receives what
// they send. Their keys used to be `__hydra_consent_*` and
// `__hydra_sponsor_*`, which put this browser's name in front of any page
// that listened (2026-11-02). Each key is now a random stem chosen once per
// process, so every frame of a page -- all injected by this process -- still
// agrees on it, and nothing a page reads names anybody.
//
// `disguise_keys` rewrites a script's source: every `__hydra_<what>_`
// becomes `__<stem><what>_`. The guards a script sets on `window` keep their
// names, because on the desktop they live in an isolated world and on
// Android the bundle moves them into a private object (`bundled`).
#include <QRandomGenerator>
#include <QString>

namespace page_key {

inline QString stem() {
	static const QString s = [] {
		static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
		QString out;
		for (int i = 0; i < 10; ++i)
			out += QLatin1Char(letters[QRandomGenerator::global()->bounded(26)]);
		return out;
	}();
	return s;
}

inline QString disguise_keys(QString source, const char *what) {
	const QString from = QStringLiteral("__hydra_") + QLatin1String(what) + QLatin1Char('_');
	const QString to = QStringLiteral("__") + stem() + QLatin1String(what) + QLatin1Char('_');
	return source.replace(from, to);
}

}  // namespace page_key
