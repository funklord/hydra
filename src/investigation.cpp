#include "investigation.h"

#include "ai_provider.h"
#include "filter_subscription.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

const char *k_system_prompt =
  "You are the ad-removal assistant inside a web browser. A person pressed a "
  "button saying an ad or nuisance got through on the page they are looking "
  "at. You work on it in turns: each turn you get the evidence and what has "
  "happened so far, and you reply with exactly ONE action as a JSON object, "
  "nothing else.\n\n"
  "Actions:\n"
  "  {\"action\": \"look\", \"why\": \"...\"}\n"
  "     look at the page again and report what is visibly ad-like.\n"
  "  {\"action\": \"try\", \"rules\": [\"...\"], \"why\": \"...\"}\n"
  "     apply these rules to this tab only, reload, and look. They replace "
  "the rules you tried before; an empty list removes them all. Rules use "
  "EasyList / uBlock Origin syntax and must be scoped to this site: "
  "\"site.example##selector\" hides elements, "
  "\"site.example##+js(name, args)\" runs a page patch (set-constant, "
  "json-prune, abort-on-property-read and the like), \"||host^\" blocks a "
  "third-party host.\n"
  "  {\"action\": \"ask\", \"kind\": \"confirm\", \"question\": \"...\"}\n"
  "     ask the person yes or no -- for example whether the page looks right "
  "now. Use sparingly.\n"
  "  {\"action\": \"ask\", \"kind\": \"point\", \"question\": \"...\"}\n"
  "     ask the person to click the thing on the page; you get its selector "
  "and markup. Use when you cannot find it yourself.\n"
  "  {\"action\": \"done\", \"solved\": true, \"summary\": \"...\"}\n"
  "     stop. solved is true only when the last look or the person says the "
  "ad is gone.\n\n"
  "Prefer the narrowest rule that works. Never hide generic tags or the "
  "page's main content. If a rule is refused you are told why.\n";

QString lines(const QStringList &l, const QString &none) {
	if (l.isEmpty())
		return "  " + none + "\n";
	QString out;
	for (const QString &s : l)
		out += "  " + s + "\n";
	return out;
}

}  // namespace

investigation::investigation(ai_provider *provider, investigation_host *host,
                             const investigation_evidence &evidence,
                             QObject *parent)
  : QObject(parent), m_provider(provider), m_host(host), m_evidence(evidence),
    m_last_seen(evidence.detected) {
	if (m_provider) {
		connect(m_provider, &ai_provider::finished, this,
		         &investigation::on_reply);
		connect(m_provider, &ai_provider::failed, this,
		         &investigation::on_failed);
	}
}

investigation::~investigation() {
	if (m_provider && m_waiting_model)
		m_provider->cancel();
}

void investigation::start() {
	if (m_running)
		return;
	if (!m_provider || !m_host) {
		emit logged("error", "No AI is set up, so there is nothing to ask.");
		emit finished(false, QStringLiteral("no AI available"));
		return;
	}
	m_running = true;
	emit logged("step", QString("Investigating %1 with %2, at most %3 turns.")
	                         .arg(m_evidence.host, m_provider->name())
	                         .arg(k_max_turns));
	next_turn();
}

void investigation::stop() {
	if (!m_running)
		return;
	if (m_provider && m_waiting_model)
		m_provider->cancel();
	m_waiting_model = false;
	finish(false, QStringLiteral("stopped"));
}

QStringList investigation::trial_rules() const {
	return m_trial_text;
}

// The evidence, the rules in trial, and the history -- everything, every
// turn, because the providers are stateless and a model that is not told what
// it tried will try it again.
QString investigation::prompt() const {
	QString p = "Page: " + m_evidence.page + "\nSite: " + m_evidence.host +
	            "\n\nWhen the person complained, the page showed:\n" +
	            lines(m_evidence.detected, "nothing the browser recognised");
	p += "\nPage patches that ran:\n" +
	     lines(m_evidence.patches, "none reported");
	p += "\nAd-shaped requests that got through:\n" +
	     lines(m_evidence.suspects.mid(0, 20), "none");
	p += "\nRules in trial now:\n" + lines(m_trial_text, "none");
	p += "\nWhat has happened so far:\n" +
	     lines(m_history, "nothing yet; this is the first turn");
	p += QString("\nThis is turn %1 of %2. Reply with one JSON action.\n")
	         .arg(m_turn).arg(k_max_turns);
	return p;
}

// From this session's own calls once there are any: the first guess is a
// guess, and a measured one is better from the second turn on.
int investigation::model_estimate() const {
	if (m_model_ms.isEmpty())
		return m_provider && m_provider->is_external() ? 15 : 30;
	qint64 sum = 0;
	for (qint64 ms : m_model_ms)
		sum += ms;
	return int(sum / m_model_ms.size() / 1000) + 1;
}

void investigation::next_turn() {
	if (!m_running)
		return;
	if (m_turn >= k_max_turns) {
		finish(false, QString("stopped after %1 turns without a fix")
		                  .arg(k_max_turns));
		return;
	}
	++m_turn;
	const int eta = model_estimate();
	emit logged("step", QString("Turn %1: asking %2.")
	                         .arg(m_turn).arg(m_provider->name()));
	emit expecting(QStringLiteral("the model's answer"), eta);
	m_waiting_model = true;
	m_clock.restart();
	m_provider->send(QString::fromLatin1(k_system_prompt), prompt());
}

QJsonObject investigation::action_in(const QString &reply) {
	for (int start = reply.indexOf('{'); start >= 0;
	     start = reply.indexOf('{', start + 1)) {
		// The closing brace of this object: depth-counted, outside strings.
		int depth = 0;
		bool in_string = false;
		for (int i = start; i < reply.size(); ++i) {
			const QChar c = reply.at(i);
			if (in_string) {
				if (c == '\\')
					++i;
				else if (c == '"')
					in_string = false;
				continue;
			}
			if (c == '"')
				in_string = true;
			else if (c == '{')
				++depth;
			else if (c == '}' && --depth == 0) {
				const QJsonDocument d =
				  QJsonDocument::fromJson(reply.mid(start, i - start + 1).toUtf8());
				if (d.isObject() && d.object().contains("action"))
					return d.object();
				break;
			}
		}
	}
	return QJsonObject();
}

void investigation::on_reply(const QString &reply) {
	if (!m_waiting_model || !m_running)
		return;
	m_waiting_model = false;
	m_model_ms << m_clock.elapsed();
	const QJsonObject a = action_in(reply);
	if (a.isEmpty()) {
		++m_bad_replies;
		emit logged("error", QString("The reply carried no action: %1")
		                          .arg(reply.simplified().left(160)));
		record("your reply had no JSON action in it; reply with exactly one");
		if (m_bad_replies >= 2) {
			finish(false, QStringLiteral("the model stopped following the "
			                              "protocol"));
			return;
		}
		next_turn();
		return;
	}
	m_bad_replies = 0;
	act(a);
}

void investigation::on_failed(const QString &error) {
	if (!m_waiting_model || !m_running)
		return;
	m_waiting_model = false;
	emit logged("error", "The model could not be reached: " + error);
	finish(false, "the model failed: " + error);
}

void investigation::record(const QString &what_happened) {
	m_history << QString("turn %1: %2").arg(m_turn).arg(what_happened);
}

void investigation::act(const QJsonObject &a) {
	const QString action = a.value("action").toString();
	const QString why = a.value("why").toString();

	if (action == "look") {
		emit logged("model", "Wants another look" +
		                         (why.isEmpty() ? QString() : ": " + why));
		emit expecting(QStringLiteral("a look at the page"), 2);
		m_host->look([this](const QStringList &seen) {
			if (!m_running)
				return;
			m_last_seen = seen;
			emit logged("page", seen.isEmpty()
			    ? QStringLiteral("Nothing ad-like is visible.")
			    : QString("Visible: %1").arg(seen.join("; ")));
			record("you looked; the page shows: " +
			       (seen.isEmpty() ? QStringLiteral("nothing ad-like")
			                       : seen.join("; ")));
			next_turn();
		});
		return;
	}

	if (action == "try") {
		QList<filter_rule> rules;
		QList<scriptlet_call> calls;
		QStringList text, refused;
		for (const QJsonValue &v : a.value("rules").toArray()) {
			const QString line = v.toString().trimmed();
			if (line.isEmpty())
				continue;
			filter_rule r;
			scriptlet_call call;
			QString said;
			const filter_subscription::line_kind k =
			  filter_subscription::classify(line, &r, &said, &call);
			if (k == filter_subscription::line_kind::scriptlet) {
				if (scriptlets::requires_trust(call.name))
					said = QStringLiteral("a trusted scriptlet, which a "
					                       "proposed rule may not use");
				else if (!filter_list::scope_matches(call.scope,
				                                      m_evidence.host))
					said = QStringLiteral("scoped to another site");
				else {
					calls << call;
					text << line;
					continue;
				}
			} else if (k == filter_subscription::line_kind::network ||
			           k == filter_subscription::line_kind::cosmetic) {
				const dry_run sim =
				  filter_list::evaluate(r, m_evidence.observed, m_evidence.host);
				if (sim.rejected)
					said = sim.reason;
				else if (r.cosmetic &&
				         !filter_list::scope_matches(r.scope, m_evidence.host))
					said = QStringLiteral("scoped to another site");
				else {
					rules << r;
					text << line;
					continue;
				}
			} else if (said.isEmpty()) {
				said = QStringLiteral("not a rule this browser can apply");
			}
			refused << QString("%1 (%2)").arg(line, said);
		}
		for (const QString &f : refused)
			emit logged("rule", "Refused: " + f);
		if (!why.isEmpty())
			emit logged("model", "Trying rules: " + why);
		for (const QString &t : text)
			emit logged("rule", "In trial: " + t);
		m_trial_rules = rules;
		m_trial_calls = calls;
		m_trial_text  = text;
		emit expecting(QStringLiteral("the page to reload with the trial rules"),
		               6);
		m_host->trial(rules, calls, [this, refused](const QStringList &seen) {
			if (!m_running)
				return;
			m_last_seen = seen;
			emit logged("page", seen.isEmpty()
			    ? QStringLiteral("After reloading, nothing ad-like is visible.")
			    : QString("After reloading, still visible: %1")
			          .arg(seen.join("; ")));
			QString what = QString("you tried %1 rule(s)").arg(m_trial_text.size());
			if (!refused.isEmpty())
				what += "; refused: " + refused.join("; ");
			what += seen.isEmpty()
			  ? QStringLiteral("; after reloading nothing ad-like is visible")
			  : "; after reloading the page still shows: " + seen.join("; ");
			record(what);
			next_turn();
		});
		return;
	}

	if (action == "ask") {
		const QString kind = a.value("kind").toString() == "point"
		                       ? QStringLiteral("point")
		                       : QStringLiteral("confirm");
		const QString question = a.value("question").toString().trimmed();
		emit logged("ask", question.isEmpty()
		                       ? QStringLiteral("(a question with no text)")
		                       : question);
		m_host->ask(kind, question, [this, kind, question](const QString &answer) {
			if (!m_running)
				return;
			emit logged("step", "Answered: " + answer);
			record(QString("you asked the person (%1) \"%2\"; they answered: %3")
			           .arg(kind, question, answer));
			next_turn();
		});
		return;
	}

	if (action == "done") {
		const bool solved = a.value("solved").toBool();
		const QString summary = a.value("summary").toString();
		finish(solved, summary.isEmpty()
		                   ? (solved ? QStringLiteral("solved")
		                             : QStringLiteral("not solved"))
		                   : summary);
		return;
	}

	++m_bad_replies;
	emit logged("error", "An action this browser does not know: " + action);
	record(QString("you asked for \"%1\", which is not one of the actions")
	           .arg(action));
	if (m_bad_replies >= 2) {
		finish(false, QStringLiteral("the model stopped following the protocol"));
		return;
	}
	next_turn();
}

void investigation::finish(bool solved, const QString &summary) {
	if (!m_running)
		return;
	m_running = false;
	emit logged("done", QString("%1: %2").arg(solved ? "Solved" : "Not solved",
	                                          summary));
	emit finished(solved, summary);
}
