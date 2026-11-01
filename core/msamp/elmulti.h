// core/msamp/elmulti.h - Elektron's .elmulti (Tonverk multisample mapping), read into the SFZ reader's region model
// (BACKLOG 7q). A TOML-like text file: `version`, `name`, then [[key-zones]] (pitch, key-center), each with
// [[key-zones.velocity-layers]] (velocity 0..1, strategy) and each of those [[...sample-slots]] (sample, loop-mode,
// loop-start, loop-end, loop-crossfade, keep-looping-on-release, trim-start, trim-end). Written from the public,
// reverse-engineered description (github.com/asatamax/tonverk-elmulti-converter docs/ELMULTI_FORMAT_SPEC.md, ISC) and
// the example posted on the Elektronauts forum; no code copied. NOT verified against a file Tonverk wrote itself.
//
//   key ranges   not stored: each zone's pitch is a root, the keys between two roots go to the nearer one (the middle
//                key of an odd gap to the lower), the first zone down to 0, the last up to 127.
//   velocity     a layer's `velocity` x 127 (rounded) is its lower bound, the next higher layer's the end of it; the
//                lowest layer takes everything below.
//   slots        the slots of one layer are round robin (seq_length / seq_position); samples are relative to the file's
//                own folder.
//   loops        Forward = loop_sustain, Forward + keep-looping-on-release = loop_continuous, Off = no_loop, none =
//                one_shot; loop-start / loop-end as frames, loop-crossfade ignored (counted).
//   trim         trim-start = offset, trim-end - 1 = end (taken as an exclusive end).
//
// parse() (sfz.h) hands text that holds a [[key-zones table to parse_elmulti, so every caller of the .sfz reader (the
// MULTISAMPLE load, the pools, the bundles) reads both.
#pragma once
#include "sfz.h"

namespace core {
namespace msamp {

// The text is an .elmulti: it has a [[key-zones...]] table header at the start of a line
bool is_elmulti(const char* text, int len);
// As parse() (sfz.h): regions and samples into d, False (d->err says why) when no region is left
bool parse_elmulti(const char* text, int len, const char* dir, Desc* d);

}  // namespace msamp
}  // namespace core
