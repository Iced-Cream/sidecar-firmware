// core/tests/test_elmulti.cpp - BACKLOG 7q: Elektron's .elmulti read into the SFZ reader's regions (core/msamp/elmulti.*).
// SYNTHETIC fixtures only, written from the public reverse-engineered description (see elmulti.h): no file Tonverk
// wrote was available, so what the real files hold beyond that description (and the meaning of loop-end, trim-end,
// key-center fractions) is unverified. The fixtures follow the shapes of the spec's example and the forum's "4 Toys".
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "lib/pools.h"
#include "msamp/elmulti.h"
#include "msamp/sfz.h"

using namespace core;
using namespace core::msamp;

struct D {
  std::vector<Region> regions = std::vector<Region>(256);
  std::vector<char> samples = std::vector<char>(64 * msamp::kPath);
  Desc d{};
  D() {
    d.regions = regions.data(), d.max_regions = 256;
    d.samples = (char(*)[msamp::kPath])samples.data(), d.max_samples = 64;
  }
  // through parse(): the sfz entry point every caller uses must sniff the format
  bool parse(const std::string& t, const char* dir = "/MULTISAMPLE/Demo") {
    return msamp::parse(t.c_str(), (int)t.size(), dir, &d);
  }
  const char* path(int i) { return d.samples[d.regions[i].sample]; }
};

// the spec's example: a C2 zone with two velocity layers (the lower one a sustained loop with crossfade), a C3 one-shot
static const char kDemo[] =
    "# ELEKTRON MULTI-SAMPLE MAPPING FORMAT\n"
    "version = 0\n"
    "name = 'Demo Pad'\n"
    "\n"
    "[[key-zones]]\n"
    "pitch = 48\n"
    "key-center = 48.0\n"
    "\n"
    "[[key-zones.velocity-layers]]\n"
    "velocity = 0.078431375\n"
    "strategy = 'Forward'\n"
    "\n"
    "[[key-zones.velocity-layers.sample-slots]]\n"
    "sample = 'Demo Pad-000-048-c2.wav'\n"
    "loop-mode = 'Forward'\n"
    "loop-start = 24000\n"
    "loop-end = 96000\n"
    "loop-crossfade = 400\n"
    "keep-looping-on-release = true\n"
    "\n"
    "[[key-zones.velocity-layers]]\n"
    "velocity = 0.49411765\n"
    "strategy = 'Forward'\n"
    "\n"
    "[[key-zones.velocity-layers.sample-slots]]\n"
    "sample = 'Demo Pad-001-048-c2.wav'\n"
    "loop-mode = 'Off'\n"
    "\n"
    "[[key-zones]]\n"
    "pitch = 60\n"
    "key-center = 60.0\n"
    "\n"
    "[[key-zones.velocity-layers]]\n"
    "velocity = 0.49411765\n"
    "strategy = 'Forward'\n"
    "\n"
    "[[key-zones.velocity-layers.sample-slots]]\n"
    "sample = 'Demo Pad-000-060-c3.wav'\n"
    "loop-mode = 'Off'\n";

int main() {
  {  // the spec's example
    D p;
    CHECK(is_elmulti(kDemo, (int)strlen(kDemo)));
    CHECK(p.parse(kDemo));
    CHECK(p.d.nregions == 3 && p.d.nsamples == 3 && p.d.skipped == 0);
    const Region& a = p.d.regions[0];
    CHECK(!strcmp(p.path(0), "/MULTISAMPLE/Demo/Demo Pad-000-048-c2.wav"));
    // keys: C2 (48) takes 0..54, C3 (60) 55..127 (the middle 54 / 55 of the gap 48..60: 54 to the lower)
    CHECK(a.lokey == 0 && a.hikey == 54 && a.keycenter == 48 && a.tune == 0);
    CHECK(p.d.regions[2].lokey == 55 && p.d.regions[2].hikey == 127 && p.d.regions[2].keycenter == 60);
    // velocity: the lowest layer from 1 to just under the next threshold (0.494 x 127 = 63)
    CHECK(a.lovel == 1 && a.hivel == 62 && p.d.regions[1].lovel == 63 && p.d.regions[1].hivel == 127);
    CHECK(p.d.regions[2].lovel == 1 && p.d.regions[2].hivel == 127);   // a lone layer: everything
    // loops: Forward + keep = continuous, with its points; Off = none; the crossfade is not in the model (counted)
    CHECK(a.loop_mode == kLoopContinuous && a.loop_start == 24000 && a.loop_end == 96000);
    CHECK(p.d.regions[1].loop_mode == kNoLoop && p.d.regions[1].loop_start == kUnset);
    CHECK(p.d.ignored == 1 && !strcmp(p.d.first_ignored, "loop-crossfade"));
    CHECK(a.seq_length == 1 && a.trigger == kTrigAttack && a.amplitude == 100);
  }
  {  // loop-mode Forward alone sustains; none at all is a one-shot; trim; a fractional key-center becomes tune
    D p;
    CHECK(p.parse("version = 0\nname = \"T\"\n[[key-zones]]\npitch = 40\nkey-center = 40.5\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.5\nstrategy = 'Forward'\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'a.wav'\nloop-mode = 'Forward'\n"
                  "loop-start = 10\nloop-end = 20\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'b.wav'\ntrim-start = 100\ntrim-end = 200\n"));
    CHECK(p.d.nregions == 2);
    const Region &a = p.d.regions[0], &b = p.d.regions[1];
    CHECK(a.loop_mode == kLoopSustain && b.loop_mode == kOneShot);
    CHECK(b.offset == 100 && b.end == 199 && a.offset == 0 && a.end == kUnset);
    CHECK(a.keycenter == 41 && a.tune > -51 && a.tune < -49);   // 40.5 -> key 41, and half a semitone down (cents)
    // two slots in one layer rotate
    CHECK(a.seq_length == 2 && a.seq_position == 1 && b.seq_length == 2 && b.seq_position == 2);
  }
  {  // several zones in any order: ranges follow the pitches; the same sample file shared by zones is one entry
    D p;
    std::string t = "version = 0\nname = 'S'\n";
    for (int pitch : {72, 36, 60}) {
      t += "[[key-zones]]\npitch = " + std::to_string(pitch) + "\nkey-center = " + std::to_string(pitch) + ".0\n";
      t += "[[key-zones.velocity-layers]]\nvelocity = 0.5\nstrategy = 'Forward'\n";
      t += "[[key-zones.velocity-layers.sample-slots]]\nsample = 'x.wav'\n";
    }
    CHECK(p.parse(t) && p.d.nregions == 3 && p.d.nsamples == 1);
    // 72: from 67; 36: 0..48; 60: 49..66
    CHECK(p.d.regions[0].lokey == 67 && p.d.regions[0].hikey == 127);
    CHECK(p.d.regions[1].lokey == 0 && p.d.regions[1].hikey == 48);
    CHECK(p.d.regions[2].lokey == 49 && p.d.regions[2].hikey == 66);
  }
  {  // three velocity layers; names with spaces and a sub folder
    D p;
    CHECK(p.parse("version = 0\nname = 'V'\n[[key-zones]]\npitch = 60\nkey-center = 60.0\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.9\nstrategy = 'Forward'\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'sub/loud one.wav'\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.2\nstrategy = 'Forward'\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'soft.wav'\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.5\nstrategy = 'Forward'\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'mid.wav'\n"));
    CHECK(p.d.nregions == 3 && !strcmp(p.path(0), "/MULTISAMPLE/Demo/sub/loud one.wav"));
    CHECK(p.d.regions[0].lovel == 114 && p.d.regions[0].hivel == 127);   // 0.9 x 127
    CHECK(p.d.regions[1].lovel == 1 && p.d.regions[1].hivel == 63);      // soft: up to under mid (0.5 x 127 = 64)
    CHECK(p.d.regions[2].lovel == 64 && p.d.regions[2].hivel == 113);
  }
  {  // damaged and odd input: dropped and counted, the rest still reads; nothing playable is refused
    D p;
    CHECK(p.parse("version = 0\n[[key-zones]]\npitch = 300\nkey-center = 60.0\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.5\n[[key-zones.velocity-layers.sample-slots]]\nsample = 'a.wav'\n"
                  "[[key-zones]]\nkey-center = 50.0\n"
                  "[[key-zones.velocity-layers]]\n[[key-zones.velocity-layers.sample-slots]]\nsample = 'b.wav'\n"
                  "[[key-zones]]\npitch = 61\nfoo = 'bar'\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.5\n[[key-zones.velocity-layers.sample-slots]]\nsample = '../../../x.wav'\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nloop-mode = 'Off'\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'ok.wav'\nweird = 3\n"
                  "[surprise]\nx = 1\n"));
    CHECK(p.d.nregions == 1 && p.d.skipped == 4 && p.d.ignored == 2 && p.d.malformed >= 1);   // pitch 300, no pitch, above root, no sample
    CHECK(!strcmp(p.path(0), "/MULTISAMPLE/Demo/ok.wav") && p.d.regions[0].lokey == 0 && p.d.regions[0].hikey == 127);
    D q;
    CHECK(!q.parse("version = 0\nname = 'empty'\n[[key-zones]]\npitch = 60\n") && strstr(q.d.err, "no playable region"));
    const char bin[] = {'[', '[', 'k', 'e', 'y', '-', 'z', 'o', 'n', 'e', 's', ']', ']', 0, 'x'};
    D r;
    CHECK(!msamp::parse(bin, sizeof bin, "/X", &r.d));
  }
  {  // a Tonverk-shaped instrument (shape only: a synthetic text): zones 6 semitones apart from C1, one layer at
     // 0.99607843, a Forward loop with a 32-frame crossfade kept on release, names with spaces and '#'
    D p;
    std::string t = "# ELEKTRON MULTI-SAMPLE MAPPING FORMAT\nversion = 0\nname = 'synth'\n";
    const char* nm[4] = {"C1", "F#1", "C2", "F#2"};
    for (int i = 0; i < 4; ++i) {
      t += "\n[[key-zones]]\npitch = " + std::to_string(36 + 6 * i) + "\nkey-center = " + std::to_string(36 + 6 * i) + ".0\n";
      t += "\n[[key-zones.velocity-layers]]\nvelocity = 0.99607843\nstrategy = 'Forward'\n";
      t += std::string("\n[[key-zones.velocity-layers.sample-slots]]\nsample = 'synth ") + nm[i] + " V110.wav'\n";
      t += "loop-mode = 'Forward'\nloop-start = 1000\nloop-end = 9000\nloop-crossfade = 32\nkeep-looping-on-release = true\n";
    }
    CHECK(p.parse(t, "/MULTISAMPLE/elmulti/synth") && p.d.nregions == 4 && p.d.nsamples == 4);
    CHECK(!strcmp(p.path(1), "/MULTISAMPLE/elmulti/synth/synth F#1 V110.wav"));
    // the spans: 0..39, 40..45, 46..51, 52..127; one layer takes every velocity, the loop is continuous
    int lo[4] = {0, 40, 46, 52}, hi[4] = {39, 45, 51, 127};
    for (int i = 0; i < 4; ++i) {
      const Region& r = p.d.regions[i];
      CHECK(r.lokey == lo[i] && r.hikey == hi[i] && r.keycenter == 36 + 6 * i && r.lovel == 1 && r.hivel == 127);
      CHECK(r.loop_mode == kLoopContinuous && r.loop_start == 1000 && r.loop_end == 9000);
    }
    CHECK(p.d.ignored == 4 && !strcmp(p.d.first_ignored, "loop-crossfade") && p.d.skipped == 0 && p.d.malformed == 0);
  }
  {  // huge or non-finite numbers are not numbers here (no cast of inf / 1e300); trim-end at the top is no sentinel;
     // a zone's layer without `velocity` takes the default, not the previous zone's
    D p;
    CHECK(p.parse("version = 0\n[[key-zones]]\npitch = 60\nkey-center = 1e300\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.9\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'a.wav'\nloop-start = 1e30\ntrim-start = 99999999999\n"
                  "trim-end = 4294967295\n"
                  "[[key-zones]]\npitch = 72\n"
                  "[[key-zones.velocity-layers]]\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'b.wav'\n"
                  "[[key-zones.velocity-layers]]\nvelocity = 0.9\n"
                  "[[key-zones.velocity-layers.sample-slots]]\nsample = 'c.wav'\n"));
    const Region& a = p.d.regions[0];
    CHECK(p.d.nregions == 3 && a.keycenter == 60 && a.tune == 0 && a.loop_start == kUnset && a.offset == 0 && a.end == kUnset);
    CHECK(p.d.ignored >= 4);
    // zone 72: the default layer (63) below the 0.9 layer (114): 1..113 and 114..127
    CHECK(p.d.regions[1].lovel == 1 && p.d.regions[1].hivel == 113 && p.d.regions[2].lovel == 114);
    D q;
    CHECK(q.parse("[[key-zones]]\npitch = 60\n[[key-zones.velocity-layers]]\n[[key-zones.velocity-layers.sample-slots]]\n"
                  "sample = 'a.wav'\ntrim-end = 4000000000\n") && q.d.regions[0].end == kUnset);   // over the limit: ignored
  }
  {  // a big zone: many layers and slots in one zone parse (linear)
    D p;
    std::string t = "[[key-zones]]\npitch = 60\n";
    for (int l = 0; l < 100; ++l) {
      t += "[[key-zones.velocity-layers]]\nvelocity = " + std::to_string(l / 100.0) + "\n";
      for (int s = 0; s < 2; ++s) t += "[[key-zones.velocity-layers.sample-slots]]\nsample = 'a" + std::to_string(l % 50) + ".wav'\n";
    }
    CHECK(p.parse(t) && p.d.nregions == 200 && p.d.regions[0].seq_length == 2 && p.d.regions[199].hivel == 127);
  }
  {  // not an elmulti: the .sfz reader as before (an sfz that merely mentions the word is still an sfz)
    D p;
    CHECK(!is_elmulti("<region> sample=a.wav // [[key-zones\n", 40));
    CHECK(p.parse("<region> sample=a.wav key=60\n") && p.d.nregions == 1 && p.d.regions[0].lokey == 60);
  }
  {  // the pools take the extension, in either case
    CHECK(pools::accepts_file(pools::kInstr, "/MULTISAMPLE/Demo/Demo Pad.elmulti"));
    CHECK(pools::accepts_file(pools::kInstr, "/MULTISAMPLE/Demo/Demo.ELMULTI"));
    CHECK(pools::accepts_file(pools::kInstr, "/MULTISAMPLE/Demo/Demo.sfz"));
    CHECK(!pools::accepts_file(pools::kInstr, "/MULTISAMPLE/Demo/Demo.wav") && !pools::accepts_file(pools::kInstr, "/M/.hidden.elmulti"));
    CHECK(!pools::accepts_file(pools::kSamples, "/Samples/a.elmulti"));
  }
  printf("ELMULTI TESTS PASS\n");
  return 0;
}
