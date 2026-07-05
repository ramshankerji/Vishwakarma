#pragma once
#include <system_error>

namespace Vishwakarma {
    enum class chars_format {
        scientific = 1,
        fixed = 2,
        hex = 4,
        general = fixed | scientific
    };

    struct from_chars_result {
        const char* ptr;
        std::errc ec;
        operator bool() const noexcept { return ec == std::errc{}; }
    };

    from_chars_result from_chars(const char* first, const char* last, double& value, chars_format fmt = chars_format::general);
    from_chars_result from_chars(const char* first, const char* last, float& value, chars_format fmt = chars_format::general);
}
