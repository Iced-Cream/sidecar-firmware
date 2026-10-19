// rec_mini.h - the Sidecar Mini's recorder (core/rec/mini_rec.h, docs/recorder-mini.md) in the host build: the
// Rytm's Overbridge IN stream to the card as the Pi writes it (/Recordings/<prefix>_NNN/NN_NAME.wav, 512-byte headers,
// 16 or 24 bit). Included by voiceloop.ino (RYTM_USB_HOST, after webui.h). Everything here is cold (FLASHMEM) but
// the recorder's producer, which ob_rx (usb_host.h) calls from the USB host interrupt.
//
// Memory, picked at boot:
//   - no PSRAM (a stock Teensy 4.1): one 16 KB ring in RAM1 (DTCM, .bss: RYTM_REC_RING_KB), 4 KB writes: MASTER
//     stereo (or any two mono IN channels); 24-bit stereo is 288 KB/s, so the ring holds 57 ms of the card being
//     busy (85 ms at 16 bit: the choice for slow cards). Stems are refused ("stems need PSRAM").
//   - PSRAM: up to 1.5 MB of it (what the 4 MB the sample pool leaves still has after the plugin state, 2 MB, and the
//     plugin code arena, ~1.1 MB with psram-arena: ~0.9 MB, so 768 KB in practice), extmem_malloc'd at 0x7xxxxxxx
//     (core::sample_ram_alloc), writes of up to 32 KB: the 12 Overbridge channels as mono stems (and MASTER), 64 KB
//     a file at 768 KB (0.45 s at 144 KB/s each), 128 KB at 1.5 MB.
// The card: SdFat (cards.sd()); a write only when the card is not busy (SdioCard::isBusy), each file reserved
// contiguous at its start (FsFile::preAllocate, up to 256 MB a file) and cut to its length when it closes.
// Console (':' lines, also the web console's):
//   rec                      @{"t":"rec",...} the recorder's state (the web console polls it): after a take the card's
//                            figures stay: cardKBps, worstWriteUs (and Ms), ringMax (high-water %), busy (polls), drops
//   rec=info                 the same as plain log lines (the state, then the card line)
//   rec=start|stop|arm       record now, stop (or disarm), arm (the Rytm's PLAY starts a take, STOP ends it)
//   rec=mode:stereo|stems    MASTER (00_MASTER.wav, stereo) or the 12 Overbridge channels (01_MAIN_L .. 12_IN_R)
//   rec=ch:LIST              any of ids 0..12 / names (core/rec/rec_names.h: master,bd,sd,...), as on the Pi
//   rec=bits:16|24           rec=name:PREFIX ([A-Za-z0-9_-], 1..24)

//
// The pool (the USB audio builds, host_stereo.sh / host_uac2.sh: -DUAC2_RING_MEM=rec_pool): without PSRAM the ring is
// one 32 KB block of RAM1 shared with USB audio's rings (the UAC2 driver's OUT ring at its start, then uac_in:
// usb_host.h), one user at a time: you record either to the computer or to the card. While the recorder is armed or
// records (MiniRecorder::busy) it holds the block and USB audio is paused (usb_uac2_pause): the computer keeps the
// audio device, IN streams silence, OUT is received and dropped; back when the recorder is idle again (rec_tick).
// With PSRAM the ring is there and USB audio is never paused. USB MIDI is not in the pool: it runs throughout.
#if defined(USB_SERIAL_UAC2) && defined(UAC2_RING_MEM)
#define REC_POOL 1
#ifndef RYTM_REC_RING_KB
#define RYTM_REC_RING_KB 32   // a power of 2 (MASTER's ring is all of it): 114 ms of the card at 24-bit stereo
#endif
#else
#define REC_POOL 0
#endif
#ifndef RYTM_REC_RING_KB
#define RYTM_REC_RING_KB 16   // the internal ring (RAM1); 0: no recorder without PSRAM
#endif

rec::MiniRecorder g_rec;
bool rec_ready = false;
#if REC_POOL
}  // namespace (the pool is the UAC2 driver's ring by name, usb_uac2.c: outside the anonymous namespace)
extern "C" int32_t UAC2_RING_MEM[RYTM_REC_RING_KB * 256] __attribute__((aligned(32)));   // the pool: RYTM_REC_RING_KB KB
int32_t UAC2_RING_MEM[RYTM_REC_RING_KB * 256] __attribute__((aligned(32)));
namespace {
uint8_t* const rec_ring = (uint8_t*)UAC2_RING_MEM;
#elif RYTM_REC_RING_KB
uint8_t rec_ring[RYTM_REC_RING_KB * 1024] __attribute__((aligned(32)));
#endif
bool rec_in_pool = false;     // the recorder's ring is the pool (no PSRAM)
bool rec_pool_lent = false;   // the recorder holds it now: USB audio paused
#if REC_POOL
void uac_pool_lend(bool lend);   // usb_host.h: USB audio paused (its rings' memory lent) or back
#endif
// Before the recorder arms or starts (it writes the ring from its next block on, in the USB host interrupt).
FLASHMEM void rec_pool_take() {
#if REC_POOL
  if (!rec_in_pool || rec_pool_lent) return;
  uac_pool_lend(true);
  rec_pool_lent = true;
  logf(PSTR("rec: USB audio paused while the recorder is armed or records (they share %d KB)"), RYTM_REC_RING_KB);
#endif
}
// Every loop pass: the recorder idle again (a take closed, disarmed, or a start refused) - USB audio back.
FLASHMEM void rec_pool_give() {
#if REC_POOL
  if (!rec_pool_lent || g_rec.busy()) return;
  rec_pool_lent = false;
  uac_pool_lend(false);
  logf(PSTR("rec: USB audio back"));
#endif
}
FsFile rec_f[rec::kMiniSources];
bool rec_prealloc_warned = false;

FLASHMEM bool rb_mkdir(const char* p) { return cards.sd().exists(p) || cards.sd().mkdir(p, true); }
FLASHMEM int rb_open(const char* p, uint64_t pre) {
  for (int i = 0; i < rec::kMiniSources; ++i) {
    if (rec_f[i].isOpen()) continue;
    if (!rec_f[i].open(p, O_RDWR | O_CREAT | O_TRUNC)) return -1;
    if (pre && !rec_f[i].preAllocate(pre) && !rec_prealloc_warned) {   // best effort: the take still records
      rec_prealloc_warned = true;
      logf(PSTR("rec: %lu MB could not be reserved contiguous for %s (a fragmented card: writes may stall more)"),
           (unsigned long)(pre >> 20), p);
    }
    return i;
  }
  return -1;
}
FLASHMEM bool rb_write(int h, const void* p, uint32_t n) { return rec_f[h].write(p, n) == n; }
FLASHMEM bool rb_header(int h, const void* p, uint32_t n) {
  uint64_t end = rec_f[h].curPosition();
  return rec_f[h].seekSet(0) && rec_f[h].write(p, n) == n && rec_f[h].seekSet(end);
}
FLASHMEM bool rb_sync(int h) { return rec_f[h].sync(); }
FLASHMEM bool rb_close(int h, uint64_t size) {
  bool ok = rec_f[h].truncate(size);   // the reservation past the data released
  ok = rec_f[h].close() && ok;
#if RYTM_WEBUI
  web_df_spent(size);   // the console's cached free space follows
#endif
  return ok;
}
FLASHMEM bool rb_busy() { return cards.sd().card()->isBusy(); }
FLASHMEM bool rb_space(uint64_t* f, uint64_t* t) {
#if RYTM_WEBUI
  if (!web_df_ok) {   // one scan of the FAT / the exFAT bitmap (shared with the console's df)
    int32_t fc = cards.sd().freeClusterCount();
    web_df_total = (uint64_t)cards.sd().clusterCount() * cards.sd().bytesPerCluster();
    web_df_ok = fc >= 0;
    web_df_free = web_df_ok ? (uint64_t)fc * cards.sd().bytesPerCluster() : 0;
  }
  *f = web_df_free, *t = web_df_total;
  return web_df_ok;
#else
  int32_t fc = cards.sd().freeClusterCount();
  *t = (uint64_t)cards.sd().clusterCount() * cards.sd().bytesPerCluster();
  *f = fc >= 0 ? (uint64_t)fc * cards.sd().bytesPerCluster() : 0;
  return fc >= 0;
#endif
}
FLASHMEM uint32_t rb_now() { return micros(); }
FLASHMEM bool rb_list(const char* d, void (*fn)(void*, const char*, bool), void* ctx) {
  FsFile dir, e;
  if (!dir.open(d, O_RDONLY) || !dir.isDir()) return false;
  char name[64];
  while (e.openNext(&dir, O_RDONLY)) {
    e.getName(name, sizeof name);
    bool is_dir = e.isDir();
    e.close();
    if (name[0] != '.') fn(ctx, name, is_dir);
  }
  dir.close();
  return true;
}
FLASHMEM bool rb_read_at(const char* p, void* b, uint32_t n, uint64_t* size) {
  FsFile f;
  if (!f.open(p, O_RDONLY)) return false;
  *size = f.fileSize();
  bool ok = (uint32_t)f.read(b, n) == n;
  f.close();
  return ok;
}
FLASHMEM bool rb_patch(const char* p, uint64_t off, const void* b, uint32_t n) {
  FsFile f;
  if (!f.open(p, O_RDWR)) return false;
  bool ok = f.seekSet(off) && f.write(b, n) == n;
  return f.close() && ok;
}
FLASHMEM bool rb_save(const char* p, const char* t) {
  FsFile f;
  if (!f.open(p, O_WRONLY | O_CREAT | O_TRUNC)) return false;
  size_t n = strlen(t);
  bool ok = f.write(t, n) == n;
  return f.close() && ok;
}
FLASHMEM int rb_load(const char* p, char* out, unsigned cap) {
  FsFile f;
  if (!f.open(p, O_RDONLY)) return -1;
  int n = f.read(out, cap - 1);
  f.close();
  if (n < 0) return -1;
  out[n] = 0;
  return n;
}
FLASHMEM bool rb_remove(const char* p) { return cards.sd().remove(p); }
FLASHMEM void rb_log(const char* l) { logf(PSTR("%s"), l); }
const rec::MiniBackend rec_backend = {rb_mkdir, rb_open, rb_write, rb_header, rb_sync, rb_close, rb_busy, rb_space, rb_now,
                                      rb_list, rb_read_at, rb_patch, rb_save, rb_load, rb_remove, rb_log};

// setup(), after the plugin state (rmp_state_begin) took its PSRAM: the rings, the power-loss repair
FLASHMEM void rec_setup() {
  void* mem = nullptr;
  uint32_t bytes = 0;
  bool psram = false;
  if (external_psram_size)
    for (uint32_t kb : {1536u, 1024u, 768u, 512u, 384u, 256u})   // 768 KB: 64 KB for each of 12 stems (0.45 s)
      if (!mem && (mem = core::sample_ram_alloc(kb * 1024))) bytes = kb * 1024, psram = true;
#if RYTM_REC_RING_KB
  if (!mem) mem = rec_ring, bytes = RYTM_REC_RING_KB * 1024, rec_in_pool = REC_POOL;
#endif
  if (!card_ok || !mem) {
    logf(PSTR("rec: off (%s)"), card_ok ? "no memory" : "no card");
    return;
  }
  g_rec.init(mem, bytes, psram ? 32768 : 4096, psram ? 14 : 2, psram, &rec_backend);
  rec_ready = true;
  int fixed = g_rec.fix_pending();
  logf(PSTR("rec: ready, %lu KB ring in %s: %s%s%s"), (unsigned long)(bytes >> 10), psram ? "PSRAM" : "RAM1",
       psram ? "stereo or stems (the 12 Overbridge channels)" : "stereo (MASTER) only - stems need PSRAM",
       rec_in_pool ? " (shared with USB audio: paused while armed or recording)" : "",
       fixed ? ", a take cut by a power loss repaired" : "");
}

FLASHMEM void rec_say() {
  rec::MiniStatus s;
  g_rec.status(&s, false);
  char ids[48];
  rec::list_ids(s.mask, ids, sizeof ids);
  static const char* const kState[] = {"idle", "armed", "recording", "finishing"};
  logf(PSTR("rec: %s%s, %u-bit, prefix %s, channels %s (%u files, %lu KB/s, ring %lu ms), take %s %lu s, dropped %lu%s%s"),
       kState[s.state & 3], s.arm_mode ? " (PLAY/STOP)" : "", s.bits, s.prefix, ids[0] ? ids : "none", rec::files(s.mask),
       (unsigned long)s.need_kbps, (unsigned long)s.ring_ms, s.take[0] ? s.take : "-", (unsigned long)(s.frames / 48000),
       (unsigned long)s.drops, s.error[0] ? ", error: " : "", s.error);
  if (rec_in_pool)
    logf(PSTR("rec: the ring shares %d KB with USB audio: USB audio %s"), RYTM_REC_RING_KB,
         rec_pool_lent ? "paused (the recorder is armed or records)" : "on");
  if (s.writes)   // the card in the last take: what a tester reports
    logf(PSTR("rec: card %lu KB/s while writing (need %lu), worst write %lu us, ring high-water %lu %%, busy polls %lu, "
              "%lu writes of %lu KB, dropped %lu frames"), (unsigned long)s.write_kbps, (unsigned long)s.need_kbps,
         (unsigned long)s.worst_write_us, (unsigned long)s.ring_max_pct, (unsigned long)s.busy_waits, (unsigned long)s.writes,
         (unsigned long)(s.block >> 10), (unsigned long)s.dropped_frames);
}

FLASHMEM void rec_json() {
#if RYTM_WEBUI
  rec::MiniStatus s;
  if (rec_ready) g_rec.status(&s, true);
  else memset(&s, 0, sizeof s);
  char ids[48];
  rec::list_ids(s.mask, ids, sizeof ids);
  const char* mode = s.mask == 1u << rec::kMaster ? "stereo" : s.mask == rec::kInMask ? "stems" : "custom";
  wj_begin(PSTR("rec"));
  wj(PSTR(",\"state\":%d,\"arm\":%d,\"ready\":%d,\"psram\":%d,\"maxs\":%u,\"bits\":%u,\"mode\":\"%s\",\"usbShared\":%d,"
          "\"usbPaused\":%d,\"prefix\":"), s.state, s.arm_mode, rec_ready, s.psram, s.max_samples, s.bits, mode, rec_in_pool,
     rec_pool_lent);
  wj_str(s.prefix, 24);
  wj(PSTR(",\"take\":"));
  wj_str(s.take, 40);
  wj(PSTR(",\"ch\":[%s],\"frames\":%lu,\"bytes\":%lu,\"drops\":%lu,\"silence\":%lu,\"cardKBps\":%lu,\"needKBps\":%lu,"
          "\"worstWriteMs\":%lu,\"worstWriteUs\":%lu,\"writes\":%lu,\"busy\":%lu,\"ringMs\":%lu,\"ringKB\":%lu,\"blockKB\":%lu,\"ringMax\":%lu,"
          "\"freeMB\":%lu,\"totalMB\":%lu,\"error\":"),
     ids, (unsigned long)s.frames, (unsigned long)(s.bytes >> 10), (unsigned long)s.drops, (unsigned long)s.dropped_frames,
     (unsigned long)s.write_kbps, (unsigned long)s.need_kbps, (unsigned long)(s.worst_write_us / 1000), (unsigned long)s.worst_write_us, (unsigned long)s.writes,
     (unsigned long)s.busy_waits, (unsigned long)s.ring_ms, (unsigned long)(s.ring_bytes >> 10), (unsigned long)(s.block >> 10),
     (unsigned long)s.ring_max_pct, (unsigned long)(s.free_bytes >> 20), (unsigned long)(s.total_bytes >> 20));
  wj_str(s.error, 64);
  wj(PSTR(",\"peak\":["));
  for (int i = 0; i < rec::kMiniSources; ++i) wj(PSTR("%s%lu"), i ? "," : "", (unsigned long)(s.peak[i] * 1000ull / 8388607));
  wj(PSTR("]"));
  wj_end();
#endif
}

FLASHMEM bool rec_console(const char* p) {
  if (strncmp(p, "rec", 3) || (p[3] && p[3] != '=')) return false;
  if (!p[3]) {
    rec_json();
    return true;
  }
  if (!strcmp(p + 4, "info")) {
    if (rec_ready) rec_say();
    else logf(PSTR("rec: off (%s)"), card_ok ? "no memory" : "no card");
    return true;
  }
  char err[96] = "";
  bool ok = rec_ready;
  if (!ok) strcpy(err, card_ok ? "off: no memory" : "off: no card");
  const char* v = p + 4;
  if (!ok) {
  } else if (!strcmp(v, "start")) rec_pool_take(), ok = g_rec.start(err, sizeof err);
  else if (!strcmp(v, "arm")) rec_pool_take(), ok = g_rec.arm(err, sizeof err);
  else if (!strcmp(v, "stop")) g_rec.stop();
  else if (!strcmp(v, "mode:stereo")) ok = g_rec.configure(1u << rec::kMaster, g_rec.bits(), err, sizeof err);
  else if (!strcmp(v, "mode:stems")) ok = g_rec.configure(rec::kInMask, g_rec.bits(), err, sizeof err);
  else if (!strncmp(v, "ch:", 3)) {
    uint32_t m = g_rec.mask();
    ok = rec::parse_list(v + 3, &m, err, sizeof err) && g_rec.configure(m, g_rec.bits(), err, sizeof err);
  } else if (!strncmp(v, "bits:", 5)) {
    ok = g_rec.configure(g_rec.mask(), (unsigned)cap_num(v + 5), err, sizeof err);
  } else if (!strncmp(v, "name:", 5)) {
    ok = g_rec.set_prefix(v + 5);
    if (!ok) strcpy(err, "name: 1..24 of A-Z a-z 0-9 _ -");
  } else {
    ok = false;
    strcpy(err, "rec=start|stop|arm|mode:stereo|mode:stems|ch:LIST|bits:16|24|name:PREFIX");
  }
  if (!ok) logf(PSTR("%s: %s"), p, err);
  if (rec_ready) rec_say();
  rec_json();
  return true;
}

// The SYSTEM page's recorder row (core/ui/remote.h: 1 idle, 2 armed, 3 recording, 4 off)
FLASHMEM void rec_fill(core::ui::RemoteStatus* rs) {
  if (!rec_ready) {
    rs->rec = 4;
    return;
  }
  rec::MiniStatus s;
  g_rec.status(&s, false);
  rs->rec = s.state == rec::MiniRecorder::kArmed ? 2 : s.state == rec::MiniRecorder::kIdle ? 1 : 3;
  rs->rec_secs = s.frames / 48000;
}

// Every loop pass: the remote REC page's request (YES, encoder A), its state every 200 ms and after a request, the
// SYSTEM row every second.
core::ui::RemoteStatus rec_rs = {-1, 0, {-1, -1, -1}, 0, 0, 0, 0, {0, 0, 0, 0}, {{0}, {0}, {0}, {0}, {0}}};
bool rec_rs_set = false;
FLASHMEM void rec_status_base(const core::ui::RemoteStatus& rs) {   // stats_report's figures, the recorder's added
  rec_rs = rs;
  rec_rs_set = true;
  rec_fill(&rec_rs);
  core::ui::remote_status(rec_rs);
}
FLASHMEM void rec_tick(uint32_t ms) {
  static uint32_t last = 0, last_sys = 0, err_at = 0;
  static char err_ui[32];
  rec_pool_give();
  int req = core::ui::remote_rec_take();
  if (req && rec_ready) {
    char err[64] = "";
    bool ok = true;
    if (req == core::ui::kRecStart) rec_pool_take(), ok = g_rec.start(err, sizeof err);
    else if (req == core::ui::kRecArm) rec_pool_take(), ok = g_rec.arm(err, sizeof err);
    else if (req == core::ui::kRecStop || req == core::ui::kRecDisarm) g_rec.stop();
    if (!ok) logf(PSTR("rec: remote page: %s"), err);
    sniprintf(err_ui, sizeof err_ui, PSTR("%s"), ok ? "" : err);
    err_at = ms;
    rec_say();
  } else if (ms - last < 200) {
    return;
  }
  last = ms;
  if (err_ui[0] && ms - err_at > 3000) err_ui[0] = 0;   // an error shows 3 s
  core::ui::RecInfo r = {};
  if (rec_ready) {
    rec::MiniStatus s;
    g_rec.status(&s, false);
    r.state = s.state == rec::MiniRecorder::kArmed ? core::ui::kRecArmed : s.state == rec::MiniRecorder::kRecording
              ? core::ui::kRecRecording : s.state == rec::MiniRecorder::kEnded ? core::ui::kRecFinishing : core::ui::kRecIdle;
    r.secs = s.frames / 48000;
    r.free_mb = (uint32_t)(s.free_bytes >> 20), r.total_mb = (uint32_t)(s.total_bytes >> 20), r.need_kbps = s.need_kbps;
    r.in_mask = (uint16_t)((s.mask >> rec::kIn & 0xFFF) | (s.mask & (1u << rec::kMaster) ? 3 : 0));   // MASTER: IN 0 / 1
    sniprintf(r.take, sizeof r.take, PSTR("%s"), s.take);
    sniprintf(r.error, sizeof r.error, PSTR("%s"), err_ui[0] ? err_ui : s.error);
    r.usb_paused = rec_pool_lent;
  }
  core::ui::remote_rec(r);   // kRecNone without a recorder: the page is not shown
  if (rec_rs_set && ms - last_sys >= 1000) {
    last_sys = ms;
    rec_fill(&rec_rs);
    core::ui::remote_status(rec_rs);
  }
}

FLASHMEM bool rec_pass() { return rec_ready && g_rec.poll(); }
FLASHMEM void rec_transport(uint8_t status) {
  if (rec_ready) g_rec.transport(status);
}
