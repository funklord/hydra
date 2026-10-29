#pragma once

#include "reader_extract.h"

#include <QString>
#include <QUrl>

// **The document reader mode shows**: the article `reader_extract` found, in
// one column of readable type, with the page's chrome, ads and scripts gone.
//
// It is shown in the tab itself with the page's address as its base, so the
// article's relative links and pictures resolve, Back returns to the page,
// and the tree keeps the page's address. That puts it in the page's origin,
// which is why it carries its own Content-Security-Policy forbidding every
// script: the markup is rebuilt from an allow-list already, and the policy
// is what makes a mistake in that list harmless rather than a hole.
//
// Pure, so the document is tested without a view.
namespace reader_page {

// `dark` follows the browser's own scheme, so the page matches the window
// around it. Text size is the tab's zoom, which the View menu already sets.
QString build(const reader_extract::article &a, const QUrl &page, bool dark);

}  // namespace reader_page
