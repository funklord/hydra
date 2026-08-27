// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy.h"

#include <QStringList>

namespace policy {

namespace {

struct info { const char *name; const char *label; const char *help; };

const info k_info[] = {
	{ "javascript", "JavaScript",
		"Scripts a site runs. Most of the modern web stops working without it, "
		"which is exactly why it is worth turning off for a few sites." },
	{ "cookies", "Cookies",
		"What a site may store to recognise you when you come back." },
	{ "thirdPartyCookies", "Third-party cookies",
		"Cookies set by other sites embedded in this one — the ordinary way "
		"tracking follows you from one site to the next." },
	{ "ads", "Ads / trackers",
		"Requests to known ad and tracking hosts, plus any filter rules you have "
		"accepted. Allowing them here also turns off those rules for the site." },
	{ "popups", "Popups",
		"Windows a page opens on its own, rather than because you clicked." },
	{ "images", "Images",
		"Pictures. Blocking them is faster and leaves some pages unreadable." },
	{ "autoplay", "Autoplay media",
		"Video and audio that starts playing without being asked." },
	{ "geolocation", "Location",
		"Where you are, when a page asks for it." },
	{ "camera", "Camera",
		"Seeing through the camera, when a page asks." },
	{ "microphone", "Microphone",
		"Listening through the microphone, when a page asks." },
	{ "notifications", "Notifications",
		"Messages a site can put on your desktop, including after you have left "
		"it." },
	{ "referer", "Referer header",
		"Telling a site which page you arrived from." },
	{ "autofill", "Password autofill",
		"Filling saved logins from KeePassXC. Limited to HTTPS pages unless that "
		"requirement is turned off, because filling a password over plain HTTP "
		"puts it on the wire." },
	{ "extractorFetch", "Extractor may fetch",
		"Lets a learned extractor fetch a manifest the page had already asked "
		"for, which is what streams hidden behind one need." },
	{ "cookieNotices", "Cookie consent banners",
		"The \"do you want to accept cookies?\" banner itself. Blocking it means "
		"Hydra answers it for you, taking the least permissive option the site "
		"actually offers." },
	{ "clipboardRead", "Clipboard reading",
		"Whether a site may read what you have copied. Writing to the "
		"clipboard is not this: a page can always put something there, and "
		"only reading tells it what you had." },
	{ "pointerLock", "Pointer lock",
		"Whether a site may capture the mouse pointer, which is what a game "
		"or a map view does to look around. Escape gives it back." },
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
		case setting::unset: break;
	}
	return "default";
}

setting setting_from_word(const QString &word) {
	const QString w = word.trimmed();
	if (w.compare("allow", Qt::CaseInsensitive) == 0) return setting::allow;
	if (w.compare("block", Qt::CaseInsensitive) == 0) return setting::block;
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
