// JsonLite.h
// =============================================================================
// JSONLITE — A SMALL, MEAN, NO-DEPENDENCY JSON READER + WRITER
// =============================================================================
// Written by: Eriumsss
//
// We need JSON for:
//   - Suggest/weights.json (ranker tuning, hot-reloadable)
//   - golden_suggestions.json (QA fixtures)
//   - replay_*.json (recorded editor sessions)
//   - forge_compat.json (compat-matrix overrides)
//   - per-Tools CLI output
//
// All of these are small (< 1 MB) and built/parsed offline or once
// at startup. We don't need rapidjson, simdjson, nlohmann or any of
// the other 50KB-of-template-soup dependencies the C++ ecosystem
// pretends are mandatory for working with JSON. We need a 2KB
// recursive-descent parser and a string-builder writer that does
// what every JSON spec says it should do, and then we stop.
//
// Spec coverage:
//   - Objects, arrays, strings, numbers, true/false/null. Nested.
//   - String escape: \\, \", \n, \t, \r, \b, \f, \/, \uXXXX (BMP only).
//   - Numbers: i64 if integer, f64 if has decimal point or exponent.
//   - Whitespace per RFC 8259.
//   - C++-style "// line comment" extension (NOT spec, we accept it
//     because hand-edited tuning files want it, the writer never
//     emits it).
//
// API: a Value struct with type + variant, parsed from a string,
// queried via accessor methods that assert on type mismatch. Parsing
// allocates inside an arena owned by the parser; the user reads
// via const Value& references that live as long as the parser.
//
// THIS IS NOT FOR LOAD-BEARING UNTRUSTED INPUT. If we ever need to
// parse JSON over the wire from a hostile source, we get a real
// hardened parser. This is for our own ranker weights and golden
// fixtures - the worst attacker is a typo by the developer.
// =============================================================================

#ifndef VESPUCCI_CORE_JSONLITE_H_
#define VESPUCCI_CORE_JSONLITE_H_

#include "VespucciTypes.h"
#include "StringRef.h"

#include <string>

namespace Vespucci {
namespace Core {
namespace JsonLite {

enum ValueKind {
    KIND_Null    = 0,
    KIND_Bool    = 1,
    KIND_Int     = 2,
    KIND_Double  = 3,
    KIND_String  = 4,
    KIND_Array   = 5,
    KIND_Object  = 6
};

struct Value;

// Object members: each entry is a (key, value) pair. We store as a
// flat array to keep allocation tight. Lookup is linear; for our
// "weights.json"-sized inputs that's fine.
struct ObjectMember {
    StringRef key;
    Value*    value;
};

struct Value {
    ValueKind kind;
    union {
        bool   b;
        i64    i;
        f64    d;
    } scalar;
    StringRef     str;            // valid when kind == STRING
    Value**       arrItems;       // valid when kind == ARRAY
    i32           arrCount;
    ObjectMember* objMembers;     // valid when kind == OBJECT
    i32           objCount;
};

// Parser. Owns an Arena for backing allocations. Document object lives
// in the arena and is exposed via root().
class Parser {
public:
    Parser();
    ~Parser();

    // Parse a JSON string. Returns true on success, false on parse
    // error (call lastError() for the message).
    bool parse(const char* text, usize length);
    bool parseFile(const char* path);

    const Value* root() const { return m_root; }
    const char*  lastError() const { return m_error.c_str(); }
    i32          errorLine() const { return m_errorLine; }
    i32          errorColumn() const { return m_errorColumn; }

private:
    // Recursive-descent helpers.
    Value*    parseValue();
    Value*    parseObject();
    Value*    parseArray();
    StringRef parseString();
    Value*    parseNumber();
    Value*    parseLiteral(); // true/false/null

    void      skipWhitespaceAndComments();
    bool      consume(char c);
    void      setError(const char* msg);

    void* arenaAlloc(usize bytes, usize align);
    Value* newValue(ValueKind k);

    const char* m_text;
    usize       m_len;
    usize       m_cursor;
    i32         m_line;
    i32         m_column;

    Value*      m_root;
    void*       m_arena;     // Arena*
    std::string m_error;
    i32         m_errorLine;
    i32         m_errorColumn;
};

// Accessors. Assert on type mismatch.
bool        AsBool(const Value& v);
i64         AsInt(const Value& v);
f64         AsDouble(const Value& v);
StringRef   AsString(const Value& v);
i32         ArraySize(const Value& v);
const Value* ArrayAt(const Value& v, i32 i);
i32         ObjectSize(const Value& v);
const Value* ObjectGet(const Value& v, const char* key);
StringRef   ObjectKeyAt(const Value& v, i32 i);
const Value* ObjectValueAt(const Value& v, i32 i);

// Type-safe optional getters: return defaultVal if missing or wrong type.
i64       OptInt(const Value* v, const char* key, i64 defVal);
f64       OptDouble(const Value* v, const char* key, f64 defVal);
bool      OptBool(const Value* v, const char* key, bool defVal);
StringRef OptString(const Value* v, const char* key, const char* defVal);

// Writer. Builds JSON into a std::string. No pretty-printing options
// because the user-facing weights.json is hand-formatted and nobody
// reads the replay files visually.
class Writer {
public:
    Writer();

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();

    void key(const char* k);
    void writeNull();
    void writeBool(bool b);
    void writeInt(i64 v);
    void writeDouble(f64 v);
    void writeString(const char* s);
    void writeString(const StringRef& s);

    // Convenience: key + value combinators.
    void kvNull(const char* k);
    void kvBool(const char* k, bool b);
    void kvInt(const char* k, i64 v);
    void kvDouble(const char* k, f64 v);
    void kvString(const char* k, const char* s);

    const std::string& result() const { return m_buf; }

private:
    void emitComma();
    void emitEscaped(const char* s, usize n);

    std::string m_buf;
    bool        m_needComma;
    i32         m_depth;
};

} // namespace JsonLite
} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_JSONLITE_H_
