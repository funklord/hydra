#pragma once

#include <QString>
#include <QUrl>

// **The warning shown in place of a page a list says not to visit**
// (`request_filter::page_block`): what was refused, the rule that refused
// it, and a way through for somebody who knows better. uBlock calls this
// strict blocking and shows the same three things.
//
// Shown in the tab with the refused address as its base, so the address bar
// says what was refused and Back leaves. That puts it in the page's origin,
// so, like the reader's document, it forbids every script by its own
// Content-Security-Policy; it needs none, being a link and some text.
//
// The way through is a link to the same address carrying a token the shell
// made for this one warning (`main_window::allow_navigation` reads it back).
// A page cannot forge it, so no other site can link past the warning.
//
// Pure, so the document is tested without a view.
namespace blocked_page {

// The query parameter the way through carries.
inline QString token_key() { return QStringLiteral("hydra-proceed"); }

QString build(const QUrl &page, const QString &rule, const QString &token, bool dark);

}  // namespace blocked_page
