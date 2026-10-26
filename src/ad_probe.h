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

// What one frame found, added to `into`. A finding from a frame other than
// the top document's says so, naming the frame's host, so the model is told
// where the ad is and not only that the top document holds a frame.
void add(findings *into, const findings &frame, const QString &frame_host,
         bool main_frame);

// One line per finding, most decisive first: a player that says it is in an
// ad is worth more than a div with "ad" in its class.
QStringList describe(const findings &f);

// **What the page can see of this browser**, asked in the page's own world,
// where the patches are -- the isolated world above has its own untouched
// `fetch` and would see nothing. Read-only and asked once, when the button
// is pressed. Each line is a tell a page could check: a patched function
// that hands back its source, a global or element named for this browser,
// and on YouTube the "Ad blockers are not allowed" wall and the request
// identity the player is sending. Empty when there is nothing to say.
QString tells_source();
QStringList parse_tells(const QString &json);

}  // namespace ad_probe
