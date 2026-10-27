#include "snapshot_parser.h"

// STUB (AUDIT.md P2). Deliberately does nothing so test_snapshot_parser fails
// for the right reason: malformed packets expect an error and get OK. Replace
// with the real parser, moved out of run_listener().
SnapshotParseResult parse_snapshot(const char*, size_t, std::vector<Snapshot>&) {
    return SnapshotParseResult::OK;
}
