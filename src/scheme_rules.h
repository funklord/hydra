#pragma once

class QUrl;

// Which urls a web engine is expected to render itself.
//
// Everything else -- `magnet:`, `mailto:`, `intent:`, whatever a page invents --
// is somebody else's job, and the shell decides whose (sec 11.4). Both engines need
// this same answer and reach it by different routes: Qt WebEngine hands custom
// schemes to a registered scheme handler, while Android's WebView asks
// `shouldOverrideUrlLoading` about *every* navigation and takes silence as
// consent to try loading it itself.
//
// Shared rather than written twice, because a scheme on one list and not the
// other is a link that works on the desktop and dead-ends on a phone -- and that
// is exactly the kind of difference nobody goes looking for.
bool renders_as_page(const QUrl &url);

// Whether "show me the markup" means anything for this address.
//
// **Narrower than `renders_as_page`, and the exclusions are the rule rather
// than caution.** `view-source:` of a `view-source:` is handled inconsistently
// between engines and says nothing to a reader either way. `about:` and
// `chrome:` pages have no served source -- what is on screen was assembled by
// the engine, so there is no fetched document to show. `data:` and `blob:`
// already carry their content in the address or in memory, so a source view
// would either repeat the address bar or find nothing to fetch.
//
// What is left is what came over a wire or off a disk, which is the only case
// where the markup and the rendering can differ -- and that difference is the
// entire reason anybody opens this.
//
// Here beside `renders_as_page` for the reason that one is shared: both
// backends need the same answer, and a scheme treated one way on the desktop
// and another on a phone is the kind of difference nobody goes looking for.
bool has_viewable_source(const QUrl &url);
