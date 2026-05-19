// DslCheckMain.cpp
// =============================================================================
// STANDALONE DSL CHECKER - PARSE + SEMA A .compat FILE, EXIT NON-ZERO ON ERR
// =============================================================================
// Written by: Eriumsss

#include "../Compat/DSL/Diagnostics.h"
#include "../Compat/DSL/Parser.h"
#include "../Compat/DSL/Sema.h"
#include "../Compat/DSL/SourceMap.h"
#include "../Core/Arena.h"
#include "../Core/PathUtils.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    using namespace Vespucci;
    if (argc < 2) {
        std::printf("usage: dsl_check.exe <file.compat>\n");
        return 1;
    }
    const char* path = argv[1];
    std::string contents;
    if (!Core::Path::ReadFile(path, contents)) {
        std::fprintf(stderr, "fatal: cannot read %s\n", path);
        return 2;
    }

    Compat::DSL::Parser p;
    Core::Arena arena(64 * 1024);
    Compat::DSL::DiagnosticBag bag;
    Compat::DSL::AstNode* root = p.parse(contents.c_str(), contents.size(), &arena, &bag);

    Compat::DSL::SourceMap smap;
    smap.build(contents.c_str(), contents.size());

    if (root) {
        Compat::DSL::Sema sema;
        sema.run(root, Schema::GlobalRegistry(),
                 Schema::GlobalSignatureDB(), &bag);
    }
    std::string rendered = bag.render(path, contents.c_str(), contents.size(), smap);
    std::printf("%s", rendered.c_str());

    if (bag.hasErrors()) {
        std::printf("FAIL (%d error(s))\n", bag.errorCount());
        return 3;
    }
    std::printf("OK (%d warning(s))\n", bag.warningCount());
    return 0;
}
