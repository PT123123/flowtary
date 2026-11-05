#pragma once

#include "df_search.h"
#include <string>

namespace df {
// Persistent, read-only snapshot query child. Returns -1 when the index or
// transport is unavailable; it never falls back to directory enumeration.
bool FlowtaryIndexAvailable();
int FlowtaryIndexQuery(const Query& query, bool pathMode, std::vector<Hit>& out);
// Starts and waits for the read-only snapshot child without issuing a search.
// Call from a background worker so first user queries can stay on the warm path.
bool FlowtaryIndexPrewarm();
bool ParseFlowtaryIndexReply(const std::string& json, std::vector<Hit>& out);
#if defined(FLOWTARY_TEST_SNAPSHOT_OVERRIDE)
unsigned long long FlowtaryIndexTestChildCpu100ns();
unsigned long FlowtaryIndexTestChildPid();
#endif
} // namespace df
