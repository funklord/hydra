#pragma once

#include <QString>
#include <QtGlobal>

// Per-site security features and the tri-state setting model (architecture
// doc sec 7.1). Each rule packs one 2-bit setting per feature into a quint64.
namespace policy {

enum class feature : int {
	javascript = 0,
	cookies,
	third_party_cookies,
	ads,
	popups,
	images,
	autoplay,
	geolocation,
	camera,
	microphone,
	notifications,
	referer,
	autofill,
	// The sec 11.5.1 helper tier. Two powers rather than one, deliberately:
	// reading a manifest the page already fetched is not comparable to reading
	// the DOM of a logged-in page, and a single "advanced extractor" switch
	// would quietly grant the second to get the first. Defaults to block.
	//
	// **`extractor_dom` is deliberately absent, and its design is not.** The DOM
	// half of sec 11.5.1 is designed and unbuilt, so the permission was offered and
	// read by nothing: denying it gave false assurance and granting it did
	// nothing, for a control whose own description says it grants access to
	// "whatever you are logged in to". A permission is a promise about what the
	// program will do, and one for a capability that does not exist is a promise
	// nobody is keeping. It comes back with the capability, under this name, in
	// this position -- rules persist by name, so nothing on disk depends on
	// where it sits.
	extractor_fetch,
	// "Do you want to accept cookies?" -- the consent banner itself, not the
	// cookies. Blocked means the banner is answered and dismissed for you;
	// allowed means it is left alone and you answer it yourself. It sits with
	// the other blocking options rather than under `cookies` because it is a
	// different question: what the *page* may put in front of you, not what it
	// may store.
	cookie_notices,
	// **Two capabilities the engine has always asked about and this never
	// answered.** Chromium requests both, `qtwebengine_view` had no
	// `policy::feature` to map them onto, and so both were denied without the
	// shield ever being consulted -- a decision made by a `default:` arm.
	//
	// They are here rather than left denied because the capability is real,
	// which is the distinction `extractor_dom` above turns on: that one is
	// absent because the *power does not exist yet*, so a permission for it
	// would be a promise nobody keeps. These exist, the engine asks, and the
	// only thing missing was somewhere to record the answer.
	//
	// Both default to block, which is exactly what happened before, so nothing
	// changes for anyone who does not go looking. What changes is that saying
	// yes becomes possible: pointer lock is asked for on entering a game or a
	// map, and a browser that can only ever refuse it is not offering a
	// setting, it is stating a limitation.
	clipboard_read,
	pointer_lock,
	// **The per-site switch the architecture doc puts here in as many words**
	// -- "a per-site 'auto-detect media' toggle lives in the PolicyEngine"
	// (sec 11) -- and which nothing had. The detector rides the same request
	// stream the blocker does and recorded every saveable resource on every
	// site, with no way to tell it not to. Allowed by default, because
	// noticing media is most of what the badge is for; a site turned off stops
	// being watched rather than being watched silently.
	media_detect,
	count
};

// `unset` is the architecture doc's "Default" state -- no rule expressed at
// this scope, so resolution falls through to the global default. Spelled
// `unset` because `default` is a keyword.
enum class setting : quint8 { unset = 0, allow = 1, block = 2 };

inline int feature_count() { return static_cast<int>(feature::count); }

// Stable machine name (JSON keys).
const char *feature_name(feature f);
// Human label (UI).
const char *feature_label(feature f);
// One line saying what the setting governs, shown under the label the way a
// browser's settings page does. It describes the *power*, not the state, so it
// reads correctly whichever way the setting is set -- "where you are, when a page
// asks" is true whether that is allowed or blocked, while "sites cannot see
// where you are" would be a lie half the time.
const char *feature_help(feature f);
// Parse a machine name; returns feature::count on failure.
feature     feature_from_name(const QString &name);

// --- one line of settings, as the INI files write it ------------------------
//
// "javascript:block, cookies:allow" -- the encoding shared by the policy file
// and the exported settings bundle. Shared because two encoders for one line
// drift, and the drift is invisible until a file written by one is read by the
// other.
QString  settings_to_line(quint64 bits);
quint64  settings_from_line(const QString &line);
const char *setting_word(setting s);
setting     setting_from_word(const QString &word);

// --- 2-bit packing into a quint64 -----------------------------------------
inline setting get_setting(quint64 bits, feature f) {
	const int shift = 2 * static_cast<int>(f);
	return static_cast<setting>((bits >> shift) & 0x3ULL);
}

inline quint64 with_setting(quint64 bits, feature f, setting s) {
	const int shift = 2 * static_cast<int>(f);
	bits &= ~(0x3ULL << shift);
	bits |= (static_cast<quint64>(s) & 0x3ULL) << shift;
	return bits;
}

}  // namespace policy
