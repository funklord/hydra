// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

struct node;

// Turns the tree into the payload an AI provider sees, and turns a proposal
// back into a tree (architecture doc sec 9.2/sec 9.3).
//
// This is deliberately NOT the canonical outline. sec 9.3 fixes exactly what may
// leave the machine -- per node: id, parent/depth, title, URL, type, tags -- so
// this format carries those and nothing else. In particular it drops the
// `created=` / `seen=` timestamps the on-disk file keeps, because they are not
// on that list; browsing history is precisely the sort of thing that should not
// travel just because it happened to be in the same file. State blobs and live
// views never come near this: they are keyed by id and stay local, which is
// what makes reorganization a pure re-parenting of ids.
namespace tree_serializer {

// The payload. Two spaces per depth level, same shape as the canonical file:
//
//   - [f0] folder | Work
//     - [a1] open | Qt Documentation | https://doc.qt.io | tags=qt,docs
QString to_payload(node *root);

// Parse a proposal back. Returns a synthetic root owning the tree (delete to
// free), or nullptr if the text contains no usable node lines at all -- an
// empty proposal is a provider failure, not a request to delete everything.
node *parse_proposal(const QString &text);

}  // namespace tree_serializer
