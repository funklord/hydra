// How good a real model is at reading what a video is, task by task, on the
// labelled corpus -- the measurement behind "signal if the model is too weak
// and what tasks fall outside its comfort zone".
//
//     HYDRA_MODEL=qwen2.5:7b HYDRA_MODEL_ENDPOINT=http://localhost:11434 \
//         QT_QPA_PLATFORM=offscreen ./test/build-make/try_media_model
//
// Each case is one request, bounded at three minutes. Printed: each reading
// with what its checks rejected, then per task how often it was right, the
// tasks under `corpus_score::k_comfort`, and what the in-browser tally would
// warn about. Exits non-zero only when the model could not be asked at all:
// a weak model is a finding, not a failure of this driver.
#include "media_corpus.h"
#include "media_interpretation.h"
#include "model_tally.h"
#include "ollama_provider.h"

#include <QApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <cstdio>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	const QString model = qEnvironmentVariableIsSet("HYDRA_MODEL")
	  ? qEnvironmentVariable("HYDRA_MODEL") : QStringLiteral("qwen2.5-coder:14b");
	const QString endpoint = qEnvironmentVariableIsSet("HYDRA_MODEL_ENDPOINT")
	  ? qEnvironmentVariable("HYDRA_MODEL_ENDPOINT")
	  : QStringLiteral("http://localhost:11434");
	ollama_provider ai;
	ai.set_endpoint(QUrl(endpoint));
	ai.set_model(model);
	QString why;
	if (!ai.probe_now())
		why = QStringLiteral("nothing answers there");
	if (!why.isEmpty() || !ai.ready(&why)) {
		std::printf("cannot ask %s at %s: %s\n", qPrintable(model),
		            qPrintable(endpoint), qPrintable(why));
		return 2;
	}

	QString dir = QStringLiteral("test/fixture/media");
	if (!QFileInfo(dir + "/labels.json").exists())
		dir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() +
		      "/../fixture/media";
	const QList<corpus_case> corpus = load_corpus(dir);
	std::printf("%s at %s, %lld cases\n", qPrintable(model), qPrintable(endpoint),
	            qint64(corpus.size()));

	corpus_score score, rules;
	model_tally tally;
	for (const corpus_case &c : corpus) {
		media_interpretation step(&ai, c.evidence);
		media_reading got;
		bool done = false;
		QObject::connect(&step, &media_interpretation::finished,
		                  [&](const media_reading &r) { got = r; done = true; });
		QEventLoop loop;
		QTimer::singleShot(180000, &loop, &QEventLoop::quit);
		QTimer poll;
		QObject::connect(&poll, &QTimer::timeout, [&] { if (done) loop.quit(); });
		poll.start(50);
		step.start();
		loop.exec();
		if (!done) {
			std::printf("\n%s: no answer in three minutes\n", qPrintable(c.name));
			got = media_interpretation::by_rules(c.evidence);
			got.rejected << QStringLiteral("answer: timed out");
		}
		for (const QString &task : { "kind", "artist", "title", "tracklist" })
			if (got.model_answered)
				tally.record(model, task, got.from_model.contains(task));
		if (!got.model_answered)
			tally.record(model, "answer", false);

		++score.total;
		++rules.total;
		const auto s = score_case(got, c);
		const auto b = score_case(media_interpretation::by_rules(c.evidence), c);
		QStringList wrong;
		for (auto it = s.cbegin(); it != s.cend(); ++it) {
			score.right[it.key()] += it.value() ? 1 : 0;
			if (!it.value())
				wrong << it.key();
		}
		for (auto it = b.cbegin(); it != b.cend(); ++it)
			rules.right[it.key()] += it.value() ? 1 : 0;
		std::printf("\n%s: %s | %s | %s | set=%d | tracklist=%s | confidence %.2f\n",
		            qPrintable(c.name), qPrintable(media_reading::name_of(got.what)),
		            qPrintable(got.artist), qPrintable(got.title), int(got.is_set),
		            qPrintable(got.tracklist.isEmpty() ? "-" : got.tracklist),
		            got.confidence);
		if (!got.rejected.isEmpty())
			std::printf("   rejected: %s\n", qPrintable(got.rejected.join("; ")));
		if (!wrong.isEmpty())
			std::printf("   wrong against the label: %s\n",
			            qPrintable(wrong.join(", ")));
	}

	std::printf("\nper task, right of %d (rules alone in brackets):\n", score.total);
	for (auto it = score.right.cbegin(); it != score.right.cend(); ++it)
		std::printf("  %-10s %2d  (%d)\n", qPrintable(it.key()), it.value(),
		            rules.right.value(it.key()));
	const QStringList outside = score.outside();
	std::printf("\noutside the comfort zone (under %d%%): %s\n",
	            int(corpus_score::k_comfort * 100),
	            outside.isEmpty() ? "none" : qPrintable(outside.join(", ")));
	for (const QString &w : tally.warnings(model))
		std::printf("tally: %s\n", qPrintable(w));
	return 0;
}
