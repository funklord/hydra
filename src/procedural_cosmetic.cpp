#include "procedural_cosmetic.h"

#include "filter_list.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace procedural_cosmetic {

namespace {

// The operators read, and the ones refused by name.
const QStringList k_filters = {
	"has-text", "upward", "has", "not", "matches-css", "matches-css-before",
	"matches-css-after", "matches-attr", "matches-path", "min-text-length", "xpath",
	"watch-attr"
};
const QStringList k_actions = { "remove", "remove-attr", "remove-class" };
const QStringList k_refused = { "style", "others", "matches-media", "matches-prop",
	                              "spath" };

const int k_max_arg = 400;

// The operator name at `at` (just past a ':'), or empty.
QString op_at(const QString &s, int at) {
	int e = at;
	while (e < s.size() && (s.at(e).isLetter() || s.at(e) == QLatin1Char('-')))
		++e;
	if (e >= s.size() || s.at(e) != QLatin1Char('('))
		return QString();
	const QString name = s.mid(at, e - at);
	if (k_filters.contains(name) || k_actions.contains(name) || k_refused.contains(name))
		return name;
	return QString();
}

// The argument starting at `open` (the '('), to its matching ')': nesting,
// quotes and a backslash honoured. -1 if it never closes.
int close_of(const QString &s, int open) {
	int depth = 0;
	QChar quote;
	for (int i = open; i < s.size(); ++i) {
		const QChar c = s.at(i);
		if (c == QLatin1Char('\\')) { ++i; continue; }
		if (!quote.isNull()) {
			if (c == quote)
				quote = QChar();
			continue;
		}
		if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quote = c; continue; }
		if (c == QLatin1Char('(')) ++depth;
		else if (c == QLatin1Char(')') && --depth == 0)
			return i;
	}
	return -1;
}

bool read_steps(const QString &sel, QJsonArray *steps, QString *action, QString *action_arg,
                QString *why, int depth) {
	if (depth > 4) {
		if (why) *why = QStringLiteral("operators nested too deep");
		return false;
	}
	QString css;
	auto flush = [&]() -> bool {
		const QString c = css.trimmed();
		css.clear();
		if (c.isEmpty())
			return true;
		const QString unsafe = filter_list::why_selector_unsafe(c);
		if (!unsafe.isEmpty()) {
			if (why) *why = unsafe;
			return false;
		}
		steps->append(QJsonObject{ { "css", c } });
		return true;
	};
	for (int i = 0; i < sel.size(); ++i) {
		const QChar c = sel.at(i);
		if (c != QLatin1Char(':')) {
			css += c;
			continue;
		}
		const QString name = op_at(sel, i + 1);
		if (name.isEmpty()) {
			css += c;
			continue;
		}
		const int open = i + 1 + name.size();
		const int close = close_of(sel, open);
		if (close < 0) {
			if (why) *why = QString("an unclosed :%1(").arg(name);
			return false;
		}
		const QString arg = sel.mid(open + 1, close - open - 1).trimmed();
		if (arg.size() > k_max_arg) {
			if (why) *why = QStringLiteral("an operator argument too long to review");
			return false;
		}
		if (k_refused.contains(name)) {
			if (why) *why = QString(":%1() is not read here").arg(name);
			return false;
		}
		// **A path gate**: `:matches-path()` opening a rule, or `:not()` of
		// one, decides whether the rule runs on this page at all, and the CSS
		// after it selects from the document rather than from a set.
		if (steps->isEmpty() && depth == 0 && css.trimmed().isEmpty()) {
			static const QRegularExpression gate(
			  QStringLiteral("^:matches-path\\((.*)\\)$"));
			const QRegularExpressionMatch m =
			  name == QLatin1String("not") ? gate.match(arg) : QRegularExpressionMatch();
			if (name == QLatin1String("matches-path") || m.hasMatch()) {
				const bool negate = m.hasMatch();
				steps->append(QJsonObject{ { "op", "path" },
				                           { "arg", negate ? m.captured(1).trimmed() : arg },
				                           { "negate", negate } });
				i = close;
				continue;
			}
		}
		// `:has()` and `:not()` of plain CSS are CSS, which the engine
		// evaluates itself; they are procedural only when what they hold is.
		if ((name == QLatin1String("has") || name == QLatin1String("not")) && !is_procedural(arg)) {
			css += sel.mid(i, close - i + 1);
			i = close;
			continue;
		}
		if (!flush())
			return false;
		// `:xpath()` alone is a whole rule: it selects from the document, as
		// does anything after a path gate.
		const bool gated = steps->size() == 1 &&
		                   steps->at(0).toObject().value("op") == QLatin1String("path");
		if ((steps->isEmpty() || gated) && depth == 0 && name != QLatin1String("xpath")) {
			if (why) *why = QStringLiteral("an operator with no selector before it");
			return false;
		}
		if (k_actions.contains(name)) {
			// An action ends the rule: what follows it would act on nothing.
			if (!sel.mid(close + 1).trimmed().isEmpty() || depth > 0) {
				if (why) *why = QString(":%1() before the end of the rule").arg(name);
				return false;
			}
			*action = name;
			*action_arg = arg;
			return true;
		}
		if (name == QLatin1String("has") || name == QLatin1String("not")) {
			QJsonArray inner;
			QString inner_action, inner_arg;
			if (!read_steps(arg, &inner, &inner_action, &inner_arg, why, depth + 1))
				return false;
			steps->append(QJsonObject{ { "op", name }, { "steps", inner } });
		} else if (name == QLatin1String("xpath") && arg.size() >= 2 &&
		           (arg.front() == QLatin1Char('"') || arg.front() == QLatin1Char('\'')) &&
		           arg.back() == arg.front()) {
			// Quoted, as some lists write it: the expression is inside.
			steps->append(QJsonObject{ { "op", name }, { "arg", arg.mid(1, arg.size() - 2) } });
		} else if (name != QLatin1String("watch-attr")) {
			steps->append(QJsonObject{ { "op", name }, { "arg", arg } });
		}
		i = close;
	}
	return flush();
}

}  // namespace

bool is_procedural(const QString &selector) {
	for (int i = 0; i < selector.size(); ++i) {
		if (selector.at(i) != QLatin1Char(':'))
			continue;
		const QString name = op_at(selector, i + 1);
		if (name.isEmpty())
			continue;
		if (name == QLatin1String("has") || name == QLatin1String("not")) {
			const int close = close_of(selector, i + 1 + name.size());
			if (close > 0 && is_procedural(selector.mid(i + name.size() + 2,
			                                             close - i - name.size() - 2)))
				return true;
			continue;
		}
		return true;
	}
	return false;
}

bool parse(const QString &selector, rule *out, QString *why) {
	rule r;
	r.action = QStringLiteral("hide");
	if (!read_steps(selector.trimmed(), &r.steps, &r.action, &r.action_arg, why, 0))
		return false;
	if (r.steps.isEmpty()) {
		if (why) *why = QStringLiteral("nothing to select");
		return false;
	}
	if (out)
		*out = r;
	return true;
}

QJsonObject to_json(const rule &r) {
	return QJsonObject{ { "steps", r.steps }, { "action", r.action }, { "arg", r.action_arg } };
}

}  // namespace procedural_cosmetic
