#pragma once

#include <QString>

struct Node;

// Reads and writes the canonical, human-readable tree file — the source of
// truth for structure and order (architecture doc §4.4). Format, two spaces
// per depth level:
//
//   - [f0] folder | Work
//     - [a2] open | Jira board | https://jira.example.com/board | created=... | seen=...
//     - [a4] unopened | Recipe | https://cook.example.com/stew
//
// Fields are separated by " | ". The first field is the node type; trailing
// key=value fields (created=, seen=) are optional.
namespace TreeOutline {

// Returns a synthetic root Node (owns the whole tree; delete it to free).
// On failure or empty file, returns an empty root.
Node* load(const QString& path);

bool  save(const QString& path, Node* root);

}  // namespace TreeOutline
