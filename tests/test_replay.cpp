// Re-score recorded model replies against the current gate, with no model.
//
// **Why this exists.** Measuring a change to the extractor loop meant ten runs
// of a 14B on CPU, fifteen minutes each when the machine is quiet, and this
// machine is never quiet. Three of five runs in the last measurement never
// answered at all, which is not a result about the model.
//
// But most of what gets changed is not the model. The gate's rules, the fields
// handed to the script, the shape of the evidence — all of it is deterministic
// code, and the replies the model has already given are on disk. Scoring those
// again costs milliseconds. What it cannot measure is a change to the *prompt*,
// because that changes what the model would say; the corpus is fixed replies
// and it stays honest by saying so.
//
// **It is validated against rates already measured**, and that is the whole
// design. Each capture records what the gate accepted when those replies were
// produced. A replay that cannot reproduce those numbers is not a cheaper
// measurement, it is a different one — so a mismatch is a failure here rather
// than a footnote.
#include "site_extractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}

// The gate compares normalised urls, and the corpus stores them as written.
static QString norm(const QString &u) {
	return QUrl(u).adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash)
	        .toString();
}

static QList<evidence_request> load_evidence(const QString &path, QString *host) {
	QList<evidence_request> ev;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return ev;
	const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	if (host)
		*host = root.value("host").toString();
	const QJsonArray reqs = root.value("requests").toArray();
	int n = 0;
	for (const QJsonValue &v : reqs) {
		const QJsonObject o = v.toObject();
		evidence_request r;
		r.url = QUrl(o.value("url").toString());
		// The capture records "script"/"image"/other exactly as the interceptor
		// saw it, which is the vocabulary the gate and the script both use.
		r.kind = o.value("kind").toString();
		if (r.kind.isEmpty())
			r.kind = "other";
		r.order = n++;
		ev << r;
	}
	return ev;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QString dir = qEnvironmentVariableIsSet("HYDRA_REPLIES")
	                        ? QString::fromLocal8Bit(qgetenv("HYDRA_REPLIES"))
	                        : QStringLiteral("evidence/replies");
	const QString index = dir + "/corpus.ini";
	if (!QFile::exists(index)) {
		// Said loudly and failed, not skipped. A corpus suite that quietly
		// reports success when it scored nothing is the same trap as a driver
		// that writes no screenshots and prints "done" — this project has been
		// caught by that once already.
		std::printf("no corpus at %s\n"
		             "  The replies live beside the captures in evidence/, which is not "
		             "in git.\n  See evidence/README.md; there is nothing to measure "
		             "without them.\n", qPrintable(index));
		return 1;
	}

	QSettings ini(index, QSettings::IniFormat);
	int captures = 0;
	for (const QString &group : ini.childGroups()) {
		ini.beginGroup(group);
		const QString ev_path = dir + "/../" + ini.value("evidence").toString();
		const QUrl    page(ini.value("page").toString());
		const QString prefix = ini.value("replies").toString();
		const QString note   = ini.value("note").toString();

		// **`toStringList`, not `toString().split(',')`.** QSettings parses a
		// comma-separated INI value as a list, and `toString()` on a list
		// variant yields the single element when there is one and an *empty
		// string* when there are several. Every entry here carried one url
		// until one carried two, at which point the whole field silently
		// vanished and three replies scored as accepted that the gate had
		// refused. The one-element case had been working by luck.
		QSet<QString> manifests;
		for (const QString &m : ini.value("manifests").toStringList())
			if (!m.trimmed().isEmpty())
				manifests.insert(norm(m.trimmed()));
		// Addresses the dialog fetched at accept time and found were not
		// streams. `check()` does not do that fetch, so without this the replay
		// accepts picks the shipping gate refused.
		//
		// **Matched as prefixes**, because the exact addresses are not reliably
		// recoverable. The logs these were read from print a pick through
		// `left(140)` and an analytics beacon here is 677 characters, so copying
		// "the url" out of a log yields a string that matches nothing — which is
		// how this entry failed silently on its first attempt. A prefix naming
		// the endpoint is both recoverable and the true statement: it is that
		// endpoint that was fetched and found not to be a stream.
		QSet<QString> disproved;
		for (const QString &d : ini.value("disproved").toStringList())
			if (!d.trimmed().isEmpty())
				disproved.insert(d.trimmed());
		QSet<QString> truncated;
		for (const QString &t : ini.value("truncated").toStringList())
			if (!t.trimmed().isEmpty())
				truncated.insert(t.trimmed());
		QHash<QString, QString> expect_for;
		for (const QString &k : ini.allKeys())
			if (k.startsWith("verdict/"))
				expect_for.insert(k.mid(8), ini.value(k).toString());
		ini.endGroup();

		QString host;
		const QList<evidence_request> ev = load_evidence(ev_path, &host);
		std::printf("\n== %s (%s) ==\n", qPrintable(group), qPrintable(note));
		if (ev.isEmpty()) {
			check(false, QString("evidence loads from %1").arg(ev_path));
			continue;
		}

		QStringList files = QDir(dir).entryList({ prefix + "*.js" }, QDir::Files,
		                                         QDir::Name);
		if (files.isEmpty()) {
			check(false, QString("replies found for prefix %1").arg(prefix));
			continue;
		}

		int accepted = 0, scored = 0, skipped = 0;
		for (const QString &name : files) {
			if (truncated.contains(name)) {
				++skipped;
				std::printf("     %-46s %-8s recorded through left(1200); "
				             "not scoreable\n", qPrintable(name), "skip");
				continue;
			}
			QFile rf(dir + "/" + name);
			if (!rf.open(QIODevice::ReadOnly)) {
				check(false, QString("reply %1 reads").arg(name));
				continue;
			}
			const QString src = QString::fromUtf8(rf.readAll());
			extractor_verdict v =
			    site_extractor::check(src, page, ev, nullptr, &manifests);
			// The step check() cannot take: the shipping dialog fetches the pick
			// itself before offering it.
			QString note;
			if (v.usable) {
				const QString picked = v.result.url.toString();
				for (const QString &d : disproved) {
					if (!picked.startsWith(d))
						continue;
					v.usable = false;
					note = " [refused by fetching the pick]";
					break;
				}
			}
			++scored;
			if (v.usable)
				++accepted;
			// Printed per reply, because the rate is the least interesting thing
			// about these: *which* rule refused a reply is what says whether a
			// change helped, and a score of 2 hides five different reasons.
			std::printf("     %-46s %-8s %s%s\n", qPrintable(name),
			             v.usable ? "ACCEPT" : "refuse",
			             qPrintable(v.usable
			                            ? v.result.url.toString().left(60)
			                            : v.message.left(60)),
			             qPrintable(note));

			// **Per reply.** A rate agreed once while disagreeing about which
			// replies, one error cancelling another, and that is the failure
			// this whole file exists to make impossible.
			const QString want = expect_for.value(name);
			if (want.isEmpty()) {
				check(false, QString("%1 has a recorded verdict in the corpus")
				                 .arg(name));
				continue;
			}
			const bool want_accept = (want == "accept");
			check(v.usable == want_accept,
			      QString("%1: %2 now, %3 when it was produced")
			          .arg(name, v.usable ? "accepted" : "refused",
			                want_accept ? "accepted" : "refused"));
		}
		std::printf("     -- %d of %d accepted", accepted, scored);
		if (skipped)
			std::printf(", %d not scoreable", skipped);
		std::printf(" --\n");
		++captures;
	}

	check(captures > 0, "the corpus held at least one capture");
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
