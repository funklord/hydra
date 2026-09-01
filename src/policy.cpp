#include "policy.h"

#include <QStringList>

namespace policy {

namespace {

// `ask_phrase` is what a permission prompt says the site wants to do, and its
// presence is also the answer to "can this feature be set to ask at all?".
//
// **One field doing both jobs, deliberately.** The alternative was a separate
// list of promptable features somewhere in the UI, which is a second copy of a
// fact -- and the failure mode of the copy going stale is a feature offered as
// `ask` that no prompt has words for, or words written for a feature the
// combo box will not let anybody select.
//
// Explicitly `nullptr` on the twelve settings that are decided in advance and
// never interrupt anybody. Leaving the member off worked -- C++ value-initialises
// it -- and cost twelve `-Wmissing-field-initializers` in a build that carries
// none, which is a warning worth having: it is the one that notices a field
// added to this struct and forgotten in every row below.
struct info {
	const char *name;
	const char *label;
	const char *help;
	const char *ask_phrase;
};

const info k_info[] = {
	{ "javascript", "JavaScript",
		"Scripts a site runs. Most of the modern web stops working without it, "
		"which is exactly why it is worth turning off for a few sites.",
		nullptr },
	{ "cookies", "Cookies",
		"What a site may store to recognise you when you come back.",
		nullptr },
	{ "thirdPartyCookies", "Third-party cookies",
		"Cookies set by other sites embedded in this one — the ordinary way "
		"tracking follows you from one site to the next.",
		nullptr },
	{ "ads", "Ads / trackers",
		"Requests to known ad and tracking hosts, plus any filter rules you have "
		"accepted. Allowing them here also turns off those rules for the site.",
		nullptr },
	{ "popups", "Popups",
		"Windows a page opens on its own, rather than because you clicked.",
		nullptr },
	{ "images", "Images",
		"Pictures. Blocking them is faster and leaves some pages unreadable.",
		nullptr },
	{ "autoplay", "Autoplay media",
		"Video and audio that starts playing without being asked.",
		nullptr },
	{ "geolocation", "Location",
		"Where you are, when a page asks for it.",
		"know where you are" },
	{ "camera", "Camera",
		"Seeing through the camera, when a page asks.",
		"use your camera" },
	{ "microphone", "Microphone",
		"Listening through the microphone, when a page asks.",
		"use your microphone" },
	{ "notifications", "Notifications",
		"Messages a site can put on your desktop, including after you have left "
		"it.",
		"send you notifications" },
	{ "referer", "Referer header",
		"Telling a site which page you arrived from.",
		nullptr },
	{ "autofill", "Password autofill",
		"Filling saved logins from KeePassXC. Limited to HTTPS pages unless that "
		"requirement is turned off, because filling a password over plain HTTP "
		"puts it on the wire.",
		nullptr },
	{ "extractorFetch", "Extractor may fetch",
		"Lets a learned extractor fetch a manifest the page had already asked "
		"for, which is what streams hidden behind one need.",
		nullptr },
	{ "cookieNotices", "Cookie consent banners",
		"The \"do you want to accept cookies?\" banner itself. Blocking it means "
		"Hydra answers it for you, taking the least permissive option the site "
		"actually offers.",
		nullptr },
	{ "clipboardRead", "Clipboard reading",
		"Whether a site may read what you have copied. Writing to the "
		"clipboard is not this: a page can always put something there, and "
		"only reading tells it what you had.",
		"read your clipboard" },
	{ "pointerLock", "Pointer lock",
		"Whether a site may capture the mouse pointer, which is what a game "
		"or a map view does to look around. Escape gives it back.",
		"take over your mouse pointer" },
	{ "autoDetectMedia", "Auto-detect media",
		"Whether to watch this site's requests for video and audio worth "
		"saving. Turning it off empties the media badge here; it does not "
		"stop the page playing anything.",
		nullptr },
};

}  // namespace

const char *feature_name(feature f) {
	const int i = static_cast<int>(f);
	if (i < 0 || i >= feature_count())
		return "";
	return k_info[i].name;
}

const char *feature_label(feature f) {
	const int i = static_cast<int>(f);
	if (i < 0 || i >= feature_count())
		return "";
	return k_info[i].label;
}

// Null where the feature is never prompted for; see the note on `info`.
const char *ask_phrase(feature f) {
	const int i = static_cast<int>(f);
	if (i < 0 || i >= feature_count())
		return nullptr;
	return k_info[i].ask_phrase;
}

bool can_ask(feature f) { return ask_phrase(f) != nullptr; }

const char *feature_help(feature f) {
	const int i = static_cast<int>(f);
	if (i < 0 || i >= feature_count())
		return "";
	return k_info[i].help;
}

const char *setting_word(setting s) {
	switch (s) {
		case setting::allow: return "allow";
		case setting::block: return "block";
		case setting::ask:   return "ask";
		case setting::unset: break;
	}
	return "default";
}

setting setting_from_word(const QString &word) {
	const QString w = word.trimmed();
	if (w.compare("allow", Qt::CaseInsensitive) == 0) return setting::allow;
	if (w.compare("block", Qt::CaseInsensitive) == 0) return setting::block;
	if (w.compare("ask",   Qt::CaseInsensitive) == 0) return setting::ask;
	return setting::unset;
}

QString settings_to_line(quint64 bits) {
	QStringList parts;
	for (int i = 0; i < feature_count(); ++i) {
		const auto f = static_cast<feature>(i);
		const setting s = get_setting(bits, f);
		if (s == setting::unset)
			continue;
		parts << QString("%1:%2").arg(feature_name(f), setting_word(s));
	}
	return parts.join(", ");
}

quint64 settings_from_line(const QString &line) {
	quint64 bits = 0;
	for (const QString &part : line.split(',', Qt::SkipEmptyParts)) {
		const QStringList kv = part.split(':');
		if (kv.size() != 2)
			continue;   // one unreadable field costs only itself
		const feature f = feature_from_name(kv[0].trimmed());
		const setting s = setting_from_word(kv[1]);
		if (f == feature::count || s == setting::unset)
			continue;
		bits = with_setting(bits, f, s);
	}
	return bits;
}

feature feature_from_name(const QString &name) {
	for (int i = 0; i < feature_count(); ++i)
		if (name == QLatin1String(k_info[i].name))
			return static_cast<feature>(i);
	return feature::count;
}

}  // namespace policy
