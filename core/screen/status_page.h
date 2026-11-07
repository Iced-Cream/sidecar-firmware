// core/screen/status_page.h - the generic engine's one page of its own on the Rytm's screen (2026-11-07, the screen
// service's return as a companion service): a read-only status page, the client core/screen draws while the Rytm's page
// is open (TRK + YES on the Rytm). It names no Rytm mod: the link, the project as the companion's store link names it,
// the step clock, the record store and locks, the route and DSP slots, the network, the uptime.
// Keys: only NO is the page's (it closes the page); every other key and every encoder stays the Rytm's.
#pragma once
#include <stdint.h>

#include "screen.h"

namespace core {
namespace screen {
namespace status_page {

const Client* client();          // set it with screen::set_client (bridge.cpp)
void set_address(const char* ip);   // the network's address ("" none), as the connect splash gets it
void tick(uint32_t ms);          // while the page is open: a redraw every kRefreshMs (the counts move)
constexpr uint32_t kRefreshMs = 500;

}  // namespace status_page
}  // namespace screen
}  // namespace core
