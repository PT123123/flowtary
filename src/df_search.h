#pragma once
#include <string>
#include <vector>

namespace df {
struct Hit {
    std::wstring path;
    bool isFolder = false;
    bool workshop = false;
};
struct Query {
    bool isFile = true;
    std::vector<std::wstring> words;
    std::vector<std::wstring> excludes;
};
// All APIs query the existing SystemIndex catalog. They never enumerate directories,
// create an index, change indexing scope, or start an external process.
bool WindowsSearchAvailable(); // A successful empty catalog is still available.
std::wstring DiagProbe();
int WindowsSearchQuery(const Query& q, bool pathMode, std::vector<Hit>& out);

// At most two catalog queries. A failed path query stays a failure; stale work stops.
template<class QueryCatalog, class IsCurrent>
int QueryWithPathFallback(const Query& q, std::vector<Hit>& out,
                          QueryCatalog queryCatalog, IsCurrent isCurrent) {
    out.clear();
    if (!isCurrent()) return 0;
    int result = queryCatalog(q, false, out);
    if (result == 0 && isCurrent()) result = queryCatalog(q, true, out);
    return result;
}
} // namespace df
