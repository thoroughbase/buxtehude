#include "validate.hpp"

namespace buxtehude {

auto ValidateJSON(const json& j, const ValidationSeries& tests) -> bool
{
    for (auto& [ptr, pred] : tests) {
        if (!j.contains(ptr)) return false;
        if (!pred) continue;
        if (!pred(j[ptr])) return false;
    }

    return true;
}

}
