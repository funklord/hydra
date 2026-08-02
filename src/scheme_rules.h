// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QUrl;

// Which urls a web engine is expected to render itself.
//
// Everything else — `magnet:`, `mailto:`, `intent:`, whatever a page invents —
// is somebody else's job, and the shell decides whose (§11.4). Both engines need
// this same answer and reach it by different routes: Qt WebEngine hands custom
// schemes to a registered scheme handler, while Android's WebView asks
// `shouldOverrideUrlLoading` about *every* navigation and takes silence as
// consent to try loading it itself.
//
// Shared rather than written twice, because a scheme on one list and not the
// other is a link that works on the desktop and dead-ends on a phone — and that
// is exactly the kind of difference nobody goes looking for.
bool renders_as_page(const QUrl &url);
