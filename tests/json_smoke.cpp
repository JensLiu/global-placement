#include <boost/json.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

int main() {
  namespace json = boost::json;
  try {
    // Generic JSON support only: this is not a placement trace schema.
    const json::object original{
        {"name", "cell\"\\\n"},
        {"signed", std::int64_t{-42}},
        {"unsigned", std::uint64_t{18446744073709551615ULL}},
        {"coordinate", 12.375},
        {"enabled", true},
        {"missing", nullptr},
        {"values", json::array{1, 2, 3}},
        {"metadata", json::object{{"units", "dbu"}}}};

    const std::string encoded = json::serialize(original);
    const json::value decoded = json::parse(encoded);
    const auto &object = decoded.as_object();
    if (decoded != json::value(original) ||
        object.at("signed").as_int64() != -42 ||
        object.at("unsigned").as_uint64() != 18446744073709551615ULL ||
        object.at("coordinate").as_double() != 12.375 ||
        !object.at("enabled").as_bool() ||
        !object.at("missing").is_null()) {
      std::cerr << "JSON round-trip failed\n";
      return 1;
    }

    boost::system::error_code error;
    (void)json::parse("{invalid json", error);
    if (!error) {
      std::cerr << "Malformed JSON was accepted\n";
      return 1;
    }
    std::cout << "Boost.JSON serialization/parsing checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "JSON check failed: " << error.what() << '\n';
    return 1;
  }
}
