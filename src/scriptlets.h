#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Scriptlets: the `host##+js(name, args...)` rule kind.
//
// **The architecture doc does not describe this capability.** Section 12 is
// the filter-evolution loop and its five steps end at accepting a rule into
// the user's own list; subscribing to an upstream one is step 5's own
// sentence, and a scriptlet is neither -- it is a patch, not a rule. So this
// cites sec 12 rather than a subsection that does not exist, and whether
// the doc gains a step for it is the copyright holder's (see project.md).
//
// **Why this exists at all, stated plainly, because it is the most
// security-weighty thing in the filter path.** A network rule cannot touch an
// ad that is served from the same host as the content and stitched into the
// same stream, and a CSS rule cannot skip one. What every blocker actually
// uses there is a small JavaScript patch applied to the page's own globals
// before its scripts run -- pruning an ad array out of a player's
// configuration, pinning a flag the player reads. That is a scriptlet, and
// nothing else in this program can do it.
//
// **The catalog is ours and it is closed.** A rule names a scriptlet; it does
// not carry one. Only a name in this file runs, each implementation is written
// here and reviewable here, and a rule naming anything else is dropped and
// counted. So a subscribed list -- tens of thousands of lines nobody in this
// program reviewed one at a time -- can ask for a patch from a fixed set and
// cannot supply a new one. uBlock Origin's own names are used deliberately, so
// that a rule written for the ecosystem resolves here; the code behind each
// name is this project's.
//
// **Arguments are data, never source.** They arrive from a filter list, so the
// one mistake that would matter is interpolating one into the script text:
// `##+js(set-constant, x, 1); evil()` would then be a list rule with code in
// it. `source_for` emits the calls as a JSON array and the runner reads them
// with `JSON.parse`, so an argument is a string at every point and the worst a
// hostile one can do is name a property that does not exist.
//
// **No `trusted-*` scriptlets.** uBlock keeps a second class that only a list
// the user has explicitly trusted may call -- ones that can write cookies,
// replace fetch, run arbitrary expressions. None is implemented here, and the
// absence is deliberate rather than pending: the first of them belongs with
// whatever UI says which lists are trusted, and there is no such UI.
struct scriptlet_call {
	QString     scope;   // the site it applies on; never empty
	QString     name;    // a catalog name, already vetted
	QStringList args;
	// **Whether the list this came from is one the person marked trusted.**
	// Carried on the call rather than asked later, because by the time a
	// script is written the list it came from is out of reach -- and the
	// scriptlets that need it are the ones where getting it wrong matters
	// most. False is the only safe default, so it is the default.
	bool        trusted = false;
};

namespace scriptlets {

// Is this a name this build implements? The catalog is closed, so this is the
// whole of what a rule may ask for.
bool vetted(const QString &name);

// **Does this scriptlet need the list that asked for it to be trusted?**
//
// uBlock keeps a second class of scriptlet whose powers are not "stop the page
// doing something" but "do something on the page's behalf": write a cookie,
// replace a response body with content of the rule's choosing, set a global to
// an arbitrary string. A rule in that class is not a filter, it is a small
// program, and a subscribed list is tens of thousands of lines nobody here
// reviewed one at a time.
//
// So these run only for a list the person marked trusted, and `source_for`
// refuses them otherwise. The name carries the warning -- every one of them
// begins `trusted-` -- but the check is not on the prefix: it is on the
// catalog entry, so a name cannot acquire the power by being spelled like one.
bool requires_trust(const QString &name);

// Every implemented name, for the settings line and the tests. Sorted.
QStringList names();

// Parse the inside of `+js(...)`: a name and comma-separated arguments, with
// `\,` as an escaped comma. Returns false when the name is not vetted or the
// text is not a call, and fills `why` where it can say something useful.
bool parse_call(const QString &inside, scriptlet_call *out, QString *why);

// The script to inject into a page, for the calls that apply to it. Empty when
// there are none, so a caller can skip the injection entirely rather than
// installing a script that does nothing.
QString source_for(const QList<scriptlet_call> &calls);

// **What the injected script did in one frame**, as it reports it on the
// console: which scriptlets ran, which failed and why, and how many were not
// for this host. The browser keeps it with the site's other signals, so the
// person and the model can both see a rule that was in force and did nothing.
struct report {
	QString     host;      // the frame's own hostname
	QStringList ran;       // canonical names, in the order they ran
	QStringList failed;    // "name: reason"
	int         skipped = 0;
};

// The console line's prefix. A message without it is the page's own.
QString report_prefix();

// Reads one console line. False for anything that is not a report, so a
// caller can hand it every message and keep only these.
bool parse_report(const QString &line, report *out);

// One line for a person or a model: the counts, the names that ran grouped
// by how often, and every failure in full.
QString describe(const report &r);

}  // namespace scriptlets
