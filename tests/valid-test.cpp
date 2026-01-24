#include <cstdio>

#include <nlohmann/json.hpp>
#include <validate.hpp>

int main()
{
    using namespace buxtehude;

    json j1 = {
        { "Dietrich", "Buxtehude" },
        { "famous", true },
        { "instrument", "organ" },
        { "year", 1637 }
    };

    assert(ValidateJSON(j1, {
        { "/Dietrich"_json_pointer, predicates::Compare("Buxtehude") },
        { "/famous"_json_pointer, predicates::IsBool },
    }) == true);

    assert(ValidateJSON(j1, {
        { "/operas"_json_pointer, predicates::Exists }
    }) == false);

    assert(ValidateJSON(j1, {
        { "/instrument"_json_pointer,
          predicates::Matches({"viola da gamba", "organ", "lute"}) }
    }) == true);

    assert(ValidateJSON(j1, {
        { "/year"_json_pointer, predicates::GreaterEq<1685> }
    }) == false);

    assert(ValidateJSON(j1, {
        { "/year"_json_pointer, [] (const json& j) { return j > 1600; } }
    }) == true);

    assert(ValidateJSON(j1, {
        { "/famous"_json_pointer, predicates::Inverse(predicates::Compare(true)) }
    }) == false);

    printf("Test (%s) completed successfully\n", __FILE__);

    return 0;
}
