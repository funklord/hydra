#pragma once

#include <QString>
#include <QStringList>

// **Whether the page in front of somebody is showing an ad right now**, asked
// once, when they press the Annoyed button.
//
// Everything else that button gathers is about what the browser did: requests
// that got through, capabilities it was asked for, page patches it ran. None
// of it says whether an ad is on screen, and for the commonest complaint -- a
// video pre-roll from the site's own host -- all three can be clean while the
// ad plays. This looks at the page itself.
//
// It runs in an isolated world, so the page cannot see it or answer it, and
// reads only the DOM: what players say about themselves in their own markup,
// visible elements named as ads, frames from ad-serving hosts, and visible
// text that labels an ad. Each finding is something a person can check and a
// model can write a rule from.
namespace ad_probe {

struct findings {
	QStringList players;    // a player in an ad state, and how it says so
	QStringList elements;   // visible ad-named elements, with a selector
	QStringList frames;     // visible frames from an ad-serving host
	QStringList labels;     // visible text that labels an ad
	bool empty() const {
		return players.isEmpty() && elements.isEmpty() && frames.isEmpty() &&
		       labels.isEmpty();
	}
};

// The script. ES5, returns its findings as JSON text, and bounds every scan so
// a page with a hundred thousand nodes costs a capped amount.
QString source();

// Reads what the script returned. False for anything that is not its output,
// which is also what a backend that cannot run a probe hands back.
bool parse(const QString &json, findings *out);

// One line per finding, most decisive first: a player that says it is in an
// ad is worth more than a div with "ad" in its class.
QStringList describe(const findings &f);

}  // namespace ad_probe
