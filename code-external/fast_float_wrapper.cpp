#include "fast_float_wrapper.h"
#include "fast_float/fast_float.h"

namespace Vishwakarma {
    from_chars_result from_chars(const char* first, const char* last, double& value, chars_format fmt) {
        auto res = fast_float::from_chars(first, last, value, static_cast<fast_float::chars_format>(fmt));
        return { res.ptr, res.ec };
    }

    from_chars_result from_chars(const char* first, const char* last, float& value, chars_format fmt) {
        auto res = fast_float::from_chars(first, last, value, static_cast<fast_float::chars_format>(fmt));
        return { res.ptr, res.ec };
    }
}
