#include "../src/file_url_path.h"

#include <cassert>
#include <iostream>

int main() {
    assert(FileURLToPath(L"file:///C:/Users/Ted/My%20Docs/%E4%BD%A0%E5%A5%BD") ==
           L"C:\\Users\\Ted\\My Docs\\你好");
    assert(FileURLToPath(L"file://server/share/My%20Docs") ==
           L"\\\\server\\share\\My Docs");
    assert(FileURLToPath(L"https://example.com/path").empty());
    std::cout << "File URL decoding passed (spaces, UTF-8, UNC, non-file URL).\n";
}
