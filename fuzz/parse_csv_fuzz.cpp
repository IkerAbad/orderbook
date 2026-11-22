#include "orderbook/trace.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    const std::string_view text(reinterpret_cast<const char*>(data), size);
    try {
        (void)ob::parse_csv(text);
    } catch (const std::exception&) {
        // Malformed traces are the expected input.
    }
    return 0;
}
