/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
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
#include "hphp/util/alloc.h"

#include <atomic>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef FOLLY_SANITIZE_ADDRESS
#include <sanitizer/asan_interface.h>
#endif

#include <fmt/format.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/SysResource.h>

#include "hphp/util/address-range.h"
#include "hphp/util/bump-mapper.h"
#include "hphp/util/extent-hooks.h"
#include "hphp/util/hugetlb.h"
#include "hphp/util/kernel-version.h"
#include "hphp/util/managed-arena.h"
#include "hphp/util/maphuge.h"
#include "hphp/util/numa.h"
#include "hphp/util/slab-manager.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

HHVM_ATTRIBUTE_WEAK uintptr_t tc_start_address() {
  return (uintptr_t)sbrk(0);
}

void flush_thread_caches() {
#ifdef USE_JEMALLOC
  mallctlCall<true>("thread.tcache.flush");
  arenas_thread_flush();
#endif
}

__thread int32_t s_numaNode;

__thread uintptr_t s_stackLimit;
__thread size_t s_stackSize;
const size_t s_pageSize = sysconf(_SC_PAGESIZE);

__thread MemBlock s_tlSpace;
__thread MemBlock s_hugeRange;

static NEVER_INLINE uintptr_t get_stack_top() {
  using ActRec = char;
  DECLARE_FRAME_POINTER(fp);
  return uintptr_t(fp) - s_pageSize;
}

void init_stack_limits(pthread_attr_t* attr) {
  size_t stacksize, guardsize;
  void *stackaddr;
  struct rlimit rlim;

  if (pthread_attr_getstack(attr, &stackaddr, &stacksize) != 0) {
    always_assert(false);
  }

  // Get the guard page's size, because the stack address returned
  // above starts at the guard page, so the thread's stack limit is
  // stackaddr + guardsize.
  if (pthread_attr_getguardsize(attr, &guardsize) != 0) {
    guardsize = 0;
  }

  assert(stackaddr != nullptr);
  assert(stacksize >= PTHREAD_STACK_MIN);
  s_stackLimit = uintptr_t(stackaddr) + guardsize;
  s_stackSize = stacksize - guardsize;

  // The main thread's native stack may be larger than desired if
  // set_stack_size() failed.  Make sure that even if the native stack is
  // extremely large (in which case anonymous mmap() could map some of the
  // "stack space"), we can differentiate between the part of the native stack
  // that could conceivably be used in practice and all anonymous mmap() memory.
  if (getrlimit(RLIMIT_STACK, &rlim) == 0 && rlim.rlim_cur == RLIM_INFINITY &&
      s_stackSize > kStackSizeMinimum) {
    s_stackLimit += s_stackSize - kStackSizeMinimum;
    s_stackSize = kStackSizeMinimum;
  }
}

void flush_thread_stack() {
  uintptr_t top = get_stack_top() & (s_pageSize - 1);
  auto const hugeBase = reinterpret_cast<uintptr_t>(s_hugeRange.ptr);
  if (top > hugeBase) top = hugeBase;
  if (top <= s_stackLimit) return;
  size_t len = top - s_stackLimit;
  if (madvise((void*)s_stackLimit, len, MADV_DONTNEED) != 0 &&
      errno != EAGAIN) {
    fprintf(stderr, "%s failed to madvise with error %d\n", __func__, errno);
  }
}

ssize_t purgeable_bytes() {
#ifdef USE_JEMALLOC
  return s_pageSize * mallctl_all_pdirty();
#else
  return 0;
#endif
}

#if !defined USE_JEMALLOC || !defined HAVE_NUMA
void set_numa_binding(int node) {}
void* mallocx_on_node(size_t size, int node, size_t align) {
  void* ret = nullptr;
  posix_memalign(&ret, align, size);
  return ret;
}
#endif

#ifdef USE_JEMALLOC
unsigned low_arena = 0;
unsigned low_small_arena = 0;
unsigned high_arena = 0;
unsigned high_cold_arena = 0;
__thread unsigned local_arena = 0;

int low_arena_flags = 0;
int low_small_arena_flags = 0;
int high_arena_flags = 0;
int high_cold_arena_flags = 0;
__thread int local_arena_flags = 0;

// Keep track of the size of recently freed memory that might be in the high1g
// arena when it is disabled, so that we know when to reenable it.
std::atomic_uint g_highArenaRecentlyFreed;

alloc::BumpFileMapper* cold_file_mapper = nullptr;

// Explicit per-thread tcache arenas needing it.
// In jemalloc/include/jemalloc/jemalloc_macros.h.in, we have
// #define MALLOCX_TCACHE_NONE MALLOCX_TCACHE(-1)
__thread int high_arena_tcache = -1;
__thread int local_arena_tcache = -1;

static unsigned base_arena;

#ifdef HAVE_NUMA

void set_numa_binding(int node) {
  if (use_nuca && s_hugeRange.ptr) {
    return nuca_bind_thread();
  }
  if (node < 0) return;                 // thread not created from JobQueue
  s_numaNode = node;

  if (use_numa) {
    unsigned arena = base_arena + node;
    mallctlWrite("thread.arena", arena);
    numa_sched_setaffinity(0, node_to_cpu_mask[node]);
    numa_set_interleave_mask(numa_no_nodes_ptr);
    bitmask* nodes = numa_allocate_nodemask();
    numa_bitmask_setbit(nodes, node);
    numa_set_membind(nodes);
    numa_bitmask_free(nodes);
  }
}

void* mallocx_on_node(size_t size, int node, size_t align) {
  assert((align & (align - 1)) == 0);
  if (use_numa) {
    int flags = MALLOCX_ALIGN(align);
    if (node < 0) return mallocx(size, flags);
    int arena = base_arena + node;
    flags |= MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE;
    return mallocx(size, flags);
  }
  return mallocx(size, MALLOCX_ALIGN(align));
}

#endif // HAVE_NUMA

using namespace alloc;
static NEVER_INLINE
RangeMapper* getMapperChain(RangeState& range, unsigned n1GPages,
                            bool use2MPages, unsigned n2MPages,
                            bool useNormalPages,
                            int numaMask, short nextNode) {
  RangeMapper* head = nullptr;
  RangeMapper** ptail = &head;
  if (n1GPages) {
    if (get_huge1g_info().nr_hugepages) {
      // Existing settings express a GiB budget. ARM64's 64K-page kernels back
      // each GiB with two 512M hugetlb pages.
      auto const numHugePages = n1GPages * (size1g / huge1g_page_size());
      RangeMapper::append(
        ptail,
        new Bump1GMapper(range, numHugePages, numaMask, nextNode)
      );
    }
#ifdef __aarch64__
    else {
      auto const thpSize = THPPageSize();
      if (thpSize > size2m && thpSize <= size1g) {
        auto mapper = new BumpTHPMapper(range, n1GPages * (size1g / thpSize),
                                        numaMask, nextNode);
        RangeMapper::append(ptail, mapper);
      }
    }
#endif
  }
  if (use2MPages) {
    RangeMapper::append(ptail, new Bump2MMapper(range, n2MPages, numaMask));
  }
  if (useNormalPages) {
    RangeMapper::append(ptail, new BumpNormalMapper(range, 0, numaMask));
  }
  assertx(head);
  return head;
}

// Find the first 2M mapper for the range, and grant it some 2M page budget.
// Return the actual number of pages granted. The actual number can be different
// from the input, because some part of the range may have already been mapped
// in.
unsigned allocate2MPagesToRange(AddrRangeClass c, unsigned pages) {
  auto& range = getRange(c);
  auto mapper = range.getLowMapper();
  if (!mapper) return 0;
  // Search for the first 2M mapper.
  do {
    if (auto mapper2m = dynamic_cast<Bump2MMapper*>(mapper)) {
      const unsigned maxPages = (range.capacity() - range.mapped()) / size2m;
      auto const assigned = std::min(pages, maxPages);
      mapper2m->setMaxPages(assigned);
      return assigned;
    }
    mapper = mapper->next();
  } while (mapper);
  return 0;
}

void setup_low_arena(PageSpec s) {
  assert(reinterpret_cast<uintptr_t>(tc_start_address()) <= kLowArenaMinAddr);
  // Initialize mappers for the VeryLow and Low address ranges.
  auto& lowRange = getRange(AddrRangeClass::Low);
  auto& lowEmergencyRange = getRange(AddrRangeClass::LowEmergency);
  auto lowMapperNumHugePages = std::min(
    s.n1GPages,
    (uint32_t)(lowRange.capacity() / size1g));
  auto lowMapper =
    getMapperChain(lowRange,
                   lowMapperNumHugePages,
                   true, s.n2MPages,    // 2M
                   true,                // 4K
                   numa_node_set, 0);
  lowRange.setLowMapper(lowMapper);

  auto& lowSmallRange = getRange(AddrRangeClass::LowSmall);
  auto lowSmallMapper =
    getMapperChain(lowSmallRange,
                   lowMapperNumHugePages,
                   true, 0,             // 2M
                   true,                // 4K
                   numa_node_set, 0);
  lowSmallRange.setLowMapper(lowSmallMapper);

  auto& midRange = getRange(AddrRangeClass::Mid);
  auto midMapperNumHugePages = s.n1GPages -
    lowMapperNumHugePages;
  auto midMapper =
    getMapperChain(midRange,
                   midMapperNumHugePages,
                   true, 0,             // 2M
                   true,                // 4K
                   numa_node_set, 0);
  midRange.setLowMapper(midMapper);

  auto lowEmergencyMapper =
    new BumpEmergencyMapper([]{ kill(getpid(), SIGTERM);}, lowEmergencyRange);
  lowEmergencyRange.setLowMapper(lowEmergencyMapper);

  auto ma = LowArena::CreateAt(&g_lowArena);
  ma->appendMapper(lowMapper);
  ma->appendMapper(midMapper);
  ma->appendMapper(lowSmallMapper);
  ma->appendMapper(lowEmergencyMapper);
  low_arena = ma->id();
  low_arena_flags = MALLOCX_ARENA(low_arena) | MALLOCX_TCACHE_NONE;

  ma = LowArena::CreateAt(&g_lowSmallArena);
  ma->appendMapper(lowMapper);
  ma->appendMapper(lowSmallMapper);
  ma->appendMapper(lowEmergencyMapper);
  low_small_arena = ma->id();
  low_small_arena_flags = MALLOCX_ARENA(low_small_arena) | MALLOCX_TCACHE_NONE;
}

void setup_high_arena(PageSpec s) {
  auto& range = getRange(AddrRangeClass::High);
  auto mapper = getMapperChain(range, s.n1GPages,
                               true, s.n2MPages, // 2M pages can be added later
                               true,             // use normal pages
                               numa_node_set,
                               num_numa_nodes() / 2 + 1);
  range.setLowMapper(mapper);

  auto arena = HighArena::CreateAt(&g_highArena);
  arena->appendMapper(range.getLowMapper());
  high_arena = arena->id();
  // The flag will be combined with thread-local tcache
  high_arena_flags = MALLOCX_ARENA(high_arena);

  auto& fileRange = getRange(AddrRangeClass::HighCold);
  cold_file_mapper = new BumpFileMapper(fileRange);
  fileRange.setLowMapper(cold_file_mapper);
  auto coldMapper =
    new BumpNormalMapper<Direction::HighToLow>(range, 0, numa_node_set);
  range.setHighMapper(coldMapper);
  auto coldArena = HighArena::CreateAt(&g_coldArena);
  coldArena->appendMapper(cold_file_mapper);
  coldArena->appendMapper(coldMapper);
  high_cold_arena = coldArena->id();
  high_cold_arena_flags = MALLOCX_ARENA(high_cold_arena) | MALLOCX_TCACHE_NONE;
}

void setup_static_literals_arena() {
  // Map space for static literals.
  auto const addr = mmap(
    reinterpret_cast<void*>(kStaticLiteralsMinAddr),
    kStaticLiteralsMaxAddr - kStaticLiteralsMinAddr,
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
    -1,
    0
  );

  always_assert_flog(
    addr == reinterpret_cast<void*>(kStaticLiteralsMinAddr),
    "mmap failed: {}", folly::errnoStr(errno)
  );

  // If the space was stolen from the emergency area, we might have stolen more
  // than needed (RangeState constructor requires range to be 2M aligned).
  // Map the remaining space as inaccessible.
  auto constexpr stolenMinAddr =
    kStaticLiteralsMaxAddr - kLowEmergencyStolenByStaticLiterals;
  if constexpr (stolenMinAddr < kStaticLiteralsMinAddr) {
    auto const stolenAddr = mmap(
      reinterpret_cast<void*>(stolenMinAddr),
      kStaticLiteralsMinAddr - stolenMinAddr,
      PROT_NONE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
      -1,
      0
    );

    always_assert_flog(
      stolenAddr == reinterpret_cast<void*>(stolenMinAddr),
      "mmap failed: {}", folly::errnoStr(errno)
    );
  }
}

HHVM_ATTRIBUTE_WEAK void alloc_warn(const std::string& msg) {
  fprintf(stderr, "%s\n", msg.c_str());
}

// The arena reservations below pass their base to mmap() as a hint instead of
// MAP_FIXED, so the kernel silently relocates them when something else already
// occupies the range. Where they land is load-bearing (is_low_mem() classifies
// pointers by numeric address), so say so rather than continuing quietly.
static void check_arena_base(const char* what,
                             uintptr_t desired, uintptr_t actual) {
  if (actual == desired) return;
  alloc_warn(
    fmt::format("{}: reserved {:#x}, requested {:#x}; "
                   "another mapping already occupies that range",
                   what, actual, desired));
}

void setup_auto_arenas(PageSpec s) {
  size_t size = size1g * s.n1GPages + size2m * s.n2MPages;
  if (size == 0) return;
  // Give arena 0 some huge pages, starting at 2TB.
  auto ret = mmap(reinterpret_cast<void*>(kArena0Base),
                  size + size1g, PROT_NONE,
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                  -1, 0);
  if (ret == MAP_FAILED) {
    // Rounding MAP_FAILED up to the next 1GB boundary wraps to 0, which then
    // satisfies every assertion below and hands arena 0 the low address space
    // -- the binary, the TC and the low arena, all of which is_low_mem() calls
    // static. Huge pages for arena 0 are an optimization, so skip them.
    alloc_warn(
      fmt::format("arena 0 huge page range: mmap failed with error {}; "
                     "continuing without huge pages for arena 0", errno));
    return;
  }
  auto base = reinterpret_cast<uintptr_t>(ret);
  check_arena_base("arena 0 huge page range", kArena0Base, base);
  if (auto r = base % size1g) {         // align to 1G boundary
    base = base + size1g - r;
  }
  assertx(base % size1g == 0);

  RangeState& range = getRange(AddrRangeClass::Global);
  new (&range) RangeState(base, base + size, Reserved{});
  auto mapper = getMapperChain(range, s.n1GPages,
                               (bool)s.n2MPages, s.n2MPages,
                               false,   // don't use normal pages
                               numa_node_set, 0);
  if (!mapper) return;
  range.setLowMapper(mapper);
  unsigned auto_arenas = 0;
  mallctlRead<unsigned>("opt.narenas", &auto_arenas);
  for (unsigned i = 0; i < auto_arenas; ++i) {
    auto a = PreMappedArena::AttachTo(low_malloc(sizeof(PreMappedArena)),
                                      i, mapper);
    g_auto_arenas.push_back(a);
  }
}

// Set up extra arenas for use in non-VM threads, when we have short bursts of
// worker threads running, e.g., during deserialization of profile data.
static std::vector<std::pair<std::vector<DefaultArena*>,
                             std::atomic_uint*>> s_extra_arenas;
static unsigned s_extra_arena_per_node;
bool setup_extra_arenas(unsigned count) {
  if (count == 0) return false;
  // This may be called when we have many other threads running.  So hold the
  // lock while making changes.
  static std::mutex lock;
  std::lock_guard<std::mutex> g(lock);
  // only the first call allocate the arenas.
  if (!s_extra_arenas.empty()) {
    return count <= s_extra_arenas.size() * s_extra_arenas[0].first.size();
  }
  // `count` needs to be a multiple of `num_numa_nodes()`, if it isn't, we round
  // it up to make it easy to balance across nodes.
  auto const nNodes = std::max(1u, num_numa_nodes());
  s_extra_arena_per_node = (count + nNodes - 1) / nNodes;
  assert(s_extra_arena_per_node >= 1);
  s_extra_arenas.resize(nNodes);
  for (unsigned n = 0; n < nNodes; ++n) {
    s_extra_arenas[n].first.resize(s_extra_arena_per_node);
    auto constexpr kArenaSize =
      (sizeof(DefaultArena) + alignof(DefaultArena) - 1)
      / alignof(DefaultArena) * alignof(DefaultArena);
    auto const allocSize = kArenaSize * s_extra_arena_per_node
      + sizeof(std::atomic_uint);
    void* addr = mallocx_on_node(allocSize, n, alignof(DefaultArena));
    memset(addr, 0, allocSize);
    for (unsigned i = 0; i < s_extra_arena_per_node; ++i) {
      s_extra_arenas[n].first[i] = DefaultArena::CreateAt(addr);
      addr = (char*)addr + kArenaSize;
    }
    s_extra_arenas[n].second = static_cast<std::atomic_uint*>(addr);
  }
  return true;
}

DefaultArena* next_extra_arena(int node) {
  if (s_extra_arena_per_node == 0) return nullptr;
  if (node >= s_extra_arenas.size()) return nullptr;
  if (node < 0) node = 0;
  auto const n = static_cast<unsigned>(node);
  auto counter = s_extra_arenas[n].second;
  auto const next = counter->fetch_add(1, std::memory_order_acq_rel);
  return s_extra_arenas[n].first[next % s_extra_arena_per_node];
}

void arenas_thread_init() {
  if (high_arena_tcache == -1) {
    mallctlRead<int, true>("tcache.create", &high_arena_tcache);
  }
  if (local_arena_tcache == -1) {
    local_arena = get_local_arena(s_numaNode);
    if (local_arena) {
      mallctlRead<int, true>("tcache.create", &local_arena_tcache);
      local_arena_flags =
        MALLOCX_ARENA(local_arena) | MALLOCX_TCACHE(local_arena_tcache);
    }
  }
}

void arenas_thread_flush() {
  // It is OK if flushing fails
  if (high_arena_tcache != -1) {
    mallctlWrite<int, true>("tcache.flush", high_arena_tcache);
  }
  if (local_arena_tcache != -1) {
    mallctlWrite<int, true>("tcache.flush", local_arena_tcache);
  }
}

void arenas_thread_exit() {
  if (high_arena_tcache != -1) {
    mallctlWrite<int, true>("tcache.destroy", high_arena_tcache);
    high_arena_tcache = -1;
  }
  if (local_arena_tcache != -1) {
    mallctlWrite<int, true>("tcache.destroy", local_arena_tcache);
    local_arena_tcache = -1;
    // Ideally we shouldn't read local_arena_flags any more, but just in case.
    local_arena_flags = MALLOCX_ARENA(local_arena) | MALLOCX_TCACHE_NONE;
  }
}

std::vector<SlabManager*> s_slab_managers;

void setup_local_arenas(PageSpec spec, unsigned slabs) {
  s_slab_managers.reserve(num_numa_nodes());
  slabs /= num_numa_nodes();

  mallctlRead<unsigned>("arenas.narenas", &base_arena); // throw upon failure
  // The default one per node.
  for (int i = 0; i < num_numa_nodes(); i++) {
    if (use_numa) {
      unsigned arena = 0;
      mallctlRead<unsigned>("arenas.create", &arena);
      always_assert(arena == base_arena + i);
    }
    if (slabs) {
      auto mem = low_malloc(sizeof(SlabManager));
      s_slab_managers.push_back(new (mem) SlabManager);
    } else {
      s_slab_managers.push_back(nullptr);
    }
  }

#ifdef __x86_64__
  spec.n1GPages = std::min(spec.n1GPages, get_huge1g_info().nr_hugepages);
  spec.n2MPages = std::min(spec.n2MPages, get_huge2m_info().nr_hugepages);
#endif
  spec.n1GPages /= num_numa_nodes();
  spec.n2MPages /= num_numa_nodes();
  const size_t reserveSize =
    spec.n1GPages * size1g + spec.n2MPages * size2m;
  if (reserveSize == 0) return;

  g_local_arenas.resize(num_numa_nodes(), 0);
  for (unsigned i = 0; i < num_numa_nodes(); ++i) {
    static_assert(kLocalArenaMinAddr % size1g == 0, "");
    auto const desiredBase = kLocalArenaMinAddr + i * kLocalArenaSizeLimit;
    // Try to get the desired address range, but don't use MAP_FIXED.
    auto ret = mmap(reinterpret_cast<void*>(desiredBase),
                    reserveSize + size1g, PROT_NONE,
                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
                    -1, 0);
    if (ret == MAP_FAILED) {
      throw std::runtime_error{"mmap() failed to reserve address range"};
    }
    auto base = reinterpret_cast<uintptr_t>(ret);
    check_arena_base("request heap local arena", desiredBase, base);
    if (base % size1g) {                // adjust to start at 1GB boundary
      auto const newBase = (base + size1g - 1) & ~(size1g - 1);
      munmap(reinterpret_cast<void*>(base), newBase - base);
      base = newBase;
    }
    assert(base % size1g == 0);
    auto range = new RangeState(base, base + reserveSize, Reserved{});
    auto mapper = getMapperChain(*range,
                                 spec.n1GPages,
                                 (bool)spec.n2MPages,
                                 spec.n2MPages,
                                 false,       // don't use normal pages
                                 1u << i,
                                 i);
    range->setLowMapper(mapper);
    auto arena = PreMappedArena::CreateAt(low_malloc(sizeof(PreMappedArena)),
                                          mapper);

    // Allocate some slabs first, which are not given to the arena, but managed
    // separately by the slab manager.
    auto const totalSlabSize = std::min(slabs * kSlabSize, reserveSize);
    if (totalSlabSize) {
      auto slabRange = mapper->alloc(totalSlabSize, kSlabAlign);
      if (slabRange) {
        s_slab_managers[i]->addRange<true>(slabRange, totalSlabSize);
      }
    }
    if (totalSlabSize == reserveSize) continue;
    g_local_arenas[i] = arena;
  }
}

unsigned get_local_arena(uint32_t node) {
  if (node >= g_local_arenas.size()) return 0;
  auto const arena = g_local_arenas[node];
  if (arena == nullptr) return 0;
  return arena->id();
}

SlabManager* get_local_slab_manager(uint32_t node) {
  if (node >= s_slab_managers.size()) return nullptr;
  return s_slab_managers[node];
}

void shutdown_slab_managers() {
  for (auto slab_manager : s_slab_managers) {
    if (slab_manager) slab_manager->shutdown();
  }
}

#else

namespace {

struct BumpAllocatedArena {
  BumpAllocatedArena(uintptr_t minAddr, uintptr_t maxAddr, bool noReplace)
    : m_nextAddr(minAddr)
    , m_maxAddr(maxAddr) {
    auto const mapFixed = noReplace ? MAP_FIXED_NOREPLACE : MAP_FIXED;
    auto const addr = mmap(
      reinterpret_cast<void*>(minAddr),
      maxAddr - minAddr,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | mapFixed,
      -1,
      0
    );
    always_assert_flog(
      addr == reinterpret_cast<void*>(minAddr),
      "mmap failed: {}", folly::errnoStr(errno)
    );
  }

  void* alloc(size_t size) {
    // Align all allocations to 16 bytes.
    size = (size + 15) & ~15;
    auto const addr = m_nextAddr.fetch_add(size, std::memory_order_relaxed);
    if (addr + size > m_maxAddr) return nullptr;
    return reinterpret_cast<void*>(addr);
  }

private:
  std::atomic<uintptr_t> m_nextAddr;
  const uintptr_t m_maxAddr;
};

std::unique_ptr<BumpAllocatedArena> s_lowArena;
std::unique_ptr<BumpAllocatedArena> s_sharedArena;

void setup_arenas() {
#ifdef FOLLY_SANITIZE_ADDRESS
  // Query the sanitizer for its shadow mapping:
  //   shadow = (addr >> scale) + offset
  size_t shadowScale;
  size_t shadowOffset;
  __asan_get_shadow_mapping(&shadowScale, &shadowOffset);

  // In ASAN builds, we pick a subset of the space between 4GB and 32GB such
  // that its shadow space lies between 2GB and 4GB. Default ASAN config
  // makes this range 4G..~16GB.
  always_assert(shadowOffset <= kLowArenaMaxAddr);
  auto constexpr lowMinAddr = kLowArenaMaxAddr;
  auto const lowMaxAddr = (kLowArenaMaxAddr - shadowOffset) << shadowScale;
  always_assert(kLowArenaMinAddr <= lowMinAddr);
  always_assert(lowMinAddr < lowMaxAddr);
  always_assert(lowMaxAddr <= kMidArenaMaxAddr);

  // For the shared range, pick all space under 256GB that doesn't overlap with
  // its own shadow space and its shadow space doesn't overlap with low range.
  // Default ASAN confnig makes this range ~112GB..256GB.
  auto const sharedMinAddr = std::max(
    (kSharedMaxAddr >> shadowScale) + shadowOffset,  // ~32GB
    (lowMaxAddr - shadowOffset) << shadowScale       // ~112GB
  );
  auto constexpr sharedMaxAddr = kSharedMaxAddr;

  // ASAN already mapped this as PROT_NONE.
  auto constexpr noReplace = false;
#else
  auto constexpr lowMinAddr = kLowArenaMinAddr;
  auto constexpr lowMaxAddr = kMidArenaMaxAddr;
  auto constexpr sharedMinAddr = kMidArenaMaxAddr;
  auto constexpr sharedMaxAddr = kSharedMaxAddr;
  auto constexpr noReplace = true;
#endif

  always_assert(!s_lowArena);
  always_assert(!s_sharedArena);
  s_lowArena = std::make_unique<BumpAllocatedArena>(
    lowMinAddr, lowMaxAddr, noReplace);
  s_sharedArena = std::make_unique<BumpAllocatedArena>(
    sharedMinAddr, sharedMaxAddr, noReplace);

#ifdef FOLLY_SANITIZE_ADDRESS
  // Map zero-initialized read only shadow space for ASAN.
  auto const lowShadowMinAddr = (lowMinAddr >> shadowScale) + shadowOffset;
  auto const lowShadowMaxAddr = (lowMaxAddr >> shadowScale) + shadowOffset;
  auto const sharedShadowMinAddr =
    (sharedMinAddr >> shadowScale) + shadowOffset;
  auto const sharedShadowMaxAddr =
    (sharedMaxAddr >> shadowScale) + shadowOffset;
  always_assert(lowShadowMinAddr < lowShadowMaxAddr);
  always_assert(lowShadowMaxAddr == lowMinAddr);
  always_assert(lowMaxAddr <= sharedShadowMinAddr);
  always_assert(sharedShadowMinAddr < sharedShadowMaxAddr);
  always_assert(sharedShadowMaxAddr <= sharedMinAddr);

  auto const allocShadow = [](uintptr_t minAddr, uintptr_t maxAddr) {
    UNUSED auto const shadow = mmap(
      reinterpret_cast<void*>(minAddr),
      maxAddr - minAddr,
      PROT_READ,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
      -1,
      0
    );
    always_assert_flog(
      shadow == reinterpret_cast<void*>(minAddr),
      "mmap failed: {}", folly::errnoStr(errno)
    );
  };

  allocShadow(lowShadowMinAddr, lowShadowMaxAddr);
  allocShadow(sharedShadowMinAddr, sharedShadowMaxAddr);
#endif

  auto const staticLiteralsAddr =
    low_bump_malloc(kStaticLiteralsMaxAddr - kStaticLiteralsMinAddr);
  always_assert_flog(
    reinterpret_cast<uintptr_t>(staticLiteralsAddr) == kStaticLiteralsMinAddr,
    "static literals allocated at wrong location"
  );

  if (debug) {
    auto const ptr1 = malloc(1);
    auto const ptr2 = malloc(1 << 20);
    assertx(reinterpret_cast<uintptr_t>(ptr1) >= kSharedMaxAddr);
    assertx(reinterpret_cast<uintptr_t>(ptr2) >= kSharedMaxAddr);
    free(ptr1);
    free(ptr2);
  }
}

}

void* low_bump_malloc(size_t size) {
  return s_lowArena->alloc(size);
}

void* low_bump_realloc(void* ptr, size_t size) {
  auto newptr = low_malloc(size);
  if (newptr == nullptr) return nullptr;
  memcpy(newptr, ptr, size);
  return newptr;
}

void low_bump_free(void* ptr) {
  // Do nothing. It is very very rare that we actually try to free something
  // and if we do just have it leak
}

void* shared_bump_malloc(size_t size) {
  return s_sharedArena->alloc(size);
}

void* shared_bump_realloc(void* ptr, size_t size) {
  auto newptr = shared_malloc(size);
  if (newptr == nullptr) return nullptr;
  memcpy(newptr, ptr, size);
  return newptr;
}

void shared_bump_free(void* ptr) {
  // Do nothing. It is very very rare that we actually try to free something
  // and if we do just have it leak
}

#endif // USE_JEMALLOC

ssize_t get_free_slab_bytes() {
  ssize_t bytes = 0;
#ifdef USE_JEMALLOC
  for (auto const slabManager : s_slab_managers) {
    if (slabManager) {
      bytes += slabManager->bytes();
    }
  }
#endif // USE_JEMALLOC
  return bytes;
}

struct JEMallocInitializer {
  JEMallocInitializer() {
    // The following comes from malloc_extension.cc in google-perftools
#ifdef __GLIBC__
    // GNU libc++ versions 3.3 and 3.4 obey the environment variables
    // GLIBCPP_FORCE_NEW and GLIBCXX_FORCE_NEW respectively.  Setting
    // one of these variables forces the STL default allocator to call
    // new() or delete() for each allocation or deletion.  Otherwise
    // the STL allocator tries to avoid the high cost of doing
    // allocations by pooling memory internally.
    setenv("GLIBCPP_FORCE_NEW", "1", false /* no overwrite*/);
    setenv("GLIBCXX_FORCE_NEW", "1", false /* no overwrite*/);

    // Now we need to make the setenv 'stick', which it may not do since
    // the env is flaky before main() is called.  But luckily stl only
    // looks at this env var the first time it tries to do an alloc, and
    // caches what it finds.  So we just cause an stl alloc here.
    std::string dummy("I need to be allocated");
    dummy += "!";         // so the definition of dummy isn't optimized out
#endif  /* __GLIBC__ */

    // Enable backtracing through PHP frames (t9814472).
    setenv("UNW_RBP_ALWAYS_VALID", "1", false);

    init_numa();
#ifdef USE_JEMALLOC
    unsigned low_1g_pages = 0;
    if (char* buffer = getenv("HHVM_LOW_1G_PAGE")) {
      if (!sscanf(buffer, "%u", &low_1g_pages)) {
        fprintf(stderr,
                "Bad environment variable HHVM_LOW_1G_PAGE: %s\n", buffer);
        abort();
      }
    }
    unsigned high_1g_pages = 0;
    if (char* buffer = getenv("HHVM_HIGH_1G_PAGE")) {
      if (!sscanf(buffer, "%u", &high_1g_pages)) {
        fprintf(stderr,
                "Bad environment variable HHVM_HIGH_1G_PAGE: %s\n", buffer);
        abort();
      }
    }
    unsigned low_2m_pages = 0;
    if (char* buffer = getenv("HHVM_LOW_2M_PAGE")) {
      if (!sscanf(buffer, "%u", &low_2m_pages)) {
        fprintf(stderr,
                "Bad environment variable HHVM_LOW_2M_PAGE: %s\n", buffer);
        abort();
      }
    }
    unsigned high_2m_pages = 0;
    if (char* buffer = getenv("HHVM_HIGH_2M_PAGE")) {
      if (!sscanf(buffer, "%u", &high_2m_pages)) {
        fprintf(stderr,
                "Bad environment variable HHVM_HIGH_2M_PAGE: %s\n", buffer);
        abort();
      }
    }

    // Do some reallocation between low and high 1G arenas based on the total
    // number of pages reserved.
    HugePageInfo info = get_huge1g_info();
    unsigned remaining = static_cast<unsigned>(info.nr_hugepages);
    if (low_1g_pages > 2) low_1g_pages = 2;
#ifdef __x86_64__
    auto const origLow1G = low_1g_pages;
    if (remaining == 0) {
      low_1g_pages = high_1g_pages = 0;
    }

    if (low_1g_pages > 0) {
      if (low_1g_pages + high_1g_pages > remaining) {
        low_1g_pages = 1;
      }
      assert(remaining >= low_1g_pages);
      remaining -= low_1g_pages;
    }

    if (origLow1G) {
      fprintf(stderr,
              "using %u (specified %u) 1G huge pages for low arena\n",
              low_1g_pages, origLow1G);
    }
#else
    if (low_1g_pages && !remaining) {
      fprintf(stderr,
              "specified %u 1G huge pages for low arena but the host doesn't "
              "have any, will try to use THP\n",
              low_1g_pages);
    }
#endif

    setup_low_arena({low_1g_pages, low_2m_pages});
#ifdef __x86_64__
    auto const origHigh1G = high_1g_pages;
    if (high_1g_pages > remaining) {
      high_1g_pages = remaining;
    }
    if (origHigh1G) {
      fprintf(stderr,
              "using %u (specified %u) 1G huge pages for high arena\n",
              high_1g_pages, origHigh1G);
    }
#else
    if (high_1g_pages && !remaining) {
      fprintf(stderr,
              "specified %u 1G huge pages for high arena but the host doesn't "
              "have any, will try to use THP\n",
              high_1g_pages);
    }
#endif
    setup_high_arena({high_1g_pages, high_2m_pages});
    setup_static_literals_arena();
    // Make sure high/low arenas are available to the current thread.
    arenas_thread_init();

    // Initialize global mibs
    init_mallctl_mibs();
#else
    setup_arenas();
#endif
  }
};

#if defined(__GNUC__) || defined(__clang__)
// Construct this object before any others.
// 101 is the highest priority allowed by the init_priority attribute.
// http://gcc.gnu.org/onlinedocs/gcc-4.0.4/gcc/C_002b_002b-Attributes.html
#define MAX_CONSTRUCTOR_PRIORITY __attribute__((init_priority(101)))
#else
#define MAX_CONSTRUCTOR_PRIORITY
#endif

static JEMallocInitializer initJEMalloc MAX_CONSTRUCTOR_PRIORITY;

void low_2m_pages(uint32_t pages) {
#if USE_JEMALLOC
  pages -= allocate2MPagesToRange(AddrRangeClass::Low, pages);
  allocate2MPagesToRange(AddrRangeClass::Mid, pages);
#endif
}

void high_2m_pages(uint32_t pages) {
#if USE_JEMALLOC
  allocate2MPagesToRange(AddrRangeClass::High, pages);
#endif
}

void enable_high_cold_file() {
#if USE_JEMALLOC
  if (cold_file_mapper) {
    cold_file_mapper->enable();
  }
#endif
}

void set_cold_file_dir(const char* dir) {
#if USE_JEMALLOC
  if (cold_file_mapper) {
    cold_file_mapper->setDirectory(dir);
  }
#endif
}

static SwappableReadonlyArena* s_swappable_readonly_arena = nullptr;
void setup_swappable_readonly_arena(uint32_t chunk_size) {
  s_swappable_readonly_arena = new SwappableReadonlyArena(
    chunk_size, ServiceData::createCounter("admin.swappable-roarena-cap"));
}
SwappableReadonlyArena* get_swappable_readonly_arena() {
  return s_swappable_readonly_arena;
}

///////////////////////////////////////////////////////////////////////////////
}

extern "C" {
#if USE_JEMALLOC
  // MALLOC_CONF
  const char* malloc_conf = "narenas:8,lg_tcache_max:16"
    ",background_thread:false"
    ",metadata_thp:disabled,bin_shards:1-256:16|320-14336:4|4096-4096:16"
    ;
#endif

#ifdef FOLLY_SANITIZE_ADDRESS
  extern const char* const kAsanDefaultOptions =
    "check_initialization_order=1:detect_invalid_pointer_pairs=1:detect_leaks=0:"
    "detect_odr_violation=1:detect_stack_use_after_return=0:handle_segv=0:"
    "print_scariness=1:print_suppressions=0:strict_init_order=1:"
    "allow_user_segv_handler=1:alloc_dealloc_mismatch=0:protect_shadow_gap=0";
#endif
}
