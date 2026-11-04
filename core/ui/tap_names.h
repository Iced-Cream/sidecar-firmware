// core/ui/tap_names.h - the scope's sources by name and the voices' IN channels (page_scope.cpp), on their own: no
// other header, so the Pi's kernel sources (pi/engine/rec_pi.cpp) can include it where ui/extra.h's send_fx.h and its
// <new> would not build. ui/extra.h includes it, so a change here and in page_scope.cpp stays one declaration.
#pragma once

namespace core {
namespace ui {

const char* tap_source_name(int src);   // "MAIN" "BD" "SD" "RS/CP" .. "CY/CB" (track order) "IN L" "IN R"
int tap_channel_of_voice(int vi);       // the IN channel of Sidecar voice vi (0..7)

}  // namespace ui
}  // namespace core
