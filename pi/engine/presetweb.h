// pi/engine/presetweb.h - the web page's Pool tab for the JUNO / 6-OP FM presets (www/index.html) over the preset index
// (core/presets/preset_index.h's view): one GET for a window of a folder, or of the project's list.
//
//   GET /presets?k=juno|fm[&f=<folder>][&o=<first row>][&n=<rows, at most 200>]
//     {"ready":true|false,"gen":<the index's walk>,"k":"juno|fm","f":<folder, -1 the project's list>,"o":<first>,"total",
//      "up":<parent folder, -1 a root>,"title","builtin","full","rows":[[name,flags],..],"kids":[folder or -1,..],
//      "msg":{"seq","text"}}   flags: 1 folder, 2 used (a list's slot with a preset); the list's rows are the slots PAT picks
//   not ready (the index is being walked): {"ready":false,...} without rows; f omitted: the project's list
//   An add is the console's "preset=juno|fm:add|set:<gen>:<folder>:<row>[:<at>]" (core/user/lists.h), queued by /cmd.
//
// Read only: web.cpp runs it in its tasks on core 0 while the index belongs to the control core (core 3); the copy is
// checked against the walk (a walk that starts or ends meanwhile: ready false, the page asks again).
// Portable: host-tested in tests/test_presetweb.cpp.
#ifndef _pi_engine_presetweb_h
#define _pi_engine_presetweb_h

namespace presetweb {

// params: the query string; the JSON into out; returns its length (0: cap too small)
int json(const char* params, char* out, int cap);

}  // namespace presetweb

#endif
