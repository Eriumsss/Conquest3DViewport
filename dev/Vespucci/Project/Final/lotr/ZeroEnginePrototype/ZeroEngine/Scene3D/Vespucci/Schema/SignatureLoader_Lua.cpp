// SignatureLoader_Lua.cpp
// =============================================================================
// MOD-FRIENDLY SIGNATURE LOADER — SCAVENGE EVENTS / ACTIONS FROM .LUA
// =============================================================================
// Written by: Eriumsss
//
// Pandemic ships a half-broken Lua animation graph the engine still
// runs (project_animation_system.md memory has the gory details).
// Mission scripts in maps live in .lua files alongside the .lvl,
// and they reference event/action names AS STRINGS that the binary
// .lvl alone does not record. If we want autocomplete to suggest
// "OnAragornsHorseArrives" or "OpenTheBigGoddamnGate" we need to
// scrape those names out of the Lua source.
//
// We DO NOT embed a Lua interpreter for this. The engine has one
// already and we are not paying that fucking weight twice. Instead,
// this is a dumb regex-style scanner that walks the .lua text and
// pulls out:
//
//   - Trigger("name") / FireEvent("name")  ->  emitted event names
//   - Listen("name", ...) / OnEvent("name", ...) -> accepted actions
//   - LinkOutput(source, "EventName", target, "ActionName")
//
// Each name found is registered against the source/target's TypeId
// (when we can resolve it) or against a pseudo-type "ScriptDefined"
// (when we cannot). The corpus builder later uses these names to
// rank autocomplete candidates.
//
// Failure modes are intentionally non-fatal: if the Lua syntax has
// drifted from what we expect, we log Warn and skip. The whole point
// is "best-effort enrichment", not "schema-of-record". Zero crashes
// allowed in this loader regardless of what insanity is in the file.
// =============================================================================

#include "EventActionSignatureDB.h"
#include "ZETypeRegistry.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"
#include "../Core/PathUtils.h"
#include "../Core/StringRef.h"
#include "../Core/VespucciAssert.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace Vespucci {
namespace Schema {

// Forward decls — file-local because LoadSignaturesFromLuaFile calls
// LoadSignaturesFromLuaText which is defined further down. Keeps the
// header free of impl-only entry points the rest of the codebase
// does not need.
i32 LoadSignaturesFromLuaText(const char* text,
                              usize length,
                              EventActionSignatureDB& db,
                              ZETypeRegistry& reg);

namespace {
    // Lowercase in-place. Trimmed-down because <cctype>::tolower
    // is locale-dependent and we want pure ASCII.
    void LowerAscii(std::string& s) {
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') s[i] = (char)(c + 32);
        }
    }

    // Skip whitespace AND -- single-line + --[[ block comments.
    void SkipLuaTriviaAt(const char* text, usize len, usize& i) {
        for (;;) {
            if (i >= len) return;
            char c = text[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
            if (c == '-' && i + 1 < len && text[i + 1] == '-') {
                i += 2;
                if (i + 1 < len && text[i] == '[' && text[i + 1] == '[') {
                    i += 2;
                    while (i + 1 < len && !(text[i] == ']' && text[i + 1] == ']')) i++;
                    if (i + 1 < len) i += 2;
                } else {
                    while (i < len && text[i] != '\n') i++;
                }
                continue;
            }
            return;
        }
    }

    // Match a literal keyword at i. Returns true on match and advances
    // i past the keyword + ANY required following non-identifier-char.
    bool MatchKeyword(const char* text, usize len, usize& i, const char* kw) {
        usize klen = std::strlen(kw);
        if (i + klen > len) return false;
        if (std::memcmp(text + i, kw, klen) != 0) return false;
        // Must be followed by a non-identifier-continuation char.
        if (i + klen < len) {
            char nx = text[i + klen];
            if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z') ||
                (nx >= '0' && nx <= '9') || nx == '_') return false;
        }
        i += klen;
        return true;
    }

    // Read a quoted Lua string. Supports both " and ' quotes, basic
    // escape \\ and \". Returns the unquoted body or empty on bad.
    bool ReadQuotedString(const char* text, usize len, usize& i, std::string& out) {
        out.clear();
        if (i >= len) return false;
        char q = text[i];
        if (q != '"' && q != '\'') return false;
        i++;
        while (i < len && text[i] != q) {
            if (text[i] == '\\' && i + 1 < len) {
                char e = text[i + 1];
                switch (e) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case '"':  out.push_back('"');  break;
                    case '\'': out.push_back('\''); break;
                    case '\\': out.push_back('\\'); break;
                    default:   out.push_back(e);    break;
                }
                i += 2;
            } else {
                out.push_back(text[i]);
                i++;
            }
        }
        if (i < len) i++; // skip closing quote
        return true;
    }

    // Read a Lua identifier. Returns empty on bad.
    void ReadIdent(const char* text, usize len, usize& i, std::string& out) {
        out.clear();
        while (i < len) {
            char c = text[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                out.push_back(c);
                i++;
            } else break;
        }
    }

    // Look-back to find the entity-type name preceding a method call.
    // E.g. "MyTrigger:Fire(\"OnEnter\")" - we want to recognize
    // "MyTrigger" as a typed receiver and resolve its type.
    // Scope is one identifier and an optional "Type:Method" pattern.
    bool TryReadReceiver(const char* text, usize len, usize i, std::string& out) {
        // Walk backwards from `i` to find the start of an identifier.
        usize end = i;
        while (end > 0 && std::isspace((unsigned char)text[end - 1])) end--;
        if (end == 0) return false;
        usize start = end;
        while (start > 0) {
            char c = text[start - 1];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') start--;
            else break;
        }
        if (start == end) return false;
        out.assign(text + start, end - start);
        return !out.empty();
    }

    // Best-effort type resolution from a Lua receiver name. We do
    // NOT have the binary name<->type mapping at parse time so we
    // do simple heuristics: strip common suffixes ("_01", "Trigger",
    // "Inst"), lowercase, look up in registry. Failure returns
    // TypeId(0) and the loader falls back to "ScriptDefined" pseudo.
    TypeId ResolveTypeFromReceiver(const std::string& recv,
                                    const ZETypeRegistry& reg)
    {
        std::string canonical = recv;
        LowerAscii(canonical);
        // Strip trailing digits and underscores.
        while (!canonical.empty()) {
            char c = canonical.back();
            if ((c >= '0' && c <= '9') || c == '_') canonical.pop_back();
            else break;
        }
        const TypeRecord* rec = reg.findByCanonical(canonical.c_str());
        if (rec) return rec->id;
        // Heuristic: try common suffix family names (capturepoint,
        // triggervolume, scriptrelay) by suffix match.
        static const char* kFamilies[] = {
            "capturepoint", "triggervolume", "scriptrelay", "scriptcounter",
            "scripttimer", "scripttoggle", "scriptbranch", "scriptsequence",
            "votrigger", "audiomanager", "musiccue", "animationcontroller",
            "spawner", "wavespawner", "destructibleobject", "dooractivator",
            "gatecontroller", "drawbridge", 0
        };
        for (i32 i = 0; kFamilies[i]; ++i) {
            const char* fam = kFamilies[i];
            usize famLen = std::strlen(fam);
            if (canonical.size() >= famLen) {
                if (canonical.compare(canonical.size() - famLen, famLen, fam) == 0) {
                    rec = reg.findByCanonical(fam);
                    if (rec) return rec->id;
                }
            }
        }
        return TypeId(0);
    }

    // Pseudo-type for unresolved receivers. Registered lazily so a
    // map without unresolved Lua does not pay the entry cost.
    TypeId GetOrRegisterScriptDefined(ZETypeRegistry& reg) {
        const TypeRecord* r = reg.findByCanonical("scriptdefined");
        if (r) return r->id;
        const TypeRecord* entityRec = reg.findByCanonical("entity");
        TypeId entityId = entityRec ? entityRec->id : TypeId(0);
        return reg.registerType("ScriptDefined", "scriptdefined",
                                entityId,
                                TRAIT_HasEvents | TRAIT_HasInputs,
                                0,
                                "Pseudo-type for entities defined in mission Lua but not in the binary type table.");
    }
} // namespace

// Public entry: scan ONE .lua file and submit observed events / actions
// to the signature DB. Returns count of new entries registered.
i32 LoadSignaturesFromLuaFile(const char* path,
                               EventActionSignatureDB& db,
                               ZETypeRegistry& reg)
{
    if (!path || !*path) return 0;
    std::string text;
    if (!Core::Path::ReadFile(path, text)) {
        Core::Logging::Warn("SignatureLoader: cannot read %s", path);
        return 0;
    }
    return LoadSignaturesFromLuaText(text.c_str(), text.size(), db, reg);
}

// Scan a Lua text buffer. Implementation does not depend on a real
// Lua parser - just heuristic pattern matching.
i32 LoadSignaturesFromLuaText(const char* text,
                               usize length,
                               EventActionSignatureDB& db,
                               ZETypeRegistry& reg)
{
    if (!text || length == 0) return 0;

    i32 added = 0;
    TypeId scriptDefined = GetOrRegisterScriptDefined(reg);

    for (usize i = 0; i < length;) {
        SkipLuaTriviaAt(text, length, i);
        if (i >= length) break;

        // Try matching one of the recognized call patterns.
        usize savedI = i;

        // Pattern A: <receiver>:<verb>("<name>", ...)
        // Walk forward to a colon. If found, the verb identifies an
        // event-emit or action-receive and the first quoted arg is
        // the name.
        // Pattern B: LinkOutput(src, "EventName", tgt, "ActionName")
        // Pattern C: FireEvent("Name") / Trigger("Name") at top-level.

        // Pattern B first - explicit and unambiguous.
        if (MatchKeyword(text, length, i, "LinkOutput") ||
            MatchKeyword(text, length, i, "Wire") ||
            MatchKeyword(text, length, i, "ConnectEvent"))
        {
            SkipLuaTriviaAt(text, length, i);
            if (i < length && text[i] == '(') {
                i++;
                // Skip first arg (source ref) — tokens until first comma.
                std::string srcIdent;
                ReadIdent(text, length, i, srcIdent);
                SkipLuaTriviaAt(text, length, i);
                if (i < length && text[i] == ',') i++;
                SkipLuaTriviaAt(text, length, i);
                std::string evtName;
                ReadQuotedString(text, length, i, evtName);
                SkipLuaTriviaAt(text, length, i);
                if (i < length && text[i] == ',') i++;
                SkipLuaTriviaAt(text, length, i);
                std::string tgtIdent;
                ReadIdent(text, length, i, tgtIdent);
                SkipLuaTriviaAt(text, length, i);
                if (i < length && text[i] == ',') i++;
                SkipLuaTriviaAt(text, length, i);
                std::string actName;
                ReadQuotedString(text, length, i, actName);
                // Skip rest of the call.
                while (i < length && text[i] != ')') i++;
                if (i < length) i++;

                if (!evtName.empty() && !srcIdent.empty()) {
                    TypeId srcType = ResolveTypeFromReceiver(srcIdent, reg);
                    if (!srcType.valid()) srcType = scriptDefined;
                    std::string canonical = evtName;
                    LowerAscii(canonical);
                    db.registerEvent(srcType, canonical.c_str(),
                                     evtName.c_str(),
                                     "Discovered in Lua via LinkOutput / Wire / ConnectEvent.",
                                     1);
                    added++;
                }
                if (!actName.empty() && !tgtIdent.empty()) {
                    TypeId tgtType = ResolveTypeFromReceiver(tgtIdent, reg);
                    if (!tgtType.valid()) tgtType = scriptDefined;
                    std::string canonical = actName;
                    LowerAscii(canonical);
                    db.registerAction(tgtType, canonical.c_str(),
                                      actName.c_str(),
                                      "Discovered in Lua via LinkOutput / Wire / ConnectEvent.",
                                      1);
                    added++;
                }
                continue;
            }
            // No '(' — bail and move on.
            i = savedI + 1;
            continue;
        }

        // Pattern C: FireEvent / Trigger / OnEvent / Listen at top level.
        bool isEmit  = false;
        bool isAccept = false;
        if      (MatchKeyword(text, length, i, "FireEvent"))  isEmit = true;
        else if (MatchKeyword(text, length, i, "Trigger"))    isEmit = true;
        else if (MatchKeyword(text, length, i, "Emit"))       isEmit = true;
        else if (MatchKeyword(text, length, i, "OnEvent"))    isAccept = true;
        else if (MatchKeyword(text, length, i, "Listen"))     isAccept = true;

        if (isEmit || isAccept) {
            SkipLuaTriviaAt(text, length, i);
            if (i < length && text[i] == '(') {
                i++;
                SkipLuaTriviaAt(text, length, i);
                std::string name;
                if (ReadQuotedString(text, length, i, name) && !name.empty()) {
                    std::string canonical = name;
                    LowerAscii(canonical);
                    if (isEmit) {
                        db.registerEvent(scriptDefined, canonical.c_str(),
                                         name.c_str(),
                                         "Discovered in Lua via FireEvent / Trigger / Emit.", 1);
                    } else {
                        db.registerAction(scriptDefined, canonical.c_str(),
                                          name.c_str(),
                                          "Discovered in Lua via OnEvent / Listen.", 1);
                    }
                    added++;
                }
                while (i < length && text[i] != ')') i++;
                if (i < length) i++;
                continue;
            }
            i = savedI + 1;
            continue;
        }

        // No pattern matched — advance one char and continue.
        i++;
    }

    if (added > 0) {
        Core::Logging::Info("SignatureLoader: %d entries from Lua scan", added);
    }
    return added;
}

// Public forward declarations exposed via the header would normally
// live there, but SignatureLoader is a helper we only call from a
// handful of sites, so we let them include this .cpp's header? No -
// we expose the entry through an EXTERN declaration in
// EventActionSignatureDB.h (added when the loader gets wired into
// Vespucci::Init). For now the calls above are file-local; the
// unresolved-symbol warning is acceptable until Phase B finalize.

} // namespace Schema
} // namespace Vespucci
