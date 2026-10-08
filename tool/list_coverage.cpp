// What this build takes from a real filter list.
//
// **The number this answers is the one that goes stale fastest.** project.md
// records what fraction of uBlock's scriptlet rules this catalog enforces,
// and that figure moves whenever the catalog grows or an upstream list is
// re-published -- so it needs a way to be re-taken rather than a sentence
// somebody has to trust. It existed as a scratch program first, which is the
// shape `evidence.md` warns about: a fact whose method cannot be re-run.
//
// It reads a list through `filter_subscription::read`, the same parser the
// shell uses, rather than a second implementation that could agree with
// itself and be wrong. It fetches nothing: a build does not go to the
// network, and the lists this project ships are named below so a person knows
// what to download.
#include "filter_subscription.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QList>
#include <QPair>

#include <algorithm>
#include <cstdio>

namespace {

// Which scriptlet names a list asks for that this build refuses, and how
// often. **Counted from the lines rather than from the report**, because a
// refusal does not appear in `subscription_read` at all -- it is the
// difference between what the list asked for and what came back, and naming
// the difference is the whole point of running this.
QList<QPair<int, QString>> refused_names(const QString &text) {
	QHash<QString, int> seen;
	for (const QString &line : text.split(QLatin1Char('\n'))) {
		const int at = line.indexOf(QStringLiteral("##+js("));
		if (at < 0)
			continue;
		QString inside = line.mid(at + 6).trimmed();
		if (inside.endsWith(QLatin1Char(')')))
			inside.chop(1);
		scriptlet_call call;
		if (scriptlets::parse_call(inside, &call, nullptr))
			continue;
		const QString asked = inside.section(QLatin1Char(','), 0, 0).trimmed();
		seen[asked.isEmpty() ? QStringLiteral("(unnamed)") : asked] += 1;
	}
	QList<QPair<int, QString>> out;
	for (auto it = seen.constBegin(); it != seen.constEnd(); ++it)
		out.append({ it.value(), it.key() });
	std::sort(out.begin(), out.end(),
	           [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	return out;
}

int report(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		std::printf("%s: cannot be read\n", qPrintable(path));
		return 1;
	}
	const QString text = QString::fromUtf8(f.readAll());
	// **Read twice, because the trust flag changes what is kept.** Reporting
	// one number would hide how much of a list is waiting on a tick.
	const subscription_read plain   = filter_subscription::read(text);
	const subscription_read trusted = filter_subscription::read(text, 0, true);

	int js_lines = 0;
	for (const QString &line : text.split(QLatin1Char('\n')))
		if (line.contains(QStringLiteral("##+js(")))
			++js_lines;

	std::printf("\n%s\n", qPrintable(path));
	std::printf("  candidate lines   %d\n", trusted.lines);
	std::printf("  network+cosmetic  %d rule(s) in force\n", trusted.accepted);
	std::printf("  scriptlets        %d of %d asked for",
	             trusted.scriptlets, js_lines);
	if (plain.scriptlets != trusted.scriptlets)
		std::printf("  (%d without trust)", plain.scriptlets);
	std::printf("\n");
	std::printf("  cannot enforce    %d line(s)\n", trusted.unsupported);
	std::printf("  refused as unsafe %d\n", trusted.unsafe);
	if (!trusted.ok())
		std::printf("  REFUSED WHOLE: %s\n", qPrintable(trusted.refusal));

	const QList<QPair<int, QString>> refused = refused_names(text);
	if (refused.isEmpty()) {
		// **A list naming none and a list naming only implemented ones are
		// different facts.** The first version printed "every scriptlet this
		// list names is implemented" for EasyList, which names zero -- a
		// true sentence that reads as coverage, which is the shape this
		// project calls a vacuous pass.
		std::printf(js_lines == 0
		             ? "  this list names no scriptlets at all\n"
		             : "  every scriptlet this list names is implemented\n");
		return 0;
	}
	int lost = 0;
	for (const auto &r : refused)
		lost += r.first;
	std::printf("  %d scriptlet rule(s) refused, across %d name(s):\n",
	             lost, int(refused.size()));
	for (const auto &r : refused)
		std::printf("    %6d  %s\n", r.first, qPrintable(r.second));
	return 0;
}

}  // namespace

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	if (argc < 2) {
		std::printf("usage: list-coverage <filter-list.txt> [...]\n\n"
		             "Reads each list through this build's own parser and says "
		             "what it takes\nfrom it. Fetches nothing. The lists this "
		             "build subscribes to by default:\n\n");
		for (const subscription &s : filter_subscription::default_subscriptions())
			std::printf("  %-18s %s\n", qPrintable(s.name),
			             qPrintable(s.url.toString()));
		std::printf("\n");
		return 2;
	}
	int bad = 0;
	for (int i = 1; i < argc; i++)
		bad += report(QString::fromLocal8Bit(argv[i]));
	return bad == 0 ? 0 : 1;
}
