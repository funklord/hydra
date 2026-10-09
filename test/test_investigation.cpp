// The Annoyed button's model loop, driven by a scripted model against a fake
// page. What is worth proving offline is the part a model cannot be trusted
// with: that every rule it proposes passes the gates a subscribed rule does,
// that a refusal reaches it with the reason, and that the loop ends -- at
// `done`, at the turn budget, on a model that has stopped following the
// protocol, and on a provider that fails.
#include "investigation.h"
#include "ai_provider.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Answers from a script, one per `send`, on the next turn of the event loop
// as a real provider would. An empty script entry is a failure.
class scripted_model : public ai_provider {
public:
	QStringList replies;
	QStringList prompts;
	bool external = false;
	QString name() const override { return QStringLiteral("scripted"); }
	bool available() const override { return true; }
	bool is_external() const override { return external; }
	void send(const QString &, const QString &user) override {
		prompts << user;
		const QString r = replies.isEmpty() ? QStringLiteral("{\"action\":\"look\"}")
		                                    : replies.takeFirst();
		QTimer::singleShot(0, this, [this, r] {
			if (r.isEmpty())
				emit failed(QStringLiteral("connection refused"));
			else
				emit finished(r);
		});
	}
};

// A page that shows an ad until a rule naming it is in trial.
class fake_page : public investigation_host {
public:
	QStringList showing { "element: div#ad-slot, 300x250, named \"ad-slot\"" };
	QList<filter_rule> tried;
	QList<scriptlet_call> calls;
	int looks = 0, trials = 0, asks = 0;
	QString answer = QStringLiteral("yes");
	void look(std::function<void(const QStringList &)> done) override {
		++looks;
		QTimer::singleShot(0, [this, done] { done(showing); });
	}
	void trial(const QList<filter_rule> &rules, const QList<scriptlet_call> &c,
	           std::function<void(const QStringList &)> done) override {
		++trials;
		tried = rules;
		calls = c;
		bool hidden = false;
		for (const filter_rule &r : rules)
			hidden = hidden || r.text.contains("#ad-slot");
		if (hidden)
			showing.clear();
		QTimer::singleShot(0, [this, done] { done(showing); });
	}
	void ask(const QString &, const QString &,
	         std::function<void(const QString &)> done) override {
		++asks;
		QTimer::singleShot(0, [this, done] { done(answer); });
	}
};

static investigation_evidence evidence() {
	investigation_evidence e;
	e.host = "news.example";
	e.page = "https://news.example/story";
	e.detected = { "element: div#ad-slot, 300x250, named \"ad-slot\"" };
	e.observed = { "https://news.example/story", "https://news.example/app.js" };
	return e;
}

// Runs until `finished`, or a deadline, and says which.
static bool run(investigation &inv, bool *solved, QString *summary) {
	bool done = false;
	QObject::connect(&inv, &investigation::finished,
	                  [&](bool s, const QString &m) {
		done = true;
		*solved = s;
		*summary = m;
	});
	inv.start();
	QEventLoop loop;
	QTimer poll;
	QObject::connect(&poll, &QTimer::timeout, [&] { if (done) loop.quit(); });
	poll.start(5);
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	loop.exec();
	return done;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("an action is found in what a model actually writes");
	{
		check(investigation::action_in("{\"action\":\"look\"}")
		          .value("action").toString() == "look",
		       "a bare object");
		check(investigation::action_in(
		          "Sure! Here you go:\n```json\n{\"action\": \"done\", "
		          "\"solved\": true, \"summary\": \"a } in {text}\"}\n```")
		          .value("summary").toString() == "a } in {text}",
		       "inside prose and a code fence, with braces in a string");
		check(investigation::action_in("{\"note\": 1} then {\"action\":\"ask\"}")
		          .value("action").toString() == "ask",
		       "the first object that names an action, not the first object");
		check(investigation::action_in("I think we should look again.").isEmpty(),
		       "and nothing from a reply with no action");
	}

	section("a run: look, try with refusals, ask, done");
	{
		scripted_model model;
		model.replies = {
			"{\"action\":\"look\",\"why\":\"see what is there\"}",
			"{\"action\":\"try\",\"why\":\"hide the slot\",\"rules\":["
			  "\"news.example###ad-slot\","
			  "\"##div\","
			  "\"other.example##.ad\","
			  "\"news.example##+js(trusted-set-cookie, a, b)\","
			  "\"news.example##+js(set, adsEnabled, false)\"]}",
			"{\"action\":\"ask\",\"kind\":\"confirm\",\"question\":\"Does it look right?\"}",
			"{\"action\":\"done\",\"solved\":true,\"summary\":\"slot hidden\"}",
		};
		fake_page page;
		investigation inv(&model, &page, evidence());
		QStringList log;
		QObject::connect(&inv, &investigation::logged,
		                  [&log](const QString &k, const QString &t) {
			log << k + ": " + t;
		});
		bool solved = false;
		QString summary;
		const bool ended = run(inv, &solved, &summary);
		check(ended, "it finishes");
		check(solved && summary == "slot hidden",
		       QString("solved, with the model's summary (%1)").arg(summary));
		check(page.looks == 1 && page.trials == 1 && page.asks == 1,
		       QString("one look, one trial, one question (%1/%2/%3)")
		           .arg(page.looks).arg(page.trials).arg(page.asks));
		check(page.tried.size() == 1 && page.tried.first().text ==
		          "news.example###ad-slot",
		       QString("only the scoped cosmetic rule reached the page as a rule "
		               "(%1)").arg(page.tried.size()));
		check(page.calls.size() == 1 && page.calls.first().name == "set-constant",
		       "and the untrusted scriptlet as a call");
		const QString all = log.join("\n");
		check(all.contains("Refused: ##div") &&
		          all.contains("Refused: other.example##.ad (scoped to another "
		                       "site)") &&
		          all.contains("Refused: news.example##+js(trusted-set-cookie"),
		       "the generic, the other site's and the trusted one are refused, "
		       "each logged");
		check(model.prompts.size() == 4 &&
		          model.prompts.at(2).contains("refused: ##div") &&
		          model.prompts.at(2).contains("nothing ad-like is visible"),
		       "and the next prompt tells the model what was refused and what "
		       "the page then showed");
		check(inv.trial_rules() ==
		          QStringList({ "news.example###ad-slot",
		                        "news.example##+js(set, adsEnabled, false)" }),
		       QString("what Keep would save is what is in trial (%1)")
		           .arg(inv.trial_rules().join(" | ")));
		check(all.contains("ask: Does it look right?") &&
		          all.contains("Answered: yes"),
		       "the question and its answer are in the log");
	}

	section("a model that stops following the protocol is stopped");
	{
		scripted_model model;
		model.replies = { "I would look at the page.", "Let me think." };
		fake_page page;
		investigation inv(&model, &page, evidence());
		bool solved = true;
		QString summary;
		// Run first: a check's message is built before its condition runs,
		// so a `run` inside it would print the state from before.
		const bool ended = run(inv, &solved, &summary);
		check(ended && !solved &&
		          summary.contains("protocol") && model.prompts.size() == 2,
		       QString("after two replies with no action (%1, %2 prompts)")
		           .arg(summary).arg(model.prompts.size()));
		check(model.prompts.at(1).contains("no JSON action"),
		       "having told it so once");
	}

	section("the loop ends at its budget");
	{
		scripted_model model;   // an empty script answers "look" forever
		fake_page page;
		investigation inv(&model, &page, evidence());
		bool solved = true;
		QString summary;
		const bool ended = run(inv, &solved, &summary);
		check(ended && !solved &&
		          model.prompts.size() == investigation::k_max_turns,
		       QString("%1 turns, then it stops (%2)")
		           .arg(model.prompts.size()).arg(summary));
	}

	section("a provider that fails ends it, and a stop is a stop");
	{
		scripted_model model;
		model.replies = { QString() };
		fake_page page;
		investigation inv(&model, &page, evidence());
		bool solved = true;
		QString summary;
		const bool ended = run(inv, &solved, &summary);
		check(ended && !solved && summary.contains("connection refused"),
		       QString("the failure is the reason given (%1)").arg(summary));

		scripted_model slow;
		fake_page page2;
		investigation two(&slow, &page2, evidence());
		bool stopped = false;
		QObject::connect(&two, &investigation::finished,
		                  [&stopped](bool, const QString &m) {
			stopped = m == "stopped";
		});
		two.start();
		two.stop();
		QEventLoop l;
		QTimer::singleShot(100, &l, &QEventLoop::quit);
		l.exec();
		check(stopped && !two.running() && page2.looks == 0,
		       "stopping before the answer arrives acts on nothing");
	}

	section("an estimate is given, and learned");
	{
		scripted_model model;
		model.replies = { "{\"action\":\"look\"}",
		                  "{\"action\":\"done\",\"solved\":false}" };
		fake_page page;
		investigation inv(&model, &page, evidence());
		QList<int> etas;
		QObject::connect(&inv, &investigation::expecting,
		                  [&etas](const QString &what, int s) {
			if (what.contains("model"))
				etas << s;
		});
		bool solved = true;
		QString summary;
		run(inv, &solved, &summary);
		check(etas.size() == 2 && etas.first() == 30 && etas.last() <= 2,
		       QString("a default for a local model first, then this session's "
		               "own measure (%1, %2)")
		           .arg(etas.value(0)).arg(etas.value(1)));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
