#include "orderbook/version.hpp"

static_assert(ORDERBOOK_VERSION_MAJOR >= 0);
static_assert(ORDERBOOK_VERSION_MINOR >= 0);
static_assert(ORDERBOOK_VERSION_PATCH >= 0);

void orderbook_header_version() {}
