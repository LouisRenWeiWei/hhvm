/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2016 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#ifndef incl_HPHP_RUNTIME_VM_JIT_MC_GENERATOR_H_
#define incl_HPHP_RUNTIME_VM_JIT_MC_GENERATOR_H_

#include <memory>
#include <utility>
#include <vector>
#include <tbb/concurrent_hash_map.h>

#include "hphp/util/asm-x64.h"
#include "hphp/util/eh-frame.h"
#include "hphp/util/ringbuffer.h"

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/vm-regs.h"

#include "hphp/runtime/vm/jit/alignment.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/fixup.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"

namespace HPHP { namespace jit {

typedef hphp_hash_map<TCA, TransID> TcaTransIDMap;

struct AsmInfo;
struct CGMeta;
struct IRGS;
struct Label;
struct MCGenerator;

extern "C" MCGenerator* mcg;

constexpr size_t kNonFallthroughAlign = 64;
constexpr int kLeaRipLen = 7;
constexpr int kTestRegRegLen = 3;
constexpr int kTestImmRegLen = 5;  // only for rax -- special encoding
// Cache alignment is required for mutable instructions to make sure
// mutations don't "tear" on remote cpus.
constexpr size_t kX64CacheLineSize = 64;
constexpr size_t kX64CacheLineMask = kX64CacheLineSize - 1;
const TCA kInvalidCatchTrace   = (TCA)(-1);

//////////////////////////////////////////////////////////////////////

struct FreeStubList {
  struct StubNode {
    StubNode* m_next;
    uint64_t  m_freed;
  };
  static const uint64_t kStubFree = 0;
  FreeStubList() : m_list(nullptr) {}
  TCA peek() { return (TCA)m_list; }
  TCA maybePop();
  void push(TCA stub);
 private:
  StubNode* m_list;
};

struct UsageInfo {
  std::string m_name;
  size_t m_used;
  size_t m_capacity;
  bool m_global;
};

struct TransRelocInfo;

using CatchTraceMap = TreadHashMap<CTCA, TCA, ctca_identity_hash>;

//////////////////////////////////////////////////////////////////////

/*
 *
 * MCGenerator handles the machine-level details of code generation (e.g.,
 * translation cache entry, code smashing, code cache management) and delegates
 * the bytecode-to-asm translation process to translateRegion().
 *
 */
struct MCGenerator {
  /*
   * True iff the calling thread is the sole writer.
   */
  static bool canWrite() {
    // We can get called early in boot, so allow null mcg.
    return !mcg || Translator::WriteLease().amOwner();
  }

  static CallSpec getDtorCall(DataType type);

  MCGenerator();
  ~MCGenerator();

  MCGenerator(const MCGenerator&) = delete;
  MCGenerator& operator=(const MCGenerator&) = delete;

  /*
   * Accessors.
   */
  CodeCache& code() { return m_code; }
  const UniqueStubs& ustubs() const { return m_ustubs; }
  Translator& tx() { return m_tx; }
  FixupMap& fixupMap() { return m_fixupMap; }
  FreeStubList& freeStubList() { return m_freeStubs; }
  LiteralMap& literals() { return m_literals; }

  DataBlock& globalData() { return m_code.data(); }
  Debug::DebugInfo* getDebugInfo() { return &m_debugInfo; }

  TcaTransIDMap& getJmpToTransIDMap() {
    return m_jmpToTransID;
  }

  inline bool isValidCodeAddress(TCA tca) const {
    return m_code.isValidCodeAddress(tca);
  }

  /*
   * Handlers for function prologues.
   */
  TCA getFuncPrologue(Func* func, int nPassed, ActRec* ar = nullptr,
                      bool forRegeneratePrologue = false);
  void smashPrologueGuards(AtomicLowPtr<uint8_t>* prologues,
                           int numPrologues, const Func* func);

  TCA getFuncBody(Func* func);

  inline void sync() {
    if (tl_regState == VMRegState::CLEAN) return;
    syncWork();
  }

  bool useLLVM() const { return m_useLLVM; }
  void setUseLLVM(bool useLLVM) { m_useLLVM = useLLVM; }

  template<typename T, typename... Args>
  T* allocData(Args&&... args) {
    return m_code.data().alloc<T>(std::forward<Args>(args)...);
  }

  /*
   * Allocate a literal value in the global data section.
   */
  const uint64_t* allocLiteral(uint64_t val, CGMeta& fixups);

  /*
   * enterTC is the main entry point for the translator from the bytecode
   * interpreter.  It operates on behalf of a given nested invocation of the
   * intepreter (calling back into it as necessary for blocks that need to be
   * interpreted).
   *
   * If start is the address of a func prologue, stashedAR should be the ActRec
   * prepared for the call to that function, otherwise it should be nullptr.
   *
   * But don't call it directly, use one of the helpers below.
   */
 private:
  void enterTC(TCA start, ActRec* stashedAR);
 public:
  void enterTC() {
    enterTC(ustubs().resumeHelper, nullptr);
  }
  void enterTCAtPrologue(ActRec *ar, TCA start) {
    assertx(ar);
    assertx(start);
    enterTC(start, ar);
  }
  void enterTCAfterPrologue(TCA start) {
    assertx(start);
    enterTC(start, nullptr);
  }

  /*
   * Called before entering a new PHP "world."
   */
  void requestInit();

  /*
   * Called at the end of eval()
   */
  void requestExit();

  void initUniqueStubs();
  int numTranslations(SrcKey sk) const;
  bool addDbgGuards(const Unit* unit);
  bool addDbgGuard(const Func* func, Offset offset, bool resumed);
  bool freeRequestStub(TCA stub);

  /*
   * Return a TCA suitable for emitting an ephemeral stub. A reused stub will
   * be returned if one is available. Otherwise, frozen.frontier() will be
   * returned.
   *
   * If not nullptr, isReused will be set to whether or not a reused stub was
   * returned.
   */
  TCA getFreeStub(CodeBlock& frozen, CGMeta* fixups,
                  bool* isReused = nullptr);
  folly::Optional<TCA> getCatchTrace(CTCA ip) const;
  CatchTraceMap& catchTraceMap() { return m_catchTraceMap; }
  TCA getTranslatedCaller() const;
  bool profileSrcKey(SrcKey sk) const;
  void getPerfCounters(Array& ret);
  bool reachedTranslationLimit(SrcKey, const SrcRec&) const;
  void recordGdbStub(const CodeBlock& cb, TCA start, const std::string& name);

  /*
   * Dump translation cache.  True if successful.
   */
  bool dumpTC(bool ignoreLease = false);

  /*
   * Return cache usage information as a string
   */
  std::string getUsageString();
  std::string getTCAddrs();
  std::vector<UsageInfo> getUsageInfo();

  /*
   * Returns the total size of the TC now and at the beginning of this request,
   * in bytes. Note that the code may have been emitted by other threads.
   */
  void codeEmittedThisRequest(size_t& requestEntry, size_t& now) const;

public:
  /*
   * This function is called by translated code to handle service requests,
   * which usually involve some kind of jump smashing. The returned address
   * will never be null, and indicates where the caller should resume
   * execution.
   *
   * The forced symbol name is so we can call this from
   * translator-asm-helpers.S without hardcoding a fragile mangled name.
   */
  TCA handleServiceRequest(svcreq::ReqInfo& info) noexcept
#ifdef _MSC_VER
    // For MSVC, we've had to hard-code the mangled name,
    // because we can't explicitly set it like we can with
    // GCC/Clang :(
    ;
#else
    asm("MCGenerator_handleServiceRequest");
#endif

  /*
   * Smash the PHP call at address toSmash to point to the appropriate prologue
   * for calleeFrame, returning the address of said prologue. If a prologue
   * doesn't exist and this function can't get the write lease it may return
   * fcallHelperThunk, which uses C++ helpers to act like a prologue.
   */
  TCA handleBindCall(TCA toSmash, ActRec* calleeFrame, bool isImmutable);

  /*
   * If we suspend an FCallAwait frame we need to suspend the
   * caller. Returning to the jitted code will automatically take care
   * of that, but if we're returning in the interpreter, we have to
   * handle it separately. If the frame we're returning from was the
   * vmJitCalledFrame(), we have to exit from handleResume (see
   * comments for jitReturnPre and jitReturnPost). After exiting from
   * there, there is no correct bytecode to resume at, so we use this
   * helper to cleanup and continue.
   */
  TCA handleFCallAwaitSuspend();

  /*
   * Look up (or create) and return the address of a translation for the
   * current VM location. If no translation can be found or created, this
   * function will interpret until it finds one, possibly throwing exceptions
   * or reentering the VM. If interpFirst is true, at least one basic block
   * will be interpreted before attempting to look up a translation. This is
   * necessary to ensure forward progress in certain situations, such as
   * hitting the translation limit for a SrcKey.
   */
  TCA handleResume(bool interpFirst);

private:
  /*
   * Service request handlers.
   */
  TCA bindJmp(TCA toSmash, SrcKey dest, ServiceRequest req,
              TransFlags trflags, bool& smashed);
  TCA bindJccFirst(TCA toSmash,
                   SrcKey skTrue, SrcKey skFalse,
                   bool toTake,
                   bool& smashed);

  bool shouldTranslate(const Func*, TransKind) const;
  bool shouldTranslateNoSizeLimit(const Func*) const;

  TCA getTopTranslation(SrcKey sk) {
    return m_tx.getSrcRec(sk)->getTopTranslation();
  }

  void syncWork();

  TCA getTranslation(const TranslArgs& args);
  TCA createTranslation(const TranslArgs& args);
  bool createRetranslateStub(SrcKey sk);
  TCA retranslate(const TranslArgs& args);
  TCA translate(const TranslArgs& args);
  TCA translateWork(const TranslArgs& args);

  TCA lookupTranslation(SrcKey sk) const;
  TCA retranslateOpt(TransID transId, bool align);

  /*
   * Prologue-generation helpers.
   */
  TCA regeneratePrologues(Func* func, SrcKey triggerSk);
  TCA regeneratePrologue(TransID prologueTransId, SrcKey triggerSk);
  TCA emitFuncPrologue(Func* func, int nPassed);
  bool checkCachedPrologue(const Func*, int prologueIndex, TCA&) const;

  void invalidateSrcKey(SrcKey sk);
  void invalidateFuncProfSrcKeys(const Func* func);

  void recordGdbTranslation(SrcKey sk, const Func* f,
                            const CodeBlock& cb,
                            const TCA start,
                            bool exit, bool inPrologue);

  void recordBCInstr(uint32_t op, const TCA addr, const TCA end, bool cold);

  /*
   * TC dump helpers
   */
  bool dumpTCCode(const char* filename);
  bool dumpTCData();
  void drawCFG(std::ofstream& out) const;

private:
  CodeCache m_code;
  UniqueStubs m_ustubs;
  Translator m_tx;

  // maps jump addresses to the ID of translation containing them.
  TcaTransIDMap      m_jmpToTransID;
  uint64_t           m_numTrans;
  FixupMap           m_fixupMap;
  EHFrameHandle      m_unwindRegistrar;
  CatchTraceMap      m_catchTraceMap;
  Debug::DebugInfo   m_debugInfo;
  FreeStubList       m_freeStubs;
  LiteralMap         m_literals;

  // asize + acoldsize + afrozensize + gdatasize
  size_t             m_totalSize;

  // Used to tell the codegen backend when it should attempt to use LLVM, and
  // to tell clients of the codegen backend when LLVM codegen succeeded.
  bool               m_useLLVM;
};

TCA fcallHelper(ActRec*);
TCA funcBodyHelper(ActRec*);
int64_t decodeCufIterHelper(Iter* it, TypedValue func);

/*
 * Look up the catch block associated with the return address in ar and save it
 * in a queue. This is called by debugger helpers right before smashing the
 * return address to prevent returning directly the to TC.
 */
void pushDebuggerCatch(const ActRec* ar);

/*
 * Pop the oldest entry in the debugger catch block queue, assert that it's
 * from the given ActRec, and return it.
 */
TCA popDebuggerCatch(const ActRec* ar);

void emitIncStat(Vout& v, Stats::StatCounter stat, int n = 1,
                 bool force = false);

bool shouldPGOFunc(const Func& func);

#define TRANS_PERF_COUNTERS \
  TPC(translate) \
  TPC(retranslate) \
  TPC(interp_bb) \
  TPC(interp_bb_force) \
  TPC(interp_instr) \
  TPC(interp_one) \
  TPC(max_trans) \
  TPC(enter_tc) \
  TPC(service_req) \
  TPC(unser_prop_slow) \
  TPC(unser_prop_fast) \
  TPC(thrift_read_slow) \
  TPC(thrift_write_slow) \
  TPC(thrift_spec_slow)

#define TPC(n) tpc_ ## n,
enum TransPerfCounter {
  TRANS_PERF_COUNTERS
  tpc_num_counters
};
#undef TPC

extern __thread int64_t s_perfCounters[];
#define INC_TPC(n) ++jit::s_perfCounters[jit::tpc_##n];

/*
 * Return whether the `calleeAR' frame overflows the stack.
 *
 * Expects `calleeAR' and its arguments to be on the VM stack.
 */
bool checkCalleeStackOverflow(const ActRec* calleeAR);

/*
 * Handle a VM stack overflow condition by throwing an appropriate exception.
 */
void handleStackOverflow(ActRec* calleeAR);

/*
 * Determine whether something is a stack overflow, and if so, handle it.
 *
 * NB: This only works when called from a particular point in a func prologue,
 * and should probably be renamed.  (Fortunately, that's the only callsite.)
 */
void handlePossibleStackOverflow(ActRec* calleeAR);

/*
 * Dumps the contents of the Translation Cache.
 * Returns whether or not it succeeded.
 */
bool tc_dump(bool ignoreLease=false);

}}

#endif
