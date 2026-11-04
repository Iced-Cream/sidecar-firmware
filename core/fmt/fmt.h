// core/fmt/fmt.h - the card's file plumbing: one atomic write (core 0 only), the boot sweep that finishes a write a
// power cut left half done, the posted settings writes from the control core, and the line readers. (The format
// registry this file once held - sniffers, readers, the load path, every format's place - went with the modules that
// had formats, in the 2026-11-04 strip-down; git history has it.)
//
// write_atomic(), AtomicWriter and recover() are the card core's (core 0 on the Pi; cardq's kAtomicReq posts one from
// the control core).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../platform.h"

namespace core {
namespace fmt {

// ---- atomic writes (spec 4.4), the card core's: <path>.tmp written, read back and compared, <path> -> <path>.prev,
// .tmp -> <path>, .prev removed. A cut at any point leaves <path> old or new: recover() at boot puts .prev back when
// <path> is missing and removes a stray .tmp.
bool write_atomic(const char* path, const void* data, uint32_t len);
void recover(const char* path);
// The boot sweep (2026-10-26): recover() for every <path>.tmp / <path>.prev under root (folders to a depth of 8).
// Some writers' readers never call recover() - a CHOP slice map is found by a scan for ".chop.sfz", the sampler's .sfz,
// the pool files - so a cut inside write_atomic's rename window left only the .prev and the file looked lost.
// sweep_boot(): /.rytm, /CHOP and /Samples, the trees the atomic writers used. The card core's, at
// boot before anything indexes the card. Returns the files it changed something for (0: a clean card). CORE_MSAMP
// builds (the Pi, the Mac core): a folder walk needs File::nextEntry.
int sweep(const char* root);
int sweep_boot();
// The same in pieces (a big file: the library's index.bin), one call of step() at most kPiece bytes of card I/O:
// begin(); then step() until it returns false; ok() says whether it is in place. The caller keeps data valid meanwhile.
class AtomicWriter {
 public:
  static constexpr uint32_t kPiece = 65536;
  void begin(const char* path, const uint8_t* data, uint32_t len, uint32_t piece = kPiece);
  bool step();            // true: more to do
  bool ok() const { return st_ == kDoneOk; }
  bool busy() const { return st_ != kIdle && st_ != kDoneOk && st_ != kDoneFail; }
  void cancel();          // its .tmp removed; the old file untouched
  uint32_t bytes_io() const { return io_; }   // the last step's card bytes

 private:
  enum { kIdle, kOpen, kWrite, kClose, kBack, kSwap, kDoneOk, kDoneFail };
  int st_ = kIdle;
  char path_[200];
  const uint8_t* data_ = nullptr;
  uint32_t len_ = 0, off_ = 0, piece_ = kPiece, io_ = 0;
  File f_;
};

// ---- the posted settings write (0.3.0 power-cut safety, spec 4.4 + fix1049): the small per-project and per-Sidecar
// files the control core saves (LFO, INSERT, keys.txt, fxin.txt, sendfx.txt, /.rytm/project) go to the card core
// through cardq's kAtomicReq, one at a time. Why posted and not write_atomic in place: core 3 runs idle_save() in the
// same loop that renders audio blocks and its OUT FIFO holds 2 ms, while on the card model the plain in-place write
// this replaced already cost ~11 ms of card time there and the atomic one would cost ~24 ms (test_powercut prints
// both). Posting takes all of it off core 3 (as seq's saves do, fix1049), so the safe write is also the quiet one.
//
//   char* t = post_begin();      null: one is in flight - the caller stays dirty and tries again
//   ... format at most kPostMax bytes into t ...
//   return post_end(path, len);  posted (without a card core: written now)
//
// The card core reads that buffer while the write is in flight, so post_begin() returning null is what keeps a second
// writer from overwriting bytes the card is still reading. Never hold the pointer across a post_end.
//
// post_step() is called once at the top of idle_save(), before any writer, so only one owns the buffer at a time; a
// failed write is logged there. post_wait(path) lets a pending write of that file land before a load reads it (as
// seq's cw_wait_file); post_wait() with no path waits for whatever is pending, which is what a project or kit change
// needs before its own save, so that save is not deferred past the load that clears the dirty flag. A wait may run
// the write on the calling core (cardq::take_back), so it is for a caller that waits on purpose (the rename's FLUSH;
// the loads without a RAM store), never for a knob turn, a save, a kit change or a pattern switch.
// post_step never takes a write back (phase 3, 2026-10-26): one the card core has not started after cardq::kTakeBackUs
// stays posted - the control core never writes the card itself - and is counted in post_held() (logged once).
//
// post_queue(path, text, len) (2026-10-26, the RAM store): a kit change's save without that wait - posted now when the
// slot is free, else a copy queued behind it, never a wait (oldest first, one entry per path - a newer text replaces
// it and goes to the tail, after every write queued before it - so bounded by the settings files, not by kit changes;
// post_step posts the next when the slot frees; post_begin
// is null while any wait, so a newer save of a queued file never lands before it). post_busy() counts the queue,
// post_wait(path) writes the queue in its order up to that path's write. False for a bad path or length, or out of
// memory (logged: the edit then stays in RAM only). An edit may now sit in RAM behind several queued writes before it
// reaches the card (the power-cut window): small next to the 1 s a save waits for the edits to settle.
// A text over kPostMax (up to kQueueMax: the sequencer's pattern files, 2026-10-26 phase 2) is always queued as its own
// copy - posted at once when the slot and the queue are free - and the slot writes from that copy (CORE_MSAMP only;
// elsewhere refused).
constexpr uint32_t kPostMax = 6144;
constexpr uint32_t kQueueMax = 1u << 20;
char* post_begin();
bool post_end(const char* path, uint32_t len);
// id (optional): this write's own number for post_result (0 when refused)
bool post_queue(const char* path, const char* text, uint32_t len, uint32_t* id = nullptr);
// a file removed through the same queue, in its turn (a queued write of it replaced; CORE_MSAMP only): post_result's
// ok when it is gone (or was not there)
bool post_remove(const char* path, uint32_t* id = nullptr);
// true only when this call posted a write. A write that only waits is no work of this core's: idle_save goes on, the streams' idle step above all
// (a burst of N pattern writes must not leave them one pass a write). post_busy() says whether one is under way.
bool post_step();
bool post_busy();
uint32_t post_held();   // posted writes the card core had not taken after cardq::kTakeBackUs (kept posted, never run here)
// fix round 1: the slot's write is held now (posted past kTakeBackUs): the LFO / INSERT idle saves queue (post_queue)
// instead of waiting for the slot
bool post_stalled();
void post_wait(const char* path = nullptr);
// 2026-10-26, the RAM store: a write of this file not yet on the card (queued, or posted and not finished); never waits.
// The store's fill reads no file while it is pending (it would hold the bytes the write is about to replace).
bool pending(const char* path);
// 2026-10-26: how many writes (queued, and the slot's once the card core has it) are not yet on the card - one still in cardq's ring is cardq::pending()'s, not counted here - and in *oldest_ms the age
// of the oldest of them (0 when none): /status "store". Diagnostics: plain reads from another core, torn at worst.
uint32_t unwritten(uint32_t* oldest_ms);
bool post_ok();     // the last finished write's result
// A write's own result (post_ok is the last finished write's, of any file): by the id post_queue gave it. Pending while
// queued or in the slot; ok / failed once done (the newest kPostResults finished writes are kept); unknown for an id
// never given, one replaced by a newer text of its path before it was written, or one older than the kept results.
// The SONG backup (core/song) waits on its own files with it: another file's failure in the same post_wait never
// hides its own (undo-replay review 2).
enum PostResult : uint8_t { kPostPending, kPostOk, kPostFailed, kPostUnknown };
constexpr int kPostResults = 64;
PostResult post_result(uint32_t id);

void reset_for_tests();

// A text file's lines through a read-ahead buffer: one File::read per sizeof buf bytes, not one per byte. On the Pi a
// one-byte read is a whole f_read, each taking FatFs's volume lock, which waits behind the card core's work: the per-kit
// LFO and insert files read a byte at a time took core 3 40 ms at every kit change [measured 2026-10-26: the Rytm's
// 3B, 40.2-40.6 ms, four switches]. line(): the next line without its \r and \n (cut at max - 1 characters), its
// length; -1 at the end (a last line without \n is still a line; a read error ends the file, as the byte reader did).
// The length, not just "there was a line": a line whose first byte is a NUL is not a blank one, and a loader must not
// take it for one (0.3.0: it would leave a damaged file unlocked for the next save to write over).
struct LineReader {
  explicit LineReader(File& file) : f(file) {}
  int line(char* out, int max);
  File& f;
  char buf[256];
  int n = 0, i = 0;
  bool end = false;
};
// The same lines from bytes already in RAM (2026-10-26: the RAM store, core/store/store.h, holds the per-kit LFO and
// insert files): the next line of d[0..n) from *at, by LineReader's rules exactly (\r dropped, cut at max - 1, a last
// line without \n still a line, its length so a NUL-led line is not a blank one); *at moves past its \n. -1 at the end.
int next_line(const uint8_t* d, uint32_t n, uint32_t* at, char* out, int max);

}  // namespace fmt
}  // namespace core
