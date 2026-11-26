// pi/ui/host/allow_shim.cpp - the remote UI service's command allowlist (pi/engine/remote_srv.cpp) for shot.c: every seqed= / seq= line the SEQ page
// sends in the host tests must be one the Pi's service lets through. Linked into ./shot only (never into the remote app: the objects are named test_allow_*).
#include "../../engine/remote_srv.h"

extern "C" int rmt_cmd_allowed (const char *line)
{
	return remote::CommandAllowed (line) ? 1 : 0;
}
