// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QString;
class QUrl;

// What the address bar does with what somebody typed.
//
// Until this existed the answer was "assume it is an address", so anything
// that was not one went to `QUrl::fromUserInput` and came back either invalid
// -- silently doing nothing -- or as a guess like `http://weather tomorrow`.
// A browser whose address bar only accepts addresses is missing the half of it
// people use most.

// Whether typed text is meant as an address rather than as terms to search
// for.
//
// **The interesting direction is the one that leaks.** Guessing "search" for
// something that was an address is a wasted keystroke; guessing "search" for
// an *intranet* address sends a private hostname to a third party, and that
// cannot be taken back. So every rule below is written to keep a plausible
// host out of the search box, and the residual ambiguity is spent the other
// way.
//
// An address is any of:
//
//   * text carrying a scheme -- `https:`, `file:`, `magnet:`, `about:`.
//     Whitespace does not override this: somebody who typed a scheme meant an
//     address, and the engine can encode the rest.
//   * a path, by its first character: `/`, `./`, `../` or `~/`.
//   * an IPv4 or IPv6 literal, with or without a port.
//   * `localhost`, with or without a port.
//   * any single token carrying a dot whose last label looks like a
//     top-level domain -- two or more characters, all letters.
//
// Everything else is search. That last rule is what keeps `3.14` and
// `1.5x faster` out of the address bar: a dot alone is not a hostname, and a
// final label of digits is not a TLD. It is also what keeps
// `internal.corp.example/secret` out of a search engine, which is the case
// worth being careful about.
//
// A bare word with no dot -- `wiki`, `router` -- is searched, which is what
// every browser does and is the one leak this accepts: the word itself goes
// out, and it is a word rather than a path. `localhost` is named explicitly
// because it is the one such host that is always real.
bool looks_like_address(const QString &text);

// The search url for `terms` under `tmpl`, where `tmpl` carries a single `%1`
// standing for the encoded terms. Returns an invalid QUrl if the template has
// no `%1`, so a mistyped setting fails visibly rather than searching for
// nothing.
QUrl search_url(const QString &terms, const QString &tmpl);
