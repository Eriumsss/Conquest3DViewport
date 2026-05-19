// HeaderScraper.cpp
// =============================================================================
// SCRAPE HEADER FILES FOR PUBLIC API + EMIT MARKDOWN DOCS
// =============================================================================
// Written by: Eriumsss
//
// Walk every Vespucci/**/*.h, extract namespace declarations + the
// signatures + the comment block above each, dump to markdown.
// This is the doc-generator that outputs reference material for
// plugin authors. Zero clang/libtooling dependency - we use
// regex-style line matching because Vespucci headers follow a
// disciplined enough style that a real C++ parser is overkill.
// =============================================================================

#include "../Core/FileIO.h"
#include "../Core/PathUtils.h"
#include "../Core/Logging.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Docs {

struct ScrapedSymbol {
    std::string namespacePath;
    std::string commentBlock;
    std::string signatureLine;
    std::string sourceFile;
    i32         lineNumber;
};

namespace {
    bool IsCommentLine(const std::string& l) {
        size_t i = 0;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
        return i + 1 < l.size() && l[i] == '/' && l[i + 1] == '/';
    }
    bool LineDeclaresFunction(const std::string& l) {
        // Cheap heuristic: contains '(' and ')' and not preceded by '#'.
        if (l.empty() || l[0] == '#') return false;
        size_t lp = l.find('(');
        size_t rp = l.find(')');
        return lp != std::string::npos && rp != std::string::npos && lp < rp;
    }
} // namespace

void ScrapeOneHeader(const char* path, std::vector<ScrapedSymbol>& outSymbols) {
    std::string contents;
    if (!Core::Path::ReadFile(path, contents)) return;

    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < contents.size(); ++i) {
            if (contents[i] == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(contents[i]);
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    std::string commentAccum;
    std::string nsPath;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (IsCommentLine(l)) {
            commentAccum += l;
            commentAccum.push_back('\n');
            continue;
        }
        if (l.find("namespace ") != std::string::npos) {
            // Best-effort namespace tracking - just append the named token.
            size_t pos = l.find("namespace ") + 10;
            while (pos < l.size() && (l[pos] == ' ' || l[pos] == '\t')) ++pos;
            std::string name;
            while (pos < l.size() && (isalnum((unsigned char)l[pos]) || l[pos] == '_')) {
                name.push_back(l[pos]); ++pos;
            }
            if (!name.empty()) {
                if (!nsPath.empty()) nsPath += "::";
                nsPath += name;
            }
        }
        if (LineDeclaresFunction(l) && !commentAccum.empty()) {
            ScrapedSymbol sym;
            sym.namespacePath = nsPath;
            sym.commentBlock  = commentAccum;
            sym.signatureLine = l;
            sym.sourceFile    = path;
            sym.lineNumber    = (i32)(i + 1);
            outSymbols.push_back(sym);
        }
        commentAccum.clear();
    }
    Core::Logging::Debug("HeaderScraper: scraped %s -> %zu symbols",
        path, outSymbols.size());
}

} // namespace Docs
} // namespace Vespucci
