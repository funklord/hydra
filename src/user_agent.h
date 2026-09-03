#pragma once

#include <QRegularExpression>
#include <QString>

// What this browser says it is.
//
// **Qt's default gets it turned away.** The string announces the Chromium it
// embeds and a token naming itself:
//
//     ... QtWebEngine/6.8.2 Chrome/122.0.6261.171 Safari/537.36
//
// Chrome 122 shipped in February 2024. A site that gates on a version reads
// that as a browser years out of date and refuses -- which is what a Swedish
// bank did, with "we no longer support the version of Google Chrome you are
// using". The `QtWebEngine/6.8.2` token is the other half: no real browser
// sends it, so a checker working from a list of known browsers has an unknown
// one in front of it.
//
// **Derived from Qt's own string rather than written out**, because the
// platform part -- `X11; Linux x86_64` here, something else elsewhere -- has
// to stay true, and a hardcoded user agent freezes it and is wrong on the next
// platform this builds for.
//
// **A claim about a version we do not have, and that is the trade.** The
// engine really is Chromium 122. A site relying on something only 140 has will
// now fail later rather than turning us away at the door, which is the worse
// failure in general and the better one here, because being refused up front
// is unconditional and cannot be worked around from inside the page.
//
// **What it does not fix, measured rather than assumed:** `sec-ch-ua` still
// reports `"Chromium";v="122"`. Chromium builds client hints from its real
// version and Qt exposes no override, so a site reading `navigator.
// userAgentData` instead of the string sees through this. A known limit.
namespace user_agent {

// The Chrome major to claim. **It has to be maintained**: nothing here can
// work out what today's Chrome is, and a number left alone for two years
// recreates the bug this exists to fix.
constexpr int k_claimed_chrome = 140;

// Qt's string in, the one to send out. Pure, so it can be tested without an
// engine -- the edge cases are what make it worth testing at all.
inline QString corrected(const QString &qt_default,
                          int claimed = k_claimed_chrome) {
	QString ua = qt_default;
	// The self-naming token, with its trailing space so nothing is left
	// doubled. Absent on a string that has already been through here, which is
	// why this is a remove rather than a required match.
	ua.remove(QRegularExpression(QStringLiteral("QtWebEngine/[0-9.]+ ")));

	// **Android's System WebView names itself three times, and every one of
	// them gets a browser turned away.** Its default string is shaped like
	//
	//     Mozilla/5.0 (Linux; Android 15; SM-F926B Build/AP3A...; wv)
	//     AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0
	//     Chrome/131.0.6778.200 Mobile Safari/537.36
	//
	// `wv` is the documented marker for "this is a WebView, not a browser", and
	// `Version/4.0` is the other half of it -- a frozen number left over from
	// the stock browser, which no Chrome has sent since. A site working from a
	// list of known browsers has an unknown one in front of it, exactly as the
	// `QtWebEngine` token did on the desktop, and Teams answers "your browser
	// isn't supported".
	//
	// `Build/...` goes with them. Real Chrome on Android does not send the
	// device's build fingerprint, so keeping it would both mark the string as
	// not-Chrome and hand every site a needlessly precise device identifier.
	//
	// All three are removals, so a desktop string passes through untouched and
	// a string that has already been here is unchanged -- the same property the
	// token removal above relies on.
	ua.remove(QRegularExpression(QStringLiteral(" Build/[^;)]+")));
	ua.remove(QStringLiteral("; wv"));
	ua.remove(QRegularExpression(QStringLiteral("Version/[0-9.]+ ")));

	// **The Android version and the model are frozen, because that is what
	// Chrome sends.**
	//
	// This used to keep them, on the stated grounds that they are true and
	// Chrome sends them. The second half was wrong, and measuring it was one
	// request: Chrome on the handset this was written for -- an Android 15
	// SM-F926B -- announces
	//
	//     Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 ...
	//
	// Android 10 and a model of "K" are placeholders. Chrome froze both in its
	// user-agent reduction, for the same reason it froze the version tail: the
	// pair identifies a handset far more precisely than any site needs, and a
	// foldable model number is close to a name. Sending the real ones made this
	// browser the only thing on the phone announcing them.
	//
	// So it is a fingerprint removed and a difference from Chrome closed at the
	// same time, which is the whole of what this function is for. Idempotent
	// like the removals above -- run over a string that already says
	// "Android 10; K" it matches and rewrites it to itself -- and scoped to
	// strings that carry an Android platform, so a desktop one passes through.
	ua.replace(QRegularExpression(QStringLiteral("Android [0-9.]+; [^)]+")),
	            QStringLiteral("Android 10; K"));

	// **The whole version, not just the major.** Replacing only the major left
	// `Chrome/140.0.6261.171` -- 140 wearing Chromium 122's build numbers, a
	// combination that has never shipped and is a worse fingerprint than the
	// honest string. Real Chrome has sent a *reduced* user agent since version
	// 101, freezing everything after the major to zero, so `Chrome/140.0.0.0`
	// is what a current browser actually says.
	ua.replace(QRegularExpression(QStringLiteral("Chrome/[0-9.]+")),
	            QStringLiteral("Chrome/%1.0.0.0").arg(claimed));
	return ua;
}


// The same browser, asking to be treated as a desktop one.
//
// **Because some sites will not serve a phone at all, whatever it looks like.**
// `teams.microsoft.com` redirects a mobile user agent to
// `/v2/unsupported-browser#isMobile=true` -- server-side, keyed on the `Mobile`
// token, and Chrome on Android gets the identical page. Looking more like Chrome
// cannot help, because looking exactly like Chrome on Android is what earns the
// redirect. The only thing that opens the site is asking as a desktop.
//
// Every mobile browser has this and calls it "request desktop site". It is a
// deliberate lie rather than a correction -- the two rules below say the machine
// is an X11 Linux desktop and drop the `Mobile` token -- which is why it is off
// by default and per tab, chosen each time by the person who wants that page.
//
// The platform string is the desktop build's own, so the two halves of this
// project claim the same thing.
inline QString desktop_form(const QString &mobile_ua) {
	QString ua = mobile_ua;
	// The platform, which is the whole of what a site keys on here.
	//
	// **By index, not by regular expression, and the first draft got this
	// wrong.** A user agent has two parenthesised groups -- the platform and
	// `(KHTML, like Gecko)` -- and a pattern replace rewrites both, producing
	// `AppleWebKit/537.36 (X11; Linux x86_64)` in the middle of the string.
	// Caught by running the transformation over a real string before building
	// it, which is the cheapest place to catch anything.
	const int open  = ua.indexOf(QLatin1Char('('));
	const int close = open >= 0 ? ua.indexOf(QLatin1Char(')'), open) : -1;
	if (open >= 0 && close > open)
		ua.replace(open, close - open + 1, QStringLiteral("(X11; Linux x86_64)"));
	// `Mobile ` sits directly before `Safari/537.36` and is the other token a
	// server reads. Removed with its trailing space so nothing is left doubled.
	ua.remove(QStringLiteral("Mobile "));
	return ua;
}

// **The other channel, which the string above does not reach.**
//
// Chromium builds `sec-ch-ua` and `navigator.userAgentData` from its own build
// identity, and neither Qt nor Android's WebView exposes an override. On the
// desktop that costs a version number. On Android it costs the whole disguise:
// the brand list says
//
//     "Chromium";v="152", "Not?A_Brand";v="24", "Android WebView";v="152"
//
// -- measured on the handset, with the corrected string already in place. A site
// reading the brands rather than the string sees `Android WebView` stated
// outright, and `teams.microsoft.com` answers "your browser version isn't
// supported" to a string that says Chrome 140.
//
// This shim rewrites the JavaScript half in the page's own world, turning the
// `Android WebView` brand into `Google Chrome` -- which, with `Chromium` and the
// deliberately-nonsensical `Not?A_Brand` already present, is exactly the triple
// real Chrome on Android sends.
//
// **What it cannot do is the header.** `sec-ch-ua` is written by the network
// stack before any script runs, so a site checking server-side still sees the
// WebView. It is a fix for client-side sniffing only, and which of the two a
// given site does is answered by trying it rather than by assuming.
//
// Written as getters over the real object rather than a flat copy, so anything
// not being corrected -- `mobile`, `platform`, fields added later -- keeps
// coming from the platform and stays true.
inline QString client_hints_shim() {
	return QStringLiteral(R"JS(
(function () {
  var real = navigator.userAgentData;
  if (!real || !real.brands) return;
  function fix(list) {
    return (list || []).map(function (b) {
      return b.brand === 'Android WebView'
        ? { brand: 'Google Chrome', version: b.version }
        : { brand: b.brand, version: b.version };
    });
  }
  var shim = {
    get brands()   { return fix(real.brands); },
    get mobile()   { return real.mobile; },
    get platform() { return real.platform; },
    toJSON: function () {
      return { brands: fix(real.brands), mobile: real.mobile,
                platform: real.platform };
    },
    getHighEntropyValues: function (hints) {
      return real.getHighEntropyValues(hints).then(function (v) {
        if (v && v.brands) v.brands = fix(v.brands);
        if (v && v.fullVersionList) v.fullVersionList = fix(v.fullVersionList);
        return v;
      });
    }
  };
  try {
    Object.defineProperty(navigator, 'userAgentData', {
      configurable: true, get: function () { return shim; }
    });
  } catch (e) { /* a page that froze navigator keeps what it has */ }
})();
)JS");
}

}  // namespace user_agent
