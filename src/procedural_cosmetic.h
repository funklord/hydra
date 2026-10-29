#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// **Cosmetic rules that select by more than CSS can say** -- uBlock's
// procedural operators: `example.com##div.post:has-text(Sponsored)`,
// `:upward(2)`, `:remove()` and the rest. 1,649 lines of the holder's lists
// on 2026-10-29, every one scoped to a site, and none applied: the reader
// refused them, because a stylesheet cannot ask what an element says.
//
// A rule is read here into steps the page's cosmetic script runs
// (`cosmetic_filters::script_source`): a CSS selector, then operators that
// filter or move the set -- `has-text`, `upward`, `has` and `not` holding
// steps of their own, `matches-css`, `matches-attr`, `matches-path`,
// `min-text-length`, `xpath` -- and an action for what is found: hidden, or
// removed, or a named attribute or class taken off. Pure, so a rule's
// reading is tested without a page.
//
// **Not read: `:style()`** (708 lines), which sets declarations of the
// list's choosing rather than hiding -- the CSS this tree refuses from
// cosmetic rules on purpose (`filter_list::why_selector_unsafe`) -- and
// `:others()`, which hides everything else on the page. Both are refused
// with their reason, not dropped.
namespace procedural_cosmetic {

struct rule {
	QJsonArray steps;    // {"css": ...} or {"op": ..., "arg": ...} / {"op", "steps"}
	QString    action;   // "hide", "remove", "remove-attr", "remove-class"
	QString    action_arg;
};

// One rule as a subscribed list scoped it, read: what the page script is
// handed for a host the scope matches (`generic_cosmetics::procedural_for`).
// `selector` is kept as written, because that is what a `site#@#selector`
// exception names.
struct scoped_rule {
	QString     scope;
	QString     selector;
	QJsonObject rule;
};

// Whether `selector` uses an operator this reads at all. A selector without
// one is plain CSS, the ordinary cosmetic rule's business.
bool is_procedural(const QString &selector);

// Reads one selector. False with `why` when it uses something refused or
// cannot be read.
bool parse(const QString &selector, rule *out, QString *why);

// The form the page script reads: {"steps": [...], "action": ..., "arg": ...}.
QJsonObject to_json(const rule &r);

}  // namespace procedural_cosmetic
