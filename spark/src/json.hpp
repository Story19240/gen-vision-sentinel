#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace spark {

class JsonError : public std::runtime_error {
public:
    JsonError(const std::string& message, std::size_t line, std::size_t column);

    std::size_t line() const noexcept;
    std::size_t column() const noexcept;

private:
    std::size_t line_;
    std::size_t column_;
};

class JsonValue {
public:
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    struct Member;
    JsonValue();
    JsonValue(const JsonValue& other);
    JsonValue(JsonValue&& other) noexcept;
    JsonValue& operator=(const JsonValue& other);
    JsonValue& operator=(JsonValue&& other) noexcept;
    ~JsonValue();
    static JsonValue null_value();
    static JsonValue boolean(bool value);
    static JsonValue number(double value);
    static JsonValue integer(std::int64_t value);
    static JsonValue string(std::string value);
    static JsonValue array(std::vector<JsonValue> values);
    static JsonValue object(std::vector<Member> members);

    Type type() const noexcept;
    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_number() const noexcept;
    bool is_integer() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool as_bool() const;
    double as_double() const;
    std::int64_t as_int64() const;
    const std::string& as_string() const;
    const std::vector<JsonValue>& as_array() const;
    const std::vector<Member>& as_object() const;
    std::vector<Member>& as_object();

    const JsonValue* find(const std::string& key) const;
    JsonValue* find(const std::string& key);
    bool contains(const std::string& key) const;

private:
    Type type_;
    bool boolean_;
    double number_;
    bool integer_;
    std::int64_t integer_value_;
    std::string string_;
    std::unique_ptr<std::vector<JsonValue>> array_;
    std::unique_ptr<std::vector<Member>> object_;
};

struct JsonValue::Member {
    std::string key;
    JsonValue value;

    Member(std::string member_key, JsonValue member_value);
};

JsonValue parse_json(const std::string& text);
JsonValue load_json_file(const std::string& path);

const char* json_type_name(JsonValue::Type type) noexcept;
std::string json_escape(const std::string& value);
std::string json_dump(const JsonValue& value, int indent = 2);

}  // namespace spark
