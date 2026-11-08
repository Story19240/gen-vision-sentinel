#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace spark {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    JsonValue parse() {
        skip_whitespace();
        JsonValue parsed_value = parse_value();
        skip_whitespace();
        if (position_ != text_.size()) {
            fail("unexpected trailing content");
        }
        return parsed_value;
    }

private:
    void fail(const std::string& message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < position_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw JsonError(message, line, column);
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    JsonValue parse_value() {
        if (position_ >= text_.size()) fail("expected a JSON value");
        const char c = text_[position_];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return JsonValue::string(parse_string());
        if (c == 't') {
            parse_literal("true");
            return JsonValue::boolean(true);
        }
        if (c == 'f') {
            parse_literal("false");
            return JsonValue::boolean(false);
        }
        if (c == 'n') {
            parse_literal("null");
            return JsonValue::null_value();
        }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail("expected a JSON value");
        return JsonValue();
    }

    JsonValue parse_object() {
        expect('{');
        std::vector<JsonValue::Member> members;
        skip_whitespace();
        if (consume('}')) return JsonValue::object(std::move(members));
        while (true) {
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            members.push_back(JsonValue::Member{std::move(key), parse_value()});
            skip_whitespace();
            if (consume('}')) break;
            expect(',');
        }
        return JsonValue::object(std::move(members));
    }

    JsonValue parse_array() {
        expect('[');
        std::vector<JsonValue> values;
        skip_whitespace();
        if (consume(']')) return JsonValue::array(std::move(values));
        while (true) {
            skip_whitespace();
            values.push_back(parse_value());
            skip_whitespace();
            if (consume(']')) break;
            expect(',');
        }
        return JsonValue::array(std::move(values));
    }

    JsonValue parse_number() {
        const std::size_t start = position_;
        consume('-');
        if (position_ >= text_.size()) fail("incomplete number");
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && is_digit(text_[position_])) {
                fail("leading zero in number");
            }
        } else if (is_digit_1_to_9(text_[position_])) {
            while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
        } else {
            fail("invalid number");
        }

        bool is_integer = true;
        if (position_ < text_.size() && text_[position_] == '.') {
            is_integer = false;
            ++position_;
            if (position_ >= text_.size() || !is_digit(text_[position_])) {
                fail("fraction requires at least one digit");
            }
            while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
        }

        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            is_integer = false;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= text_.size() || !is_digit(text_[position_])) {
                fail("exponent requires at least one digit");
            }
            while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
        }

        const std::string token = text_.substr(start, position_ - start);
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
            fail("number is out of range");
        }

        if (is_integer) {
            errno = 0;
            const long long integer_value = std::strtoll(token.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && errno != ERANGE) {
                return JsonValue::integer(static_cast<std::int64_t>(integer_value));
            }
        }
        return JsonValue::number(value);
    }

    std::string parse_string() {
        expect('"');
        std::string output;
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return output;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }

            if (position_ >= text_.size()) fail("unterminated escape sequence");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': append_utf8(output, parse_unicode_escape()); break;
                default: fail("invalid string escape"); break;
            }
        }
        fail("unterminated string");
        return output;
    }

    std::uint32_t parse_unicode_escape() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (position_ >= text_.size()) fail("incomplete Unicode escape");
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("invalid Unicode escape");
        }
        if (value >= 0xD800 && value <= 0xDBFF) {
            if (position_ + 6 > text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
                fail("high surrogate must be followed by a low surrogate");
            }
            position_ += 2;
            const std::uint32_t low = parse_unicode_escape();
            if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate");
            return 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
        }
        if (value >= 0xDC00 && value <= 0xDFFF) {
            fail("unexpected low surrogate");
        }
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t code_point) {
        if (code_point <= 0x7F) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0x10FFFF) {
            output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            throw JsonError("invalid Unicode code point", 0, 0);
        }
    }

    void parse_literal(const char* literal) {
        while (*literal != '\0') {
            if (position_ >= text_.size() || text_[position_] != *literal) {
                fail("invalid literal");
            }
            ++position_;
            ++literal;
        }
    }

    static bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }

    static bool is_digit_1_to_9(char c) {
        return c >= '1' && c <= '9';
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

void append_indent(std::string& output, int depth, int indent) {
    for (int i = 0; i < depth * indent; ++i) output.push_back(' ');
}

void dump_value(
    const JsonValue& value,
    std::string& output,
    int indent,
    int depth
) {
    switch (value.type()) {
        case JsonValue::Type::Null:
            output += "null";
            return;
        case JsonValue::Type::Boolean:
            output += value.as_bool() ? "true" : "false";
            return;
        case JsonValue::Type::Number: {
            std::ostringstream stream;
            stream << std::setprecision(std::numeric_limits<double>::max_digits10)
                   << value.as_double();
            output += stream.str();
            return;
        }
        case JsonValue::Type::String:
            output += json_escape(value.as_string());
            return;
        case JsonValue::Type::Array: {
            output.push_back('[');
            const auto& values = value.as_array();
            if (values.empty()) {
                output.push_back(']');
                return;
            }
            if (indent > 0) output.push_back('\n');
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (indent > 0) append_indent(output, depth + 1, indent);
                dump_value(values[i], output, indent, depth + 1);
                if (i + 1 != values.size()) output.push_back(',');
                if (indent > 0) output.push_back('\n');
            }
            if (indent > 0) append_indent(output, depth, indent);
            output.push_back(']');
            return;
        }
        case JsonValue::Type::Object: {
            output.push_back('{');
            const auto& members = value.as_object();
            if (members.empty()) {
                output.push_back('}');
                return;
            }
            if (indent > 0) output.push_back('\n');
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (indent > 0) append_indent(output, depth + 1, indent);
                output += json_escape(members[i].key);
                output += indent > 0 ? ": " : ":";
                dump_value(members[i].value, output, indent, depth + 1);
                if (i + 1 != members.size()) output.push_back(',');
                if (indent > 0) output.push_back('\n');
            }
            if (indent > 0) append_indent(output, depth, indent);
            output.push_back('}');
            return;
        }
    }
}

}  // namespace

JsonError::JsonError(const std::string& message, std::size_t line, std::size_t column)
    : std::runtime_error("json:" + std::to_string(line) + ":" + std::to_string(column) + ": " + message),
      line_(line),
      column_(column) {}

std::size_t JsonError::line() const noexcept { return line_; }
std::size_t JsonError::column() const noexcept { return column_; }

JsonValue::Member::Member(std::string member_key, JsonValue member_value)
    : key(std::move(member_key)), value(std::move(member_value)) {}

JsonValue::JsonValue()
    : type_(Type::Null),
      boolean_(false),
      number_(0.0),
      integer_(false),
      integer_value_(0),
      array_(new std::vector<JsonValue>()),
      object_(new std::vector<Member>()) {}

JsonValue::JsonValue(const JsonValue& other)
    : type_(other.type_),
      boolean_(other.boolean_),
      number_(other.number_),
      integer_(other.integer_),
      integer_value_(other.integer_value_),
      string_(other.string_),
      array_(new std::vector<JsonValue>(*other.array_)),
      object_(new std::vector<Member>(*other.object_)) {}

JsonValue::JsonValue(JsonValue&& other) noexcept
    : type_(other.type_),
      boolean_(other.boolean_),
      number_(other.number_),
      integer_(other.integer_),
      integer_value_(other.integer_value_),
      string_(std::move(other.string_)),
      array_(std::move(other.array_)),
      object_(std::move(other.object_)) {
    other.type_ = Type::Null;
    other.boolean_ = false;
    other.number_ = 0.0;
    other.integer_ = false;
    other.integer_value_ = 0;
    other.array_.reset(new std::vector<JsonValue>());
    other.object_.reset(new std::vector<Member>());
}

JsonValue& JsonValue::operator=(const JsonValue& other) {
    if (this == &other) return *this;
    type_ = other.type_;
    boolean_ = other.boolean_;
    number_ = other.number_;
    integer_ = other.integer_;
    integer_value_ = other.integer_value_;
    string_ = other.string_;
    array_.reset(new std::vector<JsonValue>(*other.array_));
    object_.reset(new std::vector<Member>(*other.object_));
    return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
    if (this == &other) return *this;
    type_ = other.type_;
    boolean_ = other.boolean_;
    number_ = other.number_;
    integer_ = other.integer_;
    integer_value_ = other.integer_value_;
    string_ = std::move(other.string_);
    array_ = std::move(other.array_);
    object_ = std::move(other.object_);
    other.type_ = Type::Null;
    other.boolean_ = false;
    other.number_ = 0.0;
    other.integer_ = false;
    other.integer_value_ = 0;
    other.array_.reset(new std::vector<JsonValue>());
    other.object_.reset(new std::vector<Member>());
    return *this;
}

JsonValue::~JsonValue() = default;

JsonValue JsonValue::null_value() { return JsonValue(); }

JsonValue JsonValue::boolean(bool value) {
    JsonValue json;
    json.type_ = Type::Boolean;
    json.boolean_ = value;
    return json;
}

JsonValue JsonValue::number(double value) {
    JsonValue json;
    json.type_ = Type::Number;
    json.number_ = value;
    json.integer_ = std::isfinite(value) && std::floor(value) == value &&
                    value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                    value <= static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (json.integer_) json.integer_value_ = static_cast<std::int64_t>(value);
    return json;
}

JsonValue JsonValue::integer(std::int64_t value) {
    JsonValue json;
    json.type_ = Type::Number;
    json.number_ = static_cast<double>(value);
    json.integer_ = true;
    json.integer_value_ = value;
    return json;
}

JsonValue JsonValue::string(std::string value) {
    JsonValue json;
    json.type_ = Type::String;
    json.string_ = std::move(value);
    return json;
}

JsonValue JsonValue::array(std::vector<JsonValue> values) {
    JsonValue json;
    json.type_ = Type::Array;
    json.array_.reset(new std::vector<JsonValue>(std::move(values)));
    return json;
}

JsonValue JsonValue::object(std::vector<Member> members) {
    JsonValue json;
    json.type_ = Type::Object;
    json.object_.reset(new std::vector<Member>(std::move(members)));
    return json;
}

JsonValue::Type JsonValue::type() const noexcept { return type_; }
bool JsonValue::is_null() const noexcept { return type_ == Type::Null; }
bool JsonValue::is_boolean() const noexcept { return type_ == Type::Boolean; }
bool JsonValue::is_number() const noexcept { return type_ == Type::Number; }
bool JsonValue::is_integer() const noexcept { return type_ == Type::Number && integer_; }
bool JsonValue::is_string() const noexcept { return type_ == Type::String; }
bool JsonValue::is_array() const noexcept { return type_ == Type::Array; }
bool JsonValue::is_object() const noexcept { return type_ == Type::Object; }

bool JsonValue::as_bool() const {
    if (type_ != Type::Boolean) throw JsonError("expected boolean", 0, 0);
    return boolean_;
}

double JsonValue::as_double() const {
    if (type_ != Type::Number) throw JsonError("expected number", 0, 0);
    return number_;
}

std::int64_t JsonValue::as_int64() const {
    if (!integer_) throw JsonError("expected integer", 0, 0);
    return integer_value_;
}

const std::string& JsonValue::as_string() const {
    if (type_ != Type::String) throw JsonError("expected string", 0, 0);
    return string_;
}

const std::vector<JsonValue>& JsonValue::as_array() const {
    if (type_ != Type::Array) throw JsonError("expected array", 0, 0);
    return *array_;
}

const std::vector<JsonValue::Member>& JsonValue::as_object() const {
    if (type_ != Type::Object) throw JsonError("expected object", 0, 0);
    return *object_;
}

std::vector<JsonValue::Member>& JsonValue::as_object() {
    if (type_ != Type::Object) throw JsonError("expected object", 0, 0);
    return *object_;
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& member : *object_) {
        if (member.key == key) return &member.value;
    }
    return nullptr;
}

JsonValue* JsonValue::find(const std::string& key) {
    if (type_ != Type::Object) return nullptr;
    for (auto& member : *object_) {
        if (member.key == key) return &member.value;
    }
    return nullptr;
}

bool JsonValue::contains(const std::string& key) const { return find(key) != nullptr; }

const char* json_type_name(JsonValue::Type type) noexcept {
    switch (type) {
        case JsonValue::Type::Null: return "null";
        case JsonValue::Type::Boolean: return "bool";
        case JsonValue::Type::Number: return "int/float";
        case JsonValue::Type::String: return "str";
        case JsonValue::Type::Array: return "list";
        case JsonValue::Type::Object: return "dict";
    }
    return "unknown";
}

std::string json_escape(const std::string& value) {
    std::string output;
    output.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(kHex[(c >> 4) & 0x0F]);
                    output.push_back(kHex[c & 0x0F]);
                } else {
                    output.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    output.push_back('"');
    return output;
}

std::string json_dump(const JsonValue& value, int indent) {
    std::string output;
    dump_value(value, output, indent, 0);
    return output;
}

JsonValue parse_json(const std::string& text) {
    return Parser(text).parse();
}

}  // namespace spark
