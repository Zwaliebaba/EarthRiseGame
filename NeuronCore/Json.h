#pragma once

// Json.h — a small, dependency-free JSON reader for NeuronCore.
//
// Purpose: the server and client read every secret/tunable from a JSON config
// file (see ERServer/ServerConfig.h, NeuronClient/ClientConfig.h) instead of the
// process environment. This is the minimal parser those loaders sit on top of.
//
// Design notes:
//   * Header-only and platform-independent — it builds under MSVC (pulled into
//     NeuronCore via NeuronCore.vcxitems) and under g++ -std=c++23 -Wpedantic
//     (the Linux testrunner exercises it in JsonParseTests.cpp), with no Win32
//     or third-party dependency.
//   * No exceptions: Parse()/ParseFile() report failure through a bool + an
//     optional error string, matching the project's "no exceptions in the
//     common path" posture. Accessors are total — a wrong-typed or missing
//     value yields the caller-supplied default rather than throwing.
//   * Objects preserve insertion order and are stored as a flat vector of
//     key/value pairs. Config documents are tiny, so linear lookup is simpler
//     and perfectly adequate; no std::map ordering surprises.
//
// Supported: the full JSON grammar (objects, arrays, strings with \uXXXX +
// surrogate pairs, numbers, true/false/null). Numbers are kept as double and
// surfaced as bool/int64/uint64/double on demand.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Neuron::Json
{

enum class Type : uint8_t
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

class Value
{
public:
    Value() = default;

    [[nodiscard]] Type type() const noexcept { return m_type; }
    [[nodiscard]] bool isNull()   const noexcept { return m_type == Type::Null; }
    [[nodiscard]] bool isBool()   const noexcept { return m_type == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return m_type == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return m_type == Type::String; }
    [[nodiscard]] bool isArray()  const noexcept { return m_type == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return m_type == Type::Object; }

    // -- scalar reads (return the fallback when this value is the wrong type) ----
    [[nodiscard]] bool asBool(bool fallback = false) const noexcept
    {
        return m_type == Type::Bool ? m_bool : fallback;
    }
    [[nodiscard]] double asNumber(double fallback = 0.0) const noexcept
    {
        return m_type == Type::Number ? m_number : fallback;
    }
    [[nodiscard]] int64_t asInt(int64_t fallback = 0) const noexcept
    {
        return m_type == Type::Number ? static_cast<int64_t>(m_number) : fallback;
    }
    [[nodiscard]] uint64_t asUint(uint64_t fallback = 0) const noexcept
    {
        return m_type == Type::Number ? static_cast<uint64_t>(m_number) : fallback;
    }
    [[nodiscard]] const std::string& asString(const std::string& fallback = emptyString()) const noexcept
    {
        return m_type == Type::String ? m_string : fallback;
    }

    // -- array access -----------------------------------------------------------
    [[nodiscard]] size_t size() const noexcept
    {
        return m_type == Type::Array ? m_array.size() : 0;
    }
    [[nodiscard]] const Value& at(size_t index) const noexcept
    {
        return (m_type == Type::Array && index < m_array.size()) ? m_array[index] : nullValue();
    }
    [[nodiscard]] const std::vector<Value>& items() const noexcept
    {
        return m_type == Type::Array ? m_array : emptyArray();
    }

    // -- object access ----------------------------------------------------------
    [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }

    // Returns the member, or a Null value if this is not an object / the key is
    // absent. Lets callers chain reads (e.g. cfg["database"]["user"].asString()).
    [[nodiscard]] const Value& operator[](std::string_view key) const noexcept
    {
        const Value* v = find(key);
        return v ? *v : nullValue();
    }

    [[nodiscard]] const Value* find(std::string_view key) const noexcept
    {
        if (m_type != Type::Object)
            return nullptr;
        for (const auto& [k, v] : m_object)
            if (k == key)
                return &v;
        return nullptr;
    }

    // Typed object-member convenience reads used by the config loaders.
    [[nodiscard]] std::string getString(std::string_view key, const std::string& fallback = {}) const
    {
        const Value* v = find(key);
        return (v && v->isString()) ? v->m_string : fallback;
    }
    [[nodiscard]] bool getBool(std::string_view key, bool fallback = false) const noexcept
    {
        const Value* v = find(key);
        return (v && v->isBool()) ? v->m_bool : fallback;
    }
    [[nodiscard]] uint32_t getUint32(std::string_view key, uint32_t fallback) const noexcept
    {
        const Value* v = find(key);
        return (v && v->isNumber()) ? static_cast<uint32_t>(v->m_number) : fallback;
    }
    [[nodiscard]] uint64_t getUint64(std::string_view key, uint64_t fallback) const noexcept
    {
        const Value* v = find(key);
        return (v && v->isNumber()) ? static_cast<uint64_t>(v->m_number) : fallback;
    }
    [[nodiscard]] int getInt(std::string_view key, int fallback) const noexcept
    {
        const Value* v = find(key);
        return (v && v->isNumber()) ? static_cast<int>(v->m_number) : fallback;
    }

    // Builders (used by the parser; handy in tests too).
    static Value makeNull()              { return Value{}; }
    static Value makeBool(bool b)        { Value v; v.m_type = Type::Bool;   v.m_bool = b; return v; }
    static Value makeNumber(double n)    { Value v; v.m_type = Type::Number; v.m_number = n; return v; }
    static Value makeString(std::string s){ Value v; v.m_type = Type::String; v.m_string = std::move(s); return v; }
    static Value makeArray()             { Value v; v.m_type = Type::Array; return v; }
    static Value makeObject()            { Value v; v.m_type = Type::Object; return v; }

    void pushBack(Value v) { m_array.push_back(std::move(v)); }
    void set(std::string key, Value v) { m_object.emplace_back(std::move(key), std::move(v)); }

private:
    static const Value& nullValue() noexcept
    {
        static const Value kNull{};
        return kNull;
    }
    static const std::string& emptyString() noexcept
    {
        static const std::string kEmpty{};
        return kEmpty;
    }
    static const std::vector<Value>& emptyArray() noexcept
    {
        static const std::vector<Value> kEmpty{};
        return kEmpty;
    }

    Type        m_type{ Type::Null };
    bool        m_bool{ false };
    double      m_number{ 0.0 };
    std::string m_string;
    std::vector<Value>                          m_array;
    std::vector<std::pair<std::string, Value>>  m_object;
};

namespace detail
{

// Single-pass recursive-descent parser over a string_view. All failure paths
// funnel through fail(), which records a line:column message and returns false.
class Parser
{
public:
    Parser(std::string_view text, std::string* error) : m_text(text), m_error(error) {}

    bool parse(Value& out)
    {
        skipWhitespace();
        if (!parseValue(out))
            return false;
        skipWhitespace();
        if (m_pos != m_text.size())
            return fail("trailing characters after top-level value");
        return true;
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_text.size(); }
    [[nodiscard]] char peek() const noexcept { return m_text[m_pos]; }

    bool fail(const std::string& message)
    {
        if (m_error)
        {
            // Compute a 1-based line/column for the current position.
            size_t line = 1, col = 1;
            for (size_t i = 0; i < m_pos && i < m_text.size(); ++i)
            {
                if (m_text[i] == '\n') { ++line; col = 1; }
                else                    { ++col; }
            }
            std::ostringstream os;
            os << "JSON parse error at line " << line << ", column " << col << ": " << message;
            *m_error = os.str();
        }
        return false;
    }

    void skipWhitespace() noexcept
    {
        while (!atEnd())
        {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++m_pos;
            else
                break;
        }
    }

    bool parseValue(Value& out)
    {
        if (atEnd())
            return fail("unexpected end of input");
        switch (peek())
        {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': { std::string s; if (!parseString(s)) return false; out = Value::makeString(std::move(s)); return true; }
            case 't': case 'f': return parseBool(out);
            case 'n': return parseNull(out);
            default:  return parseNumber(out);
        }
    }

    bool parseObject(Value& out)
    {
        ++m_pos; // consume '{'
        out = Value::makeObject();
        skipWhitespace();
        if (!atEnd() && peek() == '}') { ++m_pos; return true; }
        for (;;)
        {
            skipWhitespace();
            if (atEnd() || peek() != '"')
                return fail("expected string key in object");
            std::string key;
            if (!parseString(key))
                return false;
            skipWhitespace();
            if (atEnd() || peek() != ':')
                return fail("expected ':' after object key");
            ++m_pos; // consume ':'
            skipWhitespace();
            Value child;
            if (!parseValue(child))
                return false;
            out.set(std::move(key), std::move(child));
            skipWhitespace();
            if (atEnd())
                return fail("unterminated object");
            const char c = peek();
            if (c == ',') { ++m_pos; continue; }
            if (c == '}') { ++m_pos; return true; }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(Value& out)
    {
        ++m_pos; // consume '['
        out = Value::makeArray();
        skipWhitespace();
        if (!atEnd() && peek() == ']') { ++m_pos; return true; }
        for (;;)
        {
            skipWhitespace();
            Value child;
            if (!parseValue(child))
                return false;
            out.pushBack(std::move(child));
            skipWhitespace();
            if (atEnd())
                return fail("unterminated array");
            const char c = peek();
            if (c == ',') { ++m_pos; continue; }
            if (c == ']') { ++m_pos; return true; }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parseString(std::string& out)
    {
        ++m_pos; // consume opening '"'
        out.clear();
        for (;;)
        {
            if (atEnd())
                return fail("unterminated string");
            const char c = m_text[m_pos++];
            if (c == '"')
                return true;
            if (c == '\\')
            {
                if (atEnd())
                    return fail("unterminated escape in string");
                const char esc = m_text[m_pos++];
                switch (esc)
                {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u':  if (!parseUnicodeEscape(out)) return false; break;
                    default:   return fail("invalid escape sequence");
                }
            }
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                return fail("unescaped control character in string");
            }
            else
            {
                out.push_back(c);
            }
        }
    }

    // Parses the four hex digits following a "\u", handling UTF-16 surrogate
    // pairs, and appends the UTF-8 encoding to 'out'.
    bool parseUnicodeEscape(std::string& out)
    {
        uint32_t cp = 0;
        if (!readHex4(cp))
            return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) // high surrogate — expect a low surrogate
        {
            if (m_pos + 1 >= m_text.size() || m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u')
                return fail("missing low surrogate after high surrogate");
            m_pos += 2; // consume "\u"
            uint32_t low = 0;
            if (!readHex4(low))
                return false;
            if (low < 0xDC00 || low > 0xDFFF)
                return fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            return fail("unexpected low surrogate");
        }
        appendUtf8(cp, out);
        return true;
    }

    bool readHex4(uint32_t& out)
    {
        if (m_pos + 4 > m_text.size())
            return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = m_text[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else                            return fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    static void appendUtf8(uint32_t cp, std::string& out)
    {
        if (cp <= 0x7F)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseBool(Value& out)
    {
        if (m_text.compare(m_pos, 4, "true") == 0)  { m_pos += 4; out = Value::makeBool(true);  return true; }
        if (m_text.compare(m_pos, 5, "false") == 0) { m_pos += 5; out = Value::makeBool(false); return true; }
        return fail("invalid literal (expected true/false)");
    }

    bool parseNull(Value& out)
    {
        if (m_text.compare(m_pos, 4, "null") == 0) { m_pos += 4; out = Value::makeNull(); return true; }
        return fail("invalid literal (expected null)");
    }

    bool parseNumber(Value& out)
    {
        const size_t start = m_pos;
        if (!atEnd() && peek() == '-')
            ++m_pos;
        if (atEnd() || ((peek() < '0' || peek() > '9')))
            return fail("invalid number");
        while (!atEnd() && peek() >= '0' && peek() <= '9') ++m_pos;
        if (!atEnd() && peek() == '.')
        {
            ++m_pos;
            if (atEnd() || peek() < '0' || peek() > '9')
                return fail("invalid number (no digits after decimal point)");
            while (!atEnd() && peek() >= '0' && peek() <= '9') ++m_pos;
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E'))
        {
            ++m_pos;
            if (!atEnd() && (peek() == '+' || peek() == '-')) ++m_pos;
            if (atEnd() || peek() < '0' || peek() > '9')
                return fail("invalid number (no digits in exponent)");
            while (!atEnd() && peek() >= '0' && peek() <= '9') ++m_pos;
        }
        const std::string token(m_text.substr(start, m_pos - start));
        out = Value::makeNumber(std::strtod(token.c_str(), nullptr));
        return true;
    }

    std::string_view m_text;
    size_t           m_pos{ 0 };
    std::string*     m_error{ nullptr };
};

} // namespace detail

// Parse a JSON document from memory. Returns true and fills 'out' on success;
// on failure returns false and (if provided) writes a line:column diagnostic.
inline bool Parse(std::string_view text, Value& out, std::string* error = nullptr)
{
    detail::Parser parser(text, error);
    return parser.parse(out);
}

// Read and parse a JSON file. Returns false (with a diagnostic) if the file
// cannot be opened or does not parse.
inline bool ParseFile(const std::string& path, Value& out, std::string* error = nullptr)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        if (error)
            *error = "could not open config file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string contents = ss.str();
    return Parse(contents, out, error);
}

} // namespace Neuron::Json
