// SchemaDumpMain.cpp
// =============================================================================
// DUMP THE BUILT-IN SCHEMA REGISTRY AS HUMAN-READABLE TEXT
// =============================================================================
// Written by: Eriumsss

#include "../Schema/ZETypeRegistry.h"
#include "../Schema/EventActionSignatureDB.h"

#include <cstdio>

int main(int argc, char** argv) {
    using namespace Vespucci;
    Schema::ZETypeRegistry reg;
    reg.init();
    Schema::EventActionSignatureDB db;
    db.init(&reg);

    i32 n = reg.count();
    std::printf("Vespucci Schema Dump\n");
    std::printf("====================\n");
    std::printf("Types registered: %d\n\n", n);
    for (i32 i = 0; i < n; ++i) {
        const Schema::TypeRecord* r = reg.at(i);
        if (!r) continue;
        std::printf("[%3d] %s  (canonical=%s, traits=0x%X)\n",
            i, r->name.data(), r->canonicalName.data(), (unsigned)r->traits);
        const Schema::TypeSignature* sig = db.findSignature(r->id);
        if (sig) {
            for (i32 j = 0; j < sig->emittedCount; ++j) {
                std::printf("        emit  %s  (freq %u)\n",
                    sig->emittedEvents[j].displayName.data(),
                    (unsigned)sig->emittedEvents[j].frequency);
            }
            for (i32 j = 0; j < sig->acceptedCount; ++j) {
                std::printf("        accept %s  (freq %u)\n",
                    sig->acceptedActions[j].displayName.data(),
                    (unsigned)sig->acceptedActions[j].frequency);
            }
        }
    }
    (void)argc; (void)argv;
    return 0;
}
