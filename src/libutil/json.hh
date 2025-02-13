#pragma once

#include "types.hh"
#include <cstdint>
#include <string>

namespace nix {
namespace json {
struct value
{};
}

json::value * nix_libutil_json_parse_from_file(const char * path, json::value * value);

json::value * nix_libutil_json_object_new();
json::value * nix_libutil_json_object_get(json::value * object, const char * key);
bool nix_libutil_json_object_set(json::value * object, const char * key, json::value * to_insert);
bool nix_libutil_json_object_set_integer(json::value * object, const char * key, int64_t to_insert);
bool nix_libutil_json_object_set_bool(json::value * object, const char * key, const bool to_insert);
bool nix_libutil_json_object_set_string(json::value * object, const char * key, const char * to_insert);
bool nix_libutil_json_object_set_strings(
    json::value * object, const char * key, const char * strings[], std::size_t size);
bool nix_libutil_json_object_update(json::value * object, json::value * other);

json::value * nix_libutil_json_string_new(const char * string_value);
const char * nix_libutil_json_string_get(const json::value * value);

json::value * nix_libutil_json_integer_new(const int64_t string_value);

json::value * nix_libutil_json_list_new();
void nix_libutil_json_list_insert(json::value * value, const struct json::value * to_insert);

void nix_libutil_json_free(json::value * value);

namespace json {
bool set_strings(json::value * object, const char * key, const Strings & strings)
{
    std::vector<const char *> charArrays;
    for (const std::string & str : strings) {
        charArrays.push_back(str.c_str());
    }

    const char ** result = new const char *[charArrays.size() + 1];
    for (size_t i = 0; i < charArrays.size(); ++i) {
        result[i] = charArrays[i];
    }
    result[charArrays.size()] = nullptr; // Null-terminate the array

    return nix_libutil_json_object_set_strings(object, key, result, charArrays.size() + 1);
}

json::value * from_string_map(const StringMap & strings)
{
    auto * object = nix_libutil_json_object_new();

    for (auto & [key, value] : strings) {
        nix_libutil_json_object_set_string(object, key.c_str(), value.c_str());
    }

    return object;
}

json::value * from_map(const std::map<std::string, json::value *> & strings)
{
    auto * object = nix_libutil_json_object_new();

    for (auto & [key, value] : strings) {
        nix_libutil_json_object_set(object, key.c_str(), value);
    }

    return object;
}
}

}
