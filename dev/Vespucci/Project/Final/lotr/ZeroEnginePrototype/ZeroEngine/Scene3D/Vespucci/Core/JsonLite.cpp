// JsonLite.cpp
// =============================================================================
// JSON reader + writer. Recursive-descent. Arena-backed. Single-pass.
// Spec coverage above; if you need anything beyond it you are using
// the wrong tool and should reach for a real JSON library.
// =============================================================================
// Written by: Eriumsss

#include "JsonLite.h"
#include "Arena.h"
#include "PathUtils.h"
#include "Logging.h"
#include "VespucciAssert.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Vespucci {
namespace Core {
namespace JsonLite {

namespace {
    inline bool IsHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    inline u32 HexNibble(char c) {
        if (c >= '0' && c <= '9') return (u32)(c - '0');
        if (c >= 'a' && c <= 'f') return (u32)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (u32)(c - 'A' + 10);
        return 0u;
    }
    void EncodeUtf8(u32 cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0u | (cp >> 6)));
            out.push_back((char)(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back((char)(0xE0u | (cp >> 12)));
            out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back((char)(0x80u | (cp & 0x3Fu)));
        }
    }
} // namespace

// ── Parser ────────────────────────────────────────────────────────────

Parser::Parser()
    : m_text(0), m_len(0), m_cursor(0), m_line(1), m_column(1),
      m_root(0), m_arena(0), m_errorLine(0), m_errorColumn(0)
{
    m_arena = new Arena(64 * 1024);
}

Parser::~Parser() {
    delete (Arena*)m_arena;
}

void* Parser::arenaAlloc(usize bytes, usize align) {
    return ((Arena*)m_arena)->alloc(bytes, align);
}

Value* Parser::newValue(ValueKind k) {
    Value* v = (Value*)arenaAlloc(sizeof(Value), alignof(Value));
    if (!v) return 0;
    v->kind = k;
    v->scalar.i = 0;
    v->str = StringRef();
    v->arrItems = 0;
    v->arrCount = 0;
    v->objMembers = 0;
    v->objCount = 0;
    return v;
}

void Parser::setError(const char* msg) {
    if (!m_error.empty()) return; // first error wins
    m_error = msg;
    m_errorLine = m_line;
    m_errorColumn = m_column;
}

void Parser::skipWhitespaceAndComments() {
    while (m_cursor < m_len) {
        char c = m_text[m_cursor];
        if (c == ' ' || c == '\t' || c == '\r') {
            m_cursor++; m_column++;
        } else if (c == '\n') {
            m_cursor++; m_line++; m_column = 1;
        } else if (c == '/' && m_cursor + 1 < m_len && m_text[m_cursor + 1] == '/') {
            // Line comment - non-standard extension, accepted on read.
            while (m_cursor < m_len && m_text[m_cursor] != '\n') {
                m_cursor++; m_column++;
            }
        } else {
            break;
        }
    }
}

bool Parser::consume(char c) {
    skipWhitespaceAndComments();
    if (m_cursor >= m_len || m_text[m_cursor] != c) return false;
    m_cursor++; m_column++;
    return true;
}

bool Parser::parse(const char* text, usize length) {
    m_text = text; m_len = length; m_cursor = 0;
    m_line = 1; m_column = 1;
    m_error.clear();
    m_errorLine = 0; m_errorColumn = 0;
    ((Arena*)m_arena)->reset();

    skipWhitespaceAndComments();
    m_root = parseValue();
    if (!m_root) {
        if (m_error.empty()) setError("empty input");
        return false;
    }
    skipWhitespaceAndComments();
    if (m_cursor < m_len) {
        setError("trailing garbage after root value");
        return false;
    }
    return true;
}

bool Parser::parseFile(const char* path) {
    std::string contents;
    if (!Path::ReadFile(path, contents)) {
        m_error = "file read failed";
        return false;
    }
    return parse(contents.data(), contents.size());
}

Value* Parser::parseValue() {
    skipWhitespaceAndComments();
    if (m_cursor >= m_len) {
        setError("unexpected end of input");
        return 0;
    }
    char c = m_text[m_cursor];
    if (c == '{') return parseObject();
    if (c == '[') return parseArray();
    if (c == '"') {
        StringRef s = parseString();
        if (!m_error.empty()) return 0;
        Value* v = newValue(KIND_String);
        if (v) v->str = s;
        return v;
    }
    if (c == 't' || c == 'f' || c == 'n') return parseLiteral();
    if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
    setError("unexpected character at value start");
    return 0;
}

Value* Parser::parseObject() {
    if (!consume('{')) { setError("expected '{'"); return 0; }
    Value* v = newValue(KIND_Object);
    if (!v) return 0;

    skipWhitespaceAndComments();
    if (m_cursor < m_len && m_text[m_cursor] == '}') {
        m_cursor++; m_column++;
        return v;
    }

    // Member buffer. We don't know count up front so we grow into
    // an arena-backed temp scratch. Max members per object capped
    // at 4096 — design lock, not a runtime fence we expect anyone
    // to actually hit.
    static const i32 kMaxMembers = 4096;
    ObjectMember* tmp = (ObjectMember*)arenaAlloc(sizeof(ObjectMember) * (size_t)kMaxMembers,
                                                   alignof(ObjectMember));
    if (!tmp) return 0;
    i32 count = 0;

    for (;;) {
        skipWhitespaceAndComments();
        if (m_cursor >= m_len || m_text[m_cursor] != '"') {
            setError("expected string key in object");
            return 0;
        }
        StringRef key = parseString();
        if (!m_error.empty()) return 0;
        skipWhitespaceAndComments();
        if (!consume(':')) { setError("expected ':' after key"); return 0; }
        Value* val = parseValue();
        if (!val) return 0;
        if (count < kMaxMembers) {
            tmp[count].key   = key;
            tmp[count].value = val;
            count++;
        }
        skipWhitespaceAndComments();
        if (consume(',')) continue;
        if (consume('}')) break;
        setError("expected ',' or '}' in object");
        return 0;
    }

    v->objMembers = tmp;
    v->objCount   = count;
    return v;
}

Value* Parser::parseArray() {
    if (!consume('[')) { setError("expected '['"); return 0; }
    Value* v = newValue(KIND_Array);
    if (!v) return 0;

    skipWhitespaceAndComments();
    if (m_cursor < m_len && m_text[m_cursor] == ']') {
        m_cursor++; m_column++;
        return v;
    }

    static const i32 kMaxItems = 65536;
    Value** tmp = (Value**)arenaAlloc(sizeof(Value*) * (size_t)kMaxItems, alignof(Value*));
    if (!tmp) return 0;
    i32 count = 0;

    for (;;) {
        Value* item = parseValue();
        if (!item) return 0;
        if (count < kMaxItems) {
            tmp[count] = item;
            count++;
        }
        skipWhitespaceAndComments();
        if (consume(',')) continue;
        if (consume(']')) break;
        setError("expected ',' or ']' in array");
        return 0;
    }

    v->arrItems = tmp;
    v->arrCount = count;
    return v;
}

StringRef Parser::parseString() {
    if (!consume('"')) { setError("expected '\"'"); return StringRef(); }
    // Accumulate decoded bytes into an arena buffer of pessimistic
    // size = source length. Strings cannot grow on decode (escapes
    // get shorter or stay same length for ASCII; \uXXXX BMP is at
    // most 3 UTF-8 bytes vs 6 source chars, also shorter).
    usize start = m_cursor;
    char* out = (char*)arenaAlloc(m_len - m_cursor + 1, 1);
    if (!out) return StringRef();
    char* p = out;

    while (m_cursor < m_len) {
        char c = m_text[m_cursor];
        if (c == '"') {
            m_cursor++; m_column++;
            *p = 0;
            return StringRef(out, (usize)(p - out));
        }
        if (c == '\\') {
            if (m_cursor + 1 >= m_len) { setError("trailing backslash"); return StringRef(); }
            char esc = m_text[m_cursor + 1];
            m_cursor += 2; m_column += 2;
            switch (esc) {
                case '"':  *p++ = '"';  break;
                case '\\': *p++ = '\\'; break;
                case '/':  *p++ = '/';  break;
                case 'n':  *p++ = '\n'; break;
                case 't':  *p++ = '\t'; break;
                case 'r':  *p++ = '\r'; break;
                case 'b':  *p++ = '\b'; break;
                case 'f':  *p++ = '\f'; break;
                case 'u': {
                    if (m_cursor + 4 > m_len) { setError("short \\u escape"); return StringRef(); }
                    u32 cp = 0;
                    for (i32 i = 0; i < 4; ++i) {
                        char h = m_text[m_cursor + i];
                        if (!IsHex(h)) { setError("bad hex in \\u"); return StringRef(); }
                        cp = (cp << 4) | HexNibble(h);
                    }
                    m_cursor += 4; m_column += 4;
                    // Surrogate pair handling: BMP only here, surrogate
                    // pairs decode to a single � because this is
                    // a tiny parser and we said so up top.
                    if (cp >= 0xD800u && cp <= 0xDFFFu) cp = 0xFFFDu;
                    std::string tmp; EncodeUtf8(cp, tmp);
                    for (size_t i = 0; i < tmp.size(); ++i) *p++ = tmp[i];
                    break;
                }
                default: setError("unknown escape"); return StringRef();
            }
            (void)start;
            continue;
        }
        if (c == '\n') { m_line++; m_column = 1; } else { m_column++; }
        *p++ = c;
        m_cursor++;
    }
    setError("unterminated string");
    return StringRef();
}

Value* Parser::parseNumber() {
    usize start = m_cursor;
    bool isFloat = false;
    if (m_cursor < m_len && m_text[m_cursor] == '-') { m_cursor++; m_column++; }
    while (m_cursor < m_len && m_text[m_cursor] >= '0' && m_text[m_cursor] <= '9') {
        m_cursor++; m_column++;
    }
    if (m_cursor < m_len && m_text[m_cursor] == '.') {
        isFloat = true;
        m_cursor++; m_column++;
        while (m_cursor < m_len && m_text[m_cursor] >= '0' && m_text[m_cursor] <= '9') {
            m_cursor++; m_column++;
        }
    }
    if (m_cursor < m_len && (m_text[m_cursor] == 'e' || m_text[m_cursor] == 'E')) {
        isFloat = true;
        m_cursor++; m_column++;
        if (m_cursor < m_len && (m_text[m_cursor] == '+' || m_text[m_cursor] == '-')) {
            m_cursor++; m_column++;
        }
        while (m_cursor < m_len && m_text[m_cursor] >= '0' && m_text[m_cursor] <= '9') {
            m_cursor++; m_column++;
        }
    }
    usize len = m_cursor - start;
    if (len == 0) { setError("expected number"); return 0; }

    char buf[64];
    if (len >= sizeof(buf)) { setError("number too long"); return 0; }
    std::memcpy(buf, m_text + start, len);
    buf[len] = 0;

    Value* v = 0;
    if (isFloat) {
        v = newValue(KIND_Double);
        if (v) v->scalar.d = std::strtod(buf, 0);
    } else {
        v = newValue(KIND_Int);
        if (v) v->scalar.i = (i64)std::strtoll(buf, 0, 10);
    }
    return v;
}

Value* Parser::parseLiteral() {
    if (m_cursor + 4 <= m_len && std::strncmp(m_text + m_cursor, "true", 4) == 0) {
        m_cursor += 4; m_column += 4;
        Value* v = newValue(KIND_Bool); if (v) v->scalar.b = true; return v;
    }
    if (m_cursor + 5 <= m_len && std::strncmp(m_text + m_cursor, "false", 5) == 0) {
        m_cursor += 5; m_column += 5;
        Value* v = newValue(KIND_Bool); if (v) v->scalar.b = false; return v;
    }
    if (m_cursor + 4 <= m_len && std::strncmp(m_text + m_cursor, "null", 4) == 0) {
        m_cursor += 4; m_column += 4;
        return newValue(KIND_Null);
    }
    setError("unknown literal");
    return 0;
}

// ── Accessors ─────────────────────────────────────────────────────────

bool       AsBool(const Value& v)   { VESPUCCI_CHECK(v.kind == KIND_Bool);    return v.scalar.b; }
i64        AsInt(const Value& v)    { VESPUCCI_CHECK(v.kind == KIND_Int);     return v.scalar.i; }
f64        AsDouble(const Value& v) {
    if (v.kind == KIND_Int) return (f64)v.scalar.i;
    VESPUCCI_CHECK(v.kind == KIND_Double); return v.scalar.d;
}
StringRef  AsString(const Value& v) { VESPUCCI_CHECK(v.kind == KIND_String);  return v.str; }
i32        ArraySize(const Value& v){ VESPUCCI_CHECK(v.kind == KIND_Array);   return v.arrCount; }

const Value* ArrayAt(const Value& v, i32 i) {
    VESPUCCI_CHECK(v.kind == KIND_Array);
    if (i < 0 || i >= v.arrCount) return 0;
    return v.arrItems[i];
}

i32 ObjectSize(const Value& v) { VESPUCCI_CHECK(v.kind == KIND_Object); return v.objCount; }

const Value* ObjectGet(const Value& v, const char* key) {
    VESPUCCI_CHECK(v.kind == KIND_Object);
    if (!key) return 0;
    usize klen = std::strlen(key);
    for (i32 i = 0; i < v.objCount; ++i) {
        const ObjectMember& m = v.objMembers[i];
        if (m.key.size() == klen && std::memcmp(m.key.data(), key, klen) == 0) {
            return m.value;
        }
    }
    return 0;
}

StringRef ObjectKeyAt(const Value& v, i32 i) {
    VESPUCCI_CHECK(v.kind == KIND_Object);
    if (i < 0 || i >= v.objCount) return StringRef();
    return v.objMembers[i].key;
}

const Value* ObjectValueAt(const Value& v, i32 i) {
    VESPUCCI_CHECK(v.kind == KIND_Object);
    if (i < 0 || i >= v.objCount) return 0;
    return v.objMembers[i].value;
}

i64 OptInt(const Value* v, const char* key, i64 defVal) {
    if (!v || v->kind != KIND_Object) return defVal;
    const Value* m = ObjectGet(*v, key);
    if (!m) return defVal;
    if (m->kind == KIND_Int) return m->scalar.i;
    if (m->kind == KIND_Double) return (i64)m->scalar.d;
    return defVal;
}

f64 OptDouble(const Value* v, const char* key, f64 defVal) {
    if (!v || v->kind != KIND_Object) return defVal;
    const Value* m = ObjectGet(*v, key);
    if (!m) return defVal;
    if (m->kind == KIND_Double) return m->scalar.d;
    if (m->kind == KIND_Int) return (f64)m->scalar.i;
    return defVal;
}

bool OptBool(const Value* v, const char* key, bool defVal) {
    if (!v || v->kind != KIND_Object) return defVal;
    const Value* m = ObjectGet(*v, key);
    if (!m || m->kind != KIND_Bool) return defVal;
    return m->scalar.b;
}

StringRef OptString(const Value* v, const char* key, const char* defVal) {
    if (!v || v->kind != KIND_Object) return StringRef(defVal);
    const Value* m = ObjectGet(*v, key);
    if (!m || m->kind != KIND_String) return StringRef(defVal);
    return m->str;
}

// ── Writer ────────────────────────────────────────────────────────────

Writer::Writer() : m_needComma(false), m_depth(0) {}

void Writer::emitComma() {
    if (m_needComma) m_buf.push_back(',');
    m_needComma = true;
}

void Writer::emitEscaped(const char* s, usize n) {
    m_buf.push_back('"');
    for (usize i = 0; i < n; ++i) {
        char c = s[i];
        switch (c) {
            case '"':  m_buf += "\\\""; break;
            case '\\': m_buf += "\\\\"; break;
            case '\n': m_buf += "\\n"; break;
            case '\r': m_buf += "\\r"; break;
            case '\t': m_buf += "\\t"; break;
            case '\b': m_buf += "\\b"; break;
            case '\f': m_buf += "\\f"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char h[8];
                    std::snprintf(h, sizeof(h), "\\u%04x", (unsigned)(unsigned char)c);
                    m_buf += h;
                } else {
                    m_buf.push_back(c);
                }
                break;
        }
    }
    m_buf.push_back('"');
}

void Writer::beginObject() {
    emitComma(); m_buf.push_back('{'); m_needComma = false; m_depth++;
}

void Writer::endObject() {
    m_buf.push_back('}'); m_needComma = true; m_depth--;
}

void Writer::beginArray() {
    emitComma(); m_buf.push_back('['); m_needComma = false; m_depth++;
}

void Writer::endArray() {
    m_buf.push_back(']'); m_needComma = true; m_depth--;
}

void Writer::key(const char* k) {
    emitComma();
    emitEscaped(k, std::strlen(k));
    m_buf.push_back(':');
    m_needComma = false;
}

void Writer::writeNull()       { emitComma(); m_buf += "null"; }
void Writer::writeBool(bool b) { emitComma(); m_buf += b ? "true" : "false"; }
void Writer::writeInt(i64 v)   {
    emitComma();
    char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    m_buf += buf;
}
void Writer::writeDouble(f64 v) {
    emitComma();
    char buf[64]; std::snprintf(buf, sizeof(buf), "%.17g", v);
    m_buf += buf;
}
void Writer::writeString(const char* s) {
    emitComma();
    emitEscaped(s ? s : "", s ? std::strlen(s) : 0);
}
void Writer::writeString(const StringRef& s) {
    emitComma();
    emitEscaped(s.data(), s.size());
}

void Writer::kvNull(const char* k)             { key(k); writeNull(); }
void Writer::kvBool(const char* k, bool b)     { key(k); writeBool(b); }
void Writer::kvInt(const char* k, i64 v)       { key(k); writeInt(v); }
void Writer::kvDouble(const char* k, f64 v)    { key(k); writeDouble(v); }
void Writer::kvString(const char* k, const char* s) { key(k); writeString(s); }

} // namespace JsonLite
} // namespace Core
} // namespace Vespucci
