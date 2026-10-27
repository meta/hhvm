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

#include "hphp/runtime/base/preg.h"

#include <atomic>
#include <mutex>
#include <pcre.h>
#include <onigposix.h>
#include <utility>

#include <fmt/format.h>
#include <folly/AtomicHashArray.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/base/zend-functions.h"
#include "hphp/runtime/vm/debug/debug.h"

#include "hphp/runtime/ext/std/ext_std_function.h"
#include "hphp/runtime/ext/string/ext_string.h"

#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/vtune-jit.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/pcre.h"
#include "hphp/util/concurrent-scalable-cache.h"
#include "hphp/util/logger.h"

#include <folly/FileUtil.h>
#include <folly/json/json.h>

/* Only defined in pcre >= 8.32 */
#ifndef PCRE_STUDY_JIT_COMPILE
# define PCRE_STUDY_JIT_COMPILE 0
#endif

namespace HPHP {

TRACE_SET_MOD(preg)

using jit::TCA;

///////////////////////////////////////////////////////////////////////////////
// PCREglobals definition

PCREglobals::PCREglobals() {
  jit_stack = pcre_jit_stack_alloc(32768, 524288);
  // Set these to handle uses of pcre prior to PcreExtension::threadInit
  // In particular, for matching tier overrides during RuntimeOption::Load
  preg_backtrack_limit = Cfg::PCRE::BacktrackLimit;
  preg_recursion_limit = Cfg::PCRE::RecursionLimit;
}

PCREglobals::~PCREglobals() {
  pcre_jit_stack_free(jit_stack);
}

///////////////////////////////////////////////////////////////////////////////
// PCRECache definition

struct PCRECache {
  using EntryPtr = std::shared_ptr<const pcre_cache_entry>;
  using TempKeyCache = std::unique_ptr<LRUCacheKey>;

  enum class CacheKind {
    Static,
    Lru,
    Scalable
  };

private:
  struct ahm_string_data_same {
    bool operator()(const StringData* s1, const StringData* s2) {
      // ahm uses -1, -2, -3 as magic values
      return int64_t(s1) > 0 && (s1 == s2 || s1->same(s2));
    }
  };

  using StaticCache = folly::AtomicHashArray<StringData*, const pcre_cache_entry*,
          string_data_hash, ahm_string_data_same>;
  using LRUCache = ConcurrentLRUCache<LRUCacheKey, EntryPtr,
          LRUCacheKey::HashCompare>;
  using ScalableCache = ConcurrentScalableCache<LRUCacheKey, EntryPtr,
          LRUCacheKey::HashCompare>;
  using StaticCachePair = StaticCache::value_type;

public:
  struct Accessor {
    Accessor()
      : m_kind(Kind::Empty)
    {}

    ~Accessor() {
      switch (m_kind) {
        case Kind::Empty:
        case Kind::Ptr:
          break;
        case Kind::SmartPtr:
          m_u.smart_ptr.~EntryPtr();
          break;
        case Kind::AccessorKind:
          m_u.accessor.~ConstAccessor();
          break;
      }
    }

    Accessor& operator=(const pcre_cache_entry* ptr) {
      assertx(m_kind == Kind::Empty || m_kind == Kind::Ptr);
      m_kind = Kind::Ptr;
      m_u.ptr = ptr;
      return *this;
    }

    Accessor& operator=(EntryPtr&& ep) {
      switch (m_kind) {
        case Kind::AccessorKind:
          m_u.accessor.~ConstAccessor();
          [[fallthrough]];
        case Kind::Empty:
        case Kind::Ptr:
          m_kind = Kind::SmartPtr;
          new (&m_u.smart_ptr) EntryPtr(std::move(ep));
          break;
        case Kind::SmartPtr:
          m_u.smart_ptr = std::move(ep);
          break;
      }
      return *this;
    }

    // No assignment from LRUCache::ConstAccessor since it is non-copyable
    // Use resetToLRU instead
    LRUCache::ConstAccessor& resetToLRU() {
      switch (m_kind) {
        case Kind::SmartPtr:
          m_u.smart_ptr.~EntryPtr();
          [[fallthrough]];
        case Kind::Empty:
        case Kind::Ptr:
          m_kind = Kind::AccessorKind;
          new (&m_u.accessor) LRUCache::ConstAccessor();
          break;
        case Kind::AccessorKind:
          break;
      }
      return m_u.accessor;
    }

    const pcre_cache_entry* get() {
      switch (m_kind) {
        case Kind::Empty:    return nullptr;
        case Kind::Ptr:      return m_u.ptr;
        case Kind::SmartPtr: return m_u.smart_ptr.get();
        case Kind::AccessorKind: return m_u.accessor->get();
      }
      always_assert(false);
    }

    const EntryPtr& entryPtr() const {
      assertx(m_kind == Kind::SmartPtr);
      return m_u.smart_ptr;
    }

   private:
    enum class Kind : uint8_t {
      Empty,
      Ptr,
      SmartPtr,
      AccessorKind,
    };

    union Ptr {
       Ptr() {}
      ~Ptr() {}

      const pcre_cache_entry* ptr;
      EntryPtr smart_ptr;
      LRUCache::ConstAccessor accessor;
    };

    Ptr m_u;
    Kind m_kind;
  };

  PCRECache()
    : m_kind(CacheKind::Static), m_staticCache(nullptr)
  {
    reinit(CacheKind::Static);
  }

  ~PCRECache() {
    if (m_kind == CacheKind::Static && m_staticCache.load()) {
      DestroyStatic(m_staticCache);
    }
  }

  void reinit(CacheKind kind);
  bool find(Accessor& accessor, const StringData* key,
            TempKeyCache& keyCache);
  void insert(Accessor& accessor, StringData* regex,
              TempKeyCache& keyCache, const pcre_cache_entry* ent);
  void dump(folly::File& file);
  size_t size() const;

private:
  void clearStatic();

  static void DestroyStatic(StaticCache* cache);
  static StaticCache* CreateStatic();

  CacheKind m_kind;
  std::atomic<StaticCache*> m_staticCache;
  std::unique_ptr<LRUCache> m_lruCache;
  std::unique_ptr<ScalableCache> m_scalableCache;
  std::atomic<time_t> m_expire{};
  std::mutex m_clearMutex;
};

///////////////////////////////////////////////////////////////////////////////
// Data

RDS_LOCAL(PCREglobals, tl_pcre_globals);

static PCRECache s_pcreCache;
static auto pc_counter = ServiceData::createCounter("admin.pcre-cache");

// The last pcre error code is available for the whole thread.
static RDS_LOCAL(int, rl_last_error_code);

///////////////////////////////////////////////////////////////////////////////
// pcre_cache_entry implementation

pcre_cache_entry::~pcre_cache_entry() {
  if (extra) {
#if PCRE_MAJOR < 8 || (PCRE_MAJOR == 8 && PCRE_MINOR < 20)
    free(extra);
#else
    pcre_free_study(extra);
#endif
  }
  free(subpat_names);
  pcre_free(re);
}

bool literalOptions(int options) {
  constexpr int mask =
    PCRE_ANCHORED | PCRE_CASELESS |
    PCRE_DOLLAR_ENDONLY | PCRE_NOTEMPTY;
  return !(options & ~mask);
}

pcre_literal_data::pcre_literal_data(const char* pattern, int coptions) {
  if (!literalOptions(coptions)) return;

  auto p = pattern;
  options = coptions;

  if (*p == '^') {
    match_start_of_line = true;
    p++;
  }

  std::string pattern_buffer;
  while (isalnum((unsigned char)*p) || (*p && strchr("/\\ :-_", *p))) {
    // backslash + alphanumeric character --> not a literal (i.e. \d).
    // backslash + non-alphanumeric character --> literal symbol (i.e. \.)
    if (*p == '\\') {
      if (!p[1] || isalnum((unsigned char)p[1])) {
        break;
      } else {
        p++;
      }
    }
    pattern_buffer += *p++;
  }
  if (*p == '$') {
    options |= PCRE_DOLLAR_ENDONLY;
    p++;
  }
  if (!*p) {
    /* This is an encoding of a literal string. */
     ITRACE(2, "Literal pattern: {}\n", pattern_buffer);
    literal_str = std::move(pattern_buffer);
  }
}

bool pcre_literal_data::isLiteral() const {
  return literal_str.has_value();
}

bool pcre_literal_data::matches(const StringData* subject,
                                int pos,
                                int* offsets,
                                int extra_options) const {
  assertx(isLiteral() && literalOptions(extra_options));
  assertx(pos >= 0);

  // Subject must be at least as long as the literal pattern
  // for a match to occur.
  if (subject->size() < literal_str->length() + pos) {
    return false;
  }

  size_t literal_strlen = literal_str->length();
  auto const g_empty = (options | extra_options) & PCRE_NOTEMPTY;
  if (g_empty && !literal_strlen) return false;
  auto const subject_c = subject->data();
  auto const literal_c = literal_str->c_str();

  // Compare the literal pattern at an offset of the subject.
  auto const subject_substr = subject_c + pos;

  auto const match_start = [&]() {
    if (match_end() && (subject->size() - pos) != literal_strlen) {
      return false;
    }
    // If only matching the start (^), compare the strings
    // for the length of the literal pattern.
    if (case_insensitive() ?
        bstrcaseeq(subject_substr, literal_c, literal_strlen) :
        memcmp(subject_substr, literal_c, literal_strlen) == 0) {
      offsets[0] = pos * sizeof(char);
      offsets[1] = offsets[0] + literal_strlen * sizeof(char);
      return true;
    }
    return false;
  };

  if (match_start_of_line) {
    return !pos && match_start();
  } else if (match_start_of_string()) {
    return match_start();
  } else if (match_end()) {
    // Compare the literal pattern against the tail end of the subject.
    auto const subject_tail = subject_c + (subject->size() - literal_strlen);
    if (case_insensitive() ?
        bstrcaseeq(subject_tail, literal_c, literal_strlen) :
        memcmp(subject_tail, literal_c, literal_strlen) == 0) {
      offsets[0] = (subject->size() - literal_strlen) * sizeof(char);
      offsets[1] = subject->size() * sizeof(char);
      return true;
    }
  } else {
    if (!literal_strlen) {
      offsets[0] = offsets[1] = pos;
      return true;
    }
    // Check if the literal pattern occurs as a substring of the subject.
    auto const subject_str = StrNR(subject);
    auto const find_response = subject_str.asString().find(
      *literal_str, pos, !case_insensitive());
    if (find_response >= 0) {
      offsets[0] = find_response * sizeof(char);
      offsets[1] = offsets[0] + literal_strlen * sizeof(char);
      return true;
    }
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////
// PCRECache implementation

PCRECache::StaticCache* PCRECache::CreateStatic() {
  StaticCache::Config config;
  config.maxLoadFactor = 0.5;
  return StaticCache::create(Cfg::PCRE::TableSize, config).release();
}

void PCRECache::DestroyStatic(StaticCache* cache) {
  // We delete uncounted keys while iterating the cache, which is OK for
  // AtomicHashArray, but not OK for other containers, such as
  // std::unordered_map.  If you change the cache type make sure that property
  // holds or fix this function.
  static_assert(std::is_same<PCRECache::StaticCache,
      folly::AtomicHashArray<StringData*, const pcre_cache_entry*,
                             string_data_hash, ahm_string_data_same>>::value,
      "StaticCache must be an AtomicHashArray or this destructor is wrong.");
  for (auto& it : *cache) {
    DecRefSharedString(it.first);
    delete it.second;
  }
  StaticCache::destroy(cache);
}

void PCRECache::reinit(CacheKind kind) {
  switch (m_kind) {
    case CacheKind::Static:
      if (m_staticCache.load()) {
        DestroyStatic(m_staticCache);
        m_staticCache = nullptr;
      }
      break;
    case CacheKind::Lru:
      m_lruCache.reset();
      break;
    case CacheKind::Scalable:
      m_scalableCache.reset();
      break;
  }
  m_kind = kind;

  switch (kind) {
    case CacheKind::Static:
      m_staticCache = CreateStatic();
      m_expire = time(nullptr) + Cfg::PCRE::ExpireInterval;
      break;
    case CacheKind::Lru:
      m_lruCache.reset(new LRUCache(Cfg::PCRE::TableSize));
      break;
    case CacheKind::Scalable:
      m_scalableCache.reset(
        new ScalableCache(Cfg::PCRE::TableSize));
      break;
  }
}

bool PCRECache::find(Accessor& accessor,
                     const StringData* regex,
                     TempKeyCache& keyCache)
{
  switch (m_kind) {
    case CacheKind::Static:
      {
        assertx(m_staticCache.load());
        StaticCache::iterator it;
        auto cache = m_staticCache.load(std::memory_order_acquire);
        if ((it = cache->find(regex)) != cache->end()) {
          accessor = it->second;
          return true;
        }
        return false;
      }
    case CacheKind::Lru:
    case CacheKind::Scalable:
      {
        if (!keyCache) {
          keyCache.reset(new LRUCacheKey(regex->data(), regex->size()));
        }
        bool found;
        if (m_kind == CacheKind::Lru) {
          found = m_lruCache->find(accessor.resetToLRU(), *keyCache);
        } else {
          found = m_scalableCache->find(accessor.resetToLRU(), *keyCache);
        }
        return found;
      }
  }
  always_assert(false);
}

void PCRECache::clearStatic() {
  std::unique_lock<std::mutex> lock(m_clearMutex, std::try_to_lock);
  if (!lock) return;

  auto newExpire = time(nullptr) + Cfg::PCRE::ExpireInterval;
  m_expire.store(newExpire, std::memory_order_release);

  auto tmpMap = CreateStatic();
  tmpMap = m_staticCache.exchange(tmpMap, std::memory_order_acq_rel);

  Treadmill::enqueue([tmpMap]() {
      DestroyStatic(tmpMap);
   });
}

void PCRECache::insert(
  Accessor& accessor,
  StringData* regex,
  TempKeyCache& keyCache,
  const pcre_cache_entry* ent
) {
  switch (m_kind) {
    case CacheKind::Static:
      {
        assertx(m_staticCache.load());
        // Clear the cache if we haven't refreshed it in a while
        if (time(nullptr) > m_expire) {
          clearStatic();
        }
        auto const cache = m_staticCache.load(std::memory_order_acquire);
        auto const key = !regex->persistentIncRef()
          ? StringData::MakeShared(regex->slice())
          : regex;
        auto pair = cache->insert(StaticCachePair(key, ent));
        if (pair.second) {
          // Inserted, container owns the pointer
          accessor = ent;
        } else {
          // Not inserted, caller needs to own the pointer
          DecRefSharedString(key);
          accessor = EntryPtr(ent);
        }
      }
      break;
    case CacheKind::Lru:
    case CacheKind::Scalable:
      {
        if (!keyCache) {
          keyCache.reset(new LRUCacheKey(regex->data(), regex->size()));
        }
        // Pointer ownership is shared between container and caller
        accessor = EntryPtr(ent);
        if (m_kind == CacheKind::Lru) {
          m_lruCache->insert(*keyCache, accessor.entryPtr());
        } else {
          m_scalableCache->insert(*keyCache, accessor.entryPtr());
        }
      }
      break;
  }
}

void PCRECache::dump(folly::File& file) {
  switch (m_kind) {
    case CacheKind::Static:
      for (auto& it : *m_staticCache) {
        folly::writeFull(file.fd(), it.first->data(), it.first->size());
        folly::writeFull(file.fd(), "\n", 1);
      }
      break;
    case CacheKind::Lru:
    case CacheKind::Scalable:
      {
        std::vector<LRUCacheKey> keys;
        if (m_kind == CacheKind::Lru) {
          m_lruCache->snapshotKeys(keys);
        } else {
          m_scalableCache->snapshotKeys(keys);
        }
        for (auto& key: keys) {
          folly::writeFull(file.fd(), key.data(), key.size());
          folly::writeFull(file.fd(), "\n", 1);
        }
      }
      break;
  }
}

size_t PCRECache::size() const {
  switch (m_kind) {
    case CacheKind::Static:
      return m_staticCache.load(std::memory_order_acquire)->size();
    case CacheKind::Lru:
      return m_lruCache->size();
    case CacheKind::Scalable:
     return m_scalableCache->size();
  }
  always_assert(false);
}

///////////////////////////////////////////////////////////////////////////////
// Public interface and helper functions

void pcre_reinit() {
  PCRECache::CacheKind kind;
  if (Cfg::PCRE::CacheType == "static") {
    kind = PCRECache::CacheKind::Static;
  } else if (Cfg::PCRE::CacheType == "lru") {
    kind = PCRECache::CacheKind::Lru;
  } else if (Cfg::PCRE::CacheType == "scalable") {
    kind = PCRECache::CacheKind::Scalable;
  } else {
    Logger::Warning("Eval.PCRECacheType should be either static, "
                    "lru or scalable");
    kind = PCRECache::CacheKind::Scalable;
  }
  pc_counter->setValue(0);
  s_pcreCache.reinit(kind);
}

void pcre_init() {
}

void pcre_dump_cache(folly::File& file) {
  s_pcreCache.dump(file);
}

static pcre_jit_stack* alloc_jit_stack(void* /*data*/) {
  return tl_pcre_globals->jit_stack;
}

namespace {

template<bool useSmartFree = false>
struct FreeHelperImpl {
  explicit FreeHelperImpl(void* p) : p(p) {}
  ~FreeHelperImpl() {
    useSmartFree ? req::free(p) : free(p);
  }

  FreeHelperImpl(const FreeHelperImpl&) = delete;
  FreeHelperImpl& operator=(const FreeHelperImpl&) = delete;

private:
  void* p;
};

using SmartFreeHelper = FreeHelperImpl<true>;
}

static void init_local_extra(pcre_extra* local, pcre_extra* shared) {
  if (shared) {
    memcpy(local, shared, sizeof(pcre_extra));
  } else {
    memset(local, 0, sizeof(pcre_extra));
    local->flags = PCRE_EXTRA_MATCH_LIMIT | PCRE_EXTRA_MATCH_LIMIT_RECURSION;
  }
  local->match_limit = tl_pcre_globals->preg_backtrack_limit;
  local->match_limit_recursion = tl_pcre_globals->preg_recursion_limit;
}

static const char* const*
get_subpat_names(const pcre_cache_entry* pce) {
  assertx(!pce->literal_data);
  char **subpat_names = pce->subpat_names.load(std::memory_order_acquire);
  if (subpat_names) return subpat_names;

  /*
  * Build a mapping from subpattern numbers to their names. We will always
  * allocate the table, even though there may be no named subpatterns. This
  * avoids somewhat more complicated logic in the inner loops.
  */
  pcre_extra extra;
  init_local_extra(&extra, pce->extra);

  int name_count;

  subpat_names = (char **)calloc(pce->num_subpats, sizeof(char *));
  int rc = pcre_fullinfo(pce->re, &extra, PCRE_INFO_NAMECOUNT, &name_count);
  if (rc < 0) {
    raise_warning("Internal pcre_fullinfo() error %d", rc);
    return nullptr;
  }
  if (name_count > 0) {
    int name_size, ni = 0;
    unsigned short name_idx;
    char* name_table;
    int rc1, rc2;

    rc1 = pcre_fullinfo(pce->re, &extra, PCRE_INFO_NAMETABLE, &name_table);
    rc2 = pcre_fullinfo(pce->re, &extra, PCRE_INFO_NAMEENTRYSIZE, &name_size);
    rc = rc2 ? rc2 : rc1;
    if (rc < 0) {
      raise_warning("Internal pcre_fullinfo() error %d", rc);
      return nullptr;
    }
    // The table returned by PCRE_INFO_NAMETABLE is an array of fixed length
    // strings of size PCRE_INFO_NAMEENTRYSIZE.  The first two bytes are a
    // big-endian uint16_t defining the array index followed by the
    // zero-terminated name string.
    // (See https://www.pcre.org/original/doc/html/pcreapi.html)
    while (ni++ < name_count) {
      name_idx = 0x100 * (unsigned char)name_table[0] +
                 (unsigned char)name_table[1];
      subpat_names[name_idx] = name_table + 2;
      if (is_numeric_string(subpat_names[name_idx],
                            strlen(subpat_names[name_idx]),
                            nullptr, nullptr, 0) != KindOfNull) {
        raise_warning("Numeric named subpatterns are not allowed");
        return nullptr;
      }
      name_table += name_size;
    }
  }
  // Store subpat_names into the cache entry
  char **expected = nullptr;
  if (!pce->subpat_names.compare_exchange_strong(expected, subpat_names)) {
    // Another thread stored subpat_names already. The array created by the
    // other thread is now in expected, return it instead and delete the one
    // we just made.
    free(subpat_names);
    return expected;
  }
  return subpat_names;
}

static bool get_pcre_fullinfo(pcre_cache_entry* pce) {
  pcre_extra extra;
  init_local_extra(&extra, pce->extra);

  /* Calculate the size of the offsets array*/
  int rc = pcre_fullinfo(pce->re, &extra, PCRE_INFO_CAPTURECOUNT,
                         &pce->num_subpats);
  if (rc < 0) {
    raise_warning("Internal pcre_fullinfo() error %d", rc);
    return false;
  }
  pce->num_subpats++;
  return true;
}

static void raise_warning_or_create_error_message(bool warn,
                                                  StringData*& error_message,
                                                  const std::string& msg) {
  if (LIKELY(warn)) {
    raise_warning(msg);
  } else {
    if (error_message) decRefStr(error_message); // decref old StringData
    error_message = StringData::Make(msg);
  }
}

static bool
pcre_get_compiled_regex_cache(PCRECache::Accessor& accessor,
                              StringData* regex,
                              bool warn,
                              StringData*& error_message) {
  PCRECache::TempKeyCache tkc;

  /* Try to lookup the cached regex entry, and if successful, just pass
     back the compiled pattern, otherwise go on and compile it. */
  if (s_pcreCache.find(accessor, regex, tkc)) return true;

  /* Parse through the leading whitespace, and display a warning if we
     get to the end without encountering a delimiter. */
  const char *p = regex->data();
  while (isspace((int)*(unsigned char *)p)) p++;
  if (*p == 0) {
    raise_warning_or_create_error_message(warn, error_message,
                                          "Empty regular expression");
    return false;
  }

  /* Get the delimiter and display a warning if it is alphanumeric
     or a backslash. */
  char delimiter = *p++;
  if (isalnum((int)*(unsigned char *)&delimiter) || delimiter == '\\') {
    raise_warning_or_create_error_message(warn, error_message,
                                          "Delimiter must not be alphanumeric or backslash");
    return false;
  }

  char start_delimiter = delimiter;
  const char *pp = strchr("([{< )]}> )]}>", delimiter);
  if (pp) {
    delimiter = pp[5];
  }
  char end_delimiter = delimiter;

  if (start_delimiter == end_delimiter) {
    /* We need to iterate through the pattern, searching for the ending
     * delimiter, but skipping the backslashed delimiters. If the ending
     * delimiter is not found, display a warning. */
    pp = p;
    while (*pp != 0) {
      if (*pp == '\\' && pp[1] != 0) pp++;
      else if (*pp == delimiter)
        break;
      pp++;
    }
    if (*pp == 0) {
      std::string msg = fmt::format("No ending delimiter '{}' found: [{}]",
                                       delimiter, regex->data());
      raise_warning_or_create_error_message(warn, error_message, msg);
      return false;
    }
  } else {
    /* We iterate through the pattern, searching for the matching ending
     * delimiter. For each matching starting delimiter, we increment nesting
     * level, and decrement it for each matching ending delimiter. If we
     * reach the end of the pattern without matching, display a warning.
     */
    int brackets = 1; // brackets nesting level
    pp = p;
    while (*pp != 0) {
      if (*pp == '\\' && pp[1] != 0) pp++;
      else if (*pp == end_delimiter && --brackets <= 0)
        break;
      else if (*pp == start_delimiter)
        brackets++;
      pp++;
    }
    if (*pp == 0) {
      std::string msg = fmt::format("No ending matching delimiter '{}' found: [{}]",
                                       end_delimiter, regex->data());
      raise_warning_or_create_error_message(warn, error_message, msg);
      return false;
    }
  }

  /* Make a copy of the actual pattern. */
  OptString spattern(p, pp-p, CopyString);
  const char *pattern = spattern.data();

  /* Move on to the options */
  pp++;

  /* Parse through the options, setting appropriate flags.  Display
     a warning if we encounter an unknown modifier. */
  int coptions = 0;
  int poptions = 0;
  bool do_study = false;
  while (*pp != 0) {
    switch (*pp++) {
      /* Perl compatible options */
    case 'i':  coptions |= PCRE_CASELESS;       break;
    case 'm':  coptions |= PCRE_MULTILINE;      break;
    case 's':  coptions |= PCRE_DOTALL;         break;
    case 'x':  coptions |= PCRE_EXTENDED;       break;

      /* PCRE specific options */
    case 'A':  coptions |= PCRE_ANCHORED;       break;
    case 'D':  coptions |= PCRE_DOLLAR_ENDONLY; break;
    case 'S':  do_study = true;                 break;
    case 'U':  coptions |= PCRE_UNGREEDY;       break;
    case 'X':  coptions |= PCRE_EXTRA;          break;
    case 'u':  coptions |= PCRE_UTF8;
  /* In  PCRE,  by  default, \d, \D, \s, \S, \w, and \W recognize only ASCII
       characters, even in UTF-8 mode. However, this can be changed by setting
       the PCRE_UCP option. */
#ifdef PCRE_UCP
            coptions |= PCRE_UCP;
#endif
      break;

      /* Custom preg options */
    case 'e':  poptions |= PREG_REPLACE_EVAL;   break;

    case ' ':
    case '\n':
    case '\r':
      break;

    default:
      std::string msg = fmt::format("Unknown modifier '{}': [{}]",
                                       pp[-1], regex->data());
      raise_warning_or_create_error_message(warn, error_message, msg);
      return false;
    }
  }

  /* We've reached a null byte, now check if we're actually at the end of the
     string.  If not this is a bad expression, and a potential security hole. */
  if (regex->size() != (pp - regex->data())) {
    if (warn) {
      raise_error("Error: Null byte found in pattern");
    } else {
      if (error_message) decRefStr(error_message); // decref old StringData
      error_message = makeStaticString("Null byte found in pattern");
      return false;
    }
  }

  /* Store the compiled pattern and extra info in the cache. */
  auto const store_pcre_entry =
    [&](pcre_literal_data& pld, pcre* re=nullptr, pcre_extra* extra=nullptr) {
    assertx((poptions & ~0x1) == 0);
    assertx((coptions & 0x80000000) == 0);
    pcre_cache_entry* new_entry = new pcre_cache_entry();
    new_entry->re = re;
    new_entry->extra = extra;
    new_entry->preg_options = poptions;
    new_entry->compile_options = coptions;

    if (pld.isLiteral()) {
      new_entry->literal_data =
        std::make_unique<pcre_literal_data>(std::move(pld));
      new_entry->num_subpats = 1;
    } else {
       /* Get pcre full info */
      if (!get_pcre_fullinfo(new_entry)) {
        delete new_entry;
        return false;
      }
    }

    pc_counter->increment();
    s_pcreCache.insert(accessor, regex, tkc, new_entry);
    return true;
  };

  // If the pattern is a literal, we can skip compiling it.
  auto literal_data = pcre_literal_data(pattern, coptions);
  if (literal_data.isLiteral()) return store_pcre_entry(literal_data);

  /* Compile pattern and display a warning if compilation failed. */
  const char  *error;
  int erroffset;
  pcre *re = pcre_compile(pattern, coptions, &error, &erroffset, 0);
  if (re == nullptr) {
    std::string msg = fmt::format("Compilation failed: {} at offset {}",
                                     error, erroffset);
    raise_warning_or_create_error_message(warn, error_message, msg);
    return false;
  }

  // Careful: from here 're' needs to be freed if something throws.

  /* If study option was specified, study the pattern and
     store the result in extra for passing to pcre_exec. */
  pcre_extra *extra = nullptr;
  if (!literal_data.isLiteral()) {
    if (do_study || PCRE_STUDY_JIT_COMPILE) {
      int soptions = PCRE_STUDY_JIT_COMPILE;
      extra = pcre_study(re, soptions, &error);
      if (extra) {
        extra->flags |= PCRE_EXTRA_MATCH_LIMIT |
          PCRE_EXTRA_MATCH_LIMIT_RECURSION;
        pcre_assign_jit_stack(extra, alloc_jit_stack, nullptr);
      }
      if (error != nullptr && warn) {
        try {
          raise_warning("Error while studying pattern");
        } catch (...) {
          pcre_free(re);
          throw;
        }
      }
      if ((!Cfg::Jit::NoGdb ||
           Cfg::Jit::UseVtuneAPI ||
           Cfg::Eval::PerfPidMap) &&
          extra &&
          extra->executable_jit != nullptr) {
        size_t size;
        pcre_fullinfo(re, extra, PCRE_INFO_JITSIZE, &size);

        TCA start = *(TCA *)(extra->executable_jit);
        TCA end = start + size;
        std::string name = fmt::format("HHVM::pcre_jit::{}", pattern);

        if (!Cfg::Jit::NoGdb && jit::mcgen::initialized()) {
          Debug::DebugInfo::Get()->recordStub(Debug::TCRange(start, end, false),
                                              name);
        }
        if (Cfg::Jit::UseVtuneAPI) {
          HPHP::jit::reportHelperToVtune(name.c_str(), start, end);
        }
        if (Cfg::Eval::PerfPidMap && jit::mcgen::initialized()) {
          std::string escaped_name;
          folly::json::escapeString(name, escaped_name,
                                    folly::json::serialization_opts());
          Debug::DebugInfo::Get()->recordPerfMap(
            Debug::TCRange(start, end, false),
            SrcKey{}, escaped_name
          );
        }
      }
    }
  }

  return store_pcre_entry(literal_data, re, extra);
}

static bool
pcre_get_compiled_regex_cache(PCRECache::Accessor& accessor,
                              StringData* regex) {
  StringData* error_message = nullptr;
  SCOPE_EXIT {
    assertx(!error_message);
    if (error_message) decRefStr(error_message);
  };
  bool result = pcre_get_compiled_regex_cache(accessor, regex, true, error_message);
  return result;
}

static int* create_offset_array(const pcre_cache_entry* pce,
                                int& size_offsets) {
  /* Allocate memory for the offsets array */
  size_offsets = pce->num_subpats * 3;
  return (int *)req::malloc_noptrs(size_offsets * sizeof(int));
}

static Array str_offset_pair(const OptString& str, int offset) {
  return make_vec_array(str, offset);
}

static inline bool pcre_need_log_error(int pcre_code) {
  return Cfg::PCRE::ErrorLog &&
         (pcre_code == PCRE_ERROR_MATCHLIMIT ||
          pcre_code == PCRE_ERROR_RECURSIONLIMIT);
}

static void pcre_log_error(const char* func, int line, int pcre_code,
                           const char* pattern, int pattern_size,
                           const char* subject, int subject_size,
                           const char* repl, int repl_size,
                           int arg1 = 0, int arg2 = 0,
                           int arg3 = 0, int arg4 = 0) {
  const char* escapedPattern;
  const char* escapedSubject;
  const char* escapedRepl;
  std::string p(pattern, pattern_size);
  std::string s(subject, subject_size);
  std::string r(repl, repl_size);
  escapedPattern = Logger::EscapeString(p);
  escapedSubject = Logger::EscapeString(s);
  escapedRepl = Logger::EscapeString(r);
  const char* errString =
    (pcre_code == PCRE_ERROR_MATCHLIMIT) ? "PCRE_ERROR_MATCHLIMIT" :
    (pcre_code == PCRE_ERROR_RECURSIONLIMIT) ? "PCRE_ERROR_RECURSIONLIMIT" :
    "UNKNOWN";
  raise_warning_unsampled(
    "REGEXERR: %s/%d: err=%d(%s), pattern='%s', subject='%s', repl='%s', "
    "limits=(%" PRId64 ", %" PRId64 "), extra=(%d, %d, %d, %d)",
    func, line, pcre_code, errString,
    escapedPattern, escapedSubject, escapedRepl,
    tl_pcre_globals->preg_backtrack_limit,
    tl_pcre_globals->preg_recursion_limit,
    arg1, arg2, arg3, arg4);
  free((void *)escapedPattern);
  free((void *)escapedSubject);
  free((void *)escapedRepl);
}

namespace {

ALWAYS_INLINE Variant preg_return_internal_error(Variant&& return_value) {
  *rl_last_error_code = PHP_PCRE_INTERNAL_ERROR;
  return std::move(return_value);
}

ALWAYS_INLINE Variant preg_return_bad_regex_error(Variant&& return_value) {
  *rl_last_error_code = PHP_PCRE_BAD_REGEX_ERROR;
  return std::move(return_value);
}

void pcre_handle_exec_error(int pcre_code) {
  int preg_code = 0;
  switch (pcre_code) {
  case PCRE_ERROR_MATCHLIMIT:
    preg_code = PHP_PCRE_BACKTRACK_LIMIT_ERROR;
    break;
  case PCRE_ERROR_RECURSIONLIMIT:
    preg_code = PHP_PCRE_RECURSION_LIMIT_ERROR;
    break;
  case PCRE_ERROR_BADUTF8:
    preg_code = PHP_PCRE_BAD_UTF8_ERROR;
    break;
  case PCRE_ERROR_BADUTF8_OFFSET:
    preg_code = PHP_PCRE_BAD_UTF8_OFFSET_ERROR;
    break;
  case PCRE_ERROR_JIT_STACKLIMIT:
    preg_code = PHP_PCRE_JIT_STACKLIMIT_ERROR;
    break;
  default:
    preg_code = PHP_PCRE_INTERNAL_ERROR;
    break;
  }
  *rl_last_error_code = preg_code;
}

ALWAYS_INLINE Variant
preg_return_pcre_error(int pcre_code, Variant&& return_value) {
  pcre_handle_exec_error(pcre_code);
  return std::move(return_value);
}

ALWAYS_INLINE Variant preg_return_no_error(Variant&& return_value) {
  *rl_last_error_code = PHP_PCRE_NO_ERROR;
  return std::move(return_value);
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

Variant preg_grep(const OptString& pattern, const Array& input, int flags /* = 0 */) {
  PCRECache::Accessor accessor;
  if (!pcre_get_compiled_regex_cache(accessor, pattern.get())) {
    return preg_return_bad_regex_error(false);
  }
  const pcre_cache_entry* pce = accessor.get();

  int size_offsets = 0;
  int* offsets = create_offset_array(pce, size_offsets);
  if (offsets == nullptr) {
    return preg_return_internal_error(false);
  }
  SmartFreeHelper freer(offsets);

  /* Initialize return array */
  auto ret = Array::CreateDict();

  /* Go through the input array */
  bool invert = (flags & PREG_GREP_INVERT);
  pcre_extra extra;
  init_local_extra(&extra, pce->extra);

  for (ArrayIter iter(input); iter; ++iter) {
    OptString entry = iter.second().toString();
    int count = 0;

    if (pce->literal_data) {
      assertx(pce->literal_data->isLiteral());
      count = pce->literal_data->matches(entry.get(), 0, offsets, 0)
        ? 1 : PCRE_ERROR_NOMATCH;
    } else {
      /* Perform the match */
      count = pcre_exec(pce->re, &extra, entry.data(), entry.size(),
                        0, 0, offsets, size_offsets);
    }
    /* Check for too many substrings condition. */
    if (count == 0) {
      raise_warning("Matched, but too many substrings");
      count = pce->num_subpats;
    } else if (count < 0 && count != PCRE_ERROR_NOMATCH) {
      if (pcre_need_log_error(count)) {
        pcre_log_error(__FUNCTION__, __LINE__, count,
                       pattern.data(), pattern.size(),
                       entry.data(), entry.size(),
                       "", 0,
                       flags);
      }
      // NOTE: this returns an error together with a partial result :-(
      return preg_return_pcre_error(count, std::move(ret));
    }

    /* If the entry fits our requirements */
    if ((count > 0 && !invert) ||
        (count == PCRE_ERROR_NOMATCH && invert)) {

      /* Add to return array */
      ret.set(iter.first(), entry);
    }
  }

  return preg_return_no_error(std::move(ret));
}

///////////////////////////////////////////////////////////////////////////////

static Variant preg_match_impl(StringData* pattern,
                               const StringData* subject,
                               Variant* subpats, int flags, int start_offset,
                               bool global) {
  PCRECache::Accessor accessor;
  if (!pcre_get_compiled_regex_cache(accessor, pattern)) {
    return preg_return_bad_regex_error(false);
  }
  pcre_extra extra;
  const pcre_cache_entry* pce = accessor.get();
  init_local_extra(&extra, pce->extra);
  int subpats_order = global ? PREG_PATTERN_ORDER : 0;
  if (subpats) *subpats = Array::CreateDict();

  if (flags) {
    /*
     * subpats_order is pre-set to pattern mode so we change it only if
     * necessary.
     */
    if (flags & 0xff) {
      subpats_order = flags & 0xff;
    }
    if ((global && (subpats_order < PREG_PATTERN_ORDER ||
                    subpats_order > PREG_SET_ORDER)) ||
        (!global && subpats_order != 0)) {
      raise_warning("Invalid flags specified");
      return preg_return_internal_error(init_null());
    }
  }

  /* Negative offset counts from the end of the string. */
  if (start_offset < 0) {
    start_offset = subject->size() + start_offset;
    if (start_offset < 0) {
      start_offset = 0;
    }
  }

  int size_offsets = 0;
  int* offsets = create_offset_array(pce, size_offsets);
  SmartFreeHelper offsetsFreer(offsets);
  int num_subpats = pce->num_subpats;
  if (offsets == nullptr) return preg_return_internal_error(false);

  /* Allocate match sets array and initialize the values. */

  /* An array of sets of matches for each subpattern after a global match */
  auto match_sets = Array::CreateDict();
  if (global && subpats_order == PREG_PATTERN_ORDER) {
    for (int i = 0; i < num_subpats; i++) {
      match_sets.set(i, Array::CreateDict());
    }
  }

  /*
   * If PREG_OFFSET_CAPTURE, each match, instead of being a string, will
   * be an array where the first element is a substring containing the
   * match and the second element is the position of the first character of
   * the substring in the input.
   */
  bool offset_capture = flags & PREG_OFFSET_CAPTURE;
  const char** stringlist; // Holds list of subpatterns
  auto const get_value = [&](int i) {
    auto const length = offsets[(i<<1)+1] - offsets[i<<1];
    auto const match = OptString(stringlist[i], length, CopyString);
    return offset_capture
      ? Variant(str_offset_pair(match, offsets[i<<1]))
      : Variant(match);
  };
  auto const get_value_empty = [&](int i) {
    const OptString match = String::Empty();
    return offset_capture
      ? Variant(str_offset_pair(match, offsets[i<<1]))
      : Variant(match);
  };

  /*
   * Skip building name table when using literal_data. Name table is used
   * to add named subpatterns to result array. Literal data has none of these,
   * so we can skip this step.
   */
  const char* const* subpat_names = nullptr;
  auto const is_literal = pce->literal_data != nullptr;
  if (!is_literal) {
    subpat_names = get_subpat_names(pce);
    if (subpat_names == nullptr) return preg_return_internal_error(false);
  }
  auto const set_subpats = [&](auto& arr, int i, const Variant& value) {
    if (is_literal) return;
    if (subpat_names[i]) arr.set(OptString(subpat_names[i]), value);
  };

  int i;
  const bool includeNonMatchingCaptures = flags & PREG_FB__PRIVATE__HSL_IMPL;

  // Add matches to result array for this run
  auto add_match_set = [&](auto& arr, int count) {
    for (i = 0; i < count; i++) {
      auto const value = get_value(i);
      set_subpats(arr, i, value);
      arr.set(i, value);
    }
    if (includeNonMatchingCaptures) {
      for (; i < num_subpats; i++) {
        auto const value = get_value_empty(i);
        set_subpats(arr, i, value);
        arr.set(i, value);
      }
    }
  };

  int matched = 0;
  int g_notempty = 0; // If the match should not be empty
  int exec_options = 0;

  do {
    int count = 0;
    int options = exec_options | g_notempty;
    if (is_literal) {
      assertx(literalOptions(options));
      count = pce->literal_data->matches(subject, start_offset, offsets, options)
        ? 1 : PCRE_ERROR_NOMATCH;
    } else {
      /* Execute the regular expression. */
      count = pcre_exec(pce->re, &extra, subject->data(), subject->size(),
                        start_offset, options,
                        offsets, size_offsets);

      /* The string was already proved to be valid UTF-8 */
      exec_options |= PCRE_NO_UTF8_CHECK;
    }
    /* Check for too many substrings condition. */
    if (count == 0) {
      raise_warning("Matched, but too many substrings");
      count = num_subpats;
    }

    /* If something has matched */
    if (count > 0) {
      matched++;

      if (subpats) {
        // Try to get the list of substrings and display a warning if failed.
        if (offsets[1] < offsets[0] ||
            pcre_get_substring_list(subject->data(), offsets, count,
                                    &stringlist) < 0) {
          raise_warning("Get subpatterns list failed");
          return preg_return_internal_error(false);
        }

        if (global) {
          if (subpats_order == PREG_PATTERN_ORDER) {
            /* For each subpattern, insert it into the appropriate array. */
            for (i = 0; i < count; i++) {
              auto const value = get_value(i);
              auto& arr = asArrRef(match_sets.lval(i));
              assertx(arr->isVectorData());
              arr.set(safe_cast<int64_t>(arr.size()), value);
            }
            /*
             * If the number of captured subpatterns on this run is
             * less than the total possible number, pad the result
             * arrays with empty strings.
             */
            for (; i < num_subpats; i++) {
              auto& arr = asArrRef(match_sets.lval(i));
              assertx(arr->isVectorData());
              arr.set(safe_cast<int64_t>(arr.size()), Variant{String::Empty()});
            }
          } else {
            auto result_set = Array::CreateDict();
            add_match_set(result_set, count);
            auto& arr = subpats->asArrRef();
            assertx(arr->isVectorData());
            arr.set(safe_cast<int64_t>(arr.size()), std::move(result_set));
          }
        } else {
          auto& arr = subpats->asArrRef();
          add_match_set(arr, count);
        }
        pcre_free((void *) stringlist);
      }
    } else if (count == PCRE_ERROR_NOMATCH) {
      /* If we previously set PCRE_NOTEMPTY after a null match,
         this is not necessarily the end. We need to advance
         the start offset, and continue. Fudge the offset values
         to achieve this, unless we're already at the end of the string. */
      if (g_notempty && start_offset < subject->size()) {
        offsets[0] = start_offset;
        offsets[1] = start_offset + 1;
      } else
        break;
    } else {
      if (pcre_need_log_error(count)) {
        pcre_log_error(__FUNCTION__, __LINE__, count,
                       pattern->data(), pattern->size(),
                       subject->data(), subject->size(),
                       "", 0,
                       flags, start_offset, g_notempty, global);
      }
      return preg_return_pcre_error(count, false);
    }

    /* If we have matched an empty string, mimic what Perl's /g options does.
       This turns out to be rather cunning. First we set PCRE_NOTEMPTY and try
       the match again at the same point. If this fails (picked up above) we
       advance to the next character. */
    g_notempty = (offsets[1] == offsets[0])? PCRE_NOTEMPTY | PCRE_ANCHORED : 0;

    /* Advance to the position right after the last full match */
    start_offset = offsets[1];
  } while (global);

  /* Add the match sets to the output array and clean up */
  if (subpats && global && subpats_order == PREG_PATTERN_ORDER) {
    auto& arr = subpats->asArrRef();
    for (i = 0; i < num_subpats; i++) {
      auto const value = match_sets[i];
      set_subpats(arr, i, value);
      arr.set(i, match_sets[i]);
    }
  }
  return preg_return_no_error(std::move(matched));
}

Variant preg_match(const OptString& pattern, const OptString& subject,
                   Variant* matches /* = nullptr */, int flags /* = 0 */,
                   int offset /* = 0 */) {
  return preg_match(pattern.get(), subject.get(), matches, flags, offset);
}

Variant preg_match(StringData* pattern, const StringData* subject,
                   Variant* matches /* = nullptr */, int flags /* = 0 */,
                   int offset /* = 0 */) {
  return preg_match_impl(pattern, subject, matches, flags, offset, false);
}

Variant preg_match_all(const OptString& pattern, const OptString& subject,
                       Variant* matches /* = nullptr */,
                       int flags /* = 0 */, int offset /* = 0 */) {
  return preg_match_all(pattern.get(), subject.get(), matches, flags, offset);
}

Variant preg_match_all(StringData* pattern, const StringData* subject,
                       Variant* matches /* = nullptr */,
                       int flags /* = 0 */, int offset /* = 0 */) {
  return preg_match_impl(pattern, subject, matches, flags, offset, true);
}

OptString preg_get_error_message_if_invalid(const OptString& pattern) {
  PCRECache::Accessor accessor;
  StringData* error_message = nullptr;
  bool is_valid;
  try {
    is_valid = pcre_get_compiled_regex_cache(accessor, pattern.get(), false, error_message);
  } catch (...) {
    if (error_message) decRefStr(error_message);
    throw;
  }
  if (is_valid) {
    return null_string;
  }
  if (!error_message) {
    error_message = makeStaticString("Failed to process regex for unknown reason");
  }
  return OptString::attach(error_message);
}

///////////////////////////////////////////////////////////////////////////////

static OptString preg_do_repl_func(const Variant& function, const OptString& subject,
                                   int* offsets, const char* const* subpat_names,
                                   int count) {
  Array subpats = Array::CreateDict();
  for (int i = 0; i < count; i++) {
    auto off1 = offsets[i<<1];
    auto off2 = offsets[(i<<1)+1];
    auto sub = subject.substr(off1, off2 - off1);

    if (subpat_names && subpat_names[i]) {
      subpats.set(OptString(subpat_names[i]), sub);
    }
    subpats.set(i, sub);
  }

  return vm_call_user_func(function, make_vec_array(subpats)).toString();
}

static bool preg_get_backref(const char** str, int* backref) {
  char in_brace = 0;
  const char* walk = *str;

  if (walk[1] == 0) {
    return false;
  }

  if (*walk == '$' && walk[1] == '{') {
    in_brace = 1;
    walk++;
  }
  walk++;

  if (*walk >= '0' && *walk <= '9') {
    *backref = *walk - '0';
    walk++;
  } else {
    return false;
  }

  if (*walk && *walk >= '0' && *walk <= '9') {
    *backref = *backref * 10 + *walk - '0';
    walk++;
  }

  if (in_brace) {
    if (*walk == 0 || *walk != '}') {
      return false;
    }
    walk++;
  }

  *str = walk;
  return true;
}

static Variant php_pcre_replace(const OptString& pattern, const OptString& subject,
                                const Variant& replace_var, bool callable,
                                int limit, int* replace_count) {
  PCRECache::Accessor accessor;
  if (!pcre_get_compiled_regex_cache(accessor, pattern.get())) {
    return preg_return_bad_regex_error(init_null());
  }
  const pcre_cache_entry* pce = accessor.get();
  if (pce->preg_options & PREG_REPLACE_EVAL) {
    raise_error("preg_replace(): Support for the /e modifier has been removed, use "
                "preg_replace_callback instead");
  }

  int size_offsets;
  int* offsets = create_offset_array(pce, size_offsets);
  SmartFreeHelper offsetsFreer(offsets);
  if (offsets == nullptr) {
    return preg_return_internal_error(init_null());
  }
  auto const is_literal = pce->literal_data != nullptr;
  const char* const* subpat_names = nullptr;
  if (!is_literal) {
    subpat_names = get_subpat_names(pce);
    if (subpat_names == nullptr) return preg_return_internal_error(init_null());
  }

  const char* replace = nullptr;
  const char* replace_end = nullptr;
  int replace_len = 0;
  OptString replace_val;

  if (!callable) {
    replace_val = replace_var.toString();
    replace = replace_val.data();
    replace_len = replace_val.size();
    replace_end = replace + replace_len;
  }

  StringBuffer result(2 * subject.size());

  try {

    /* Initialize */
    const char* match = nullptr;
    int start_offset = 0;
    pcre_extra extra;
    init_local_extra(&extra, pce->extra);

    const char* walk;     // Used to walk the replacement string
    char walk_last;       // Last walked character
    int match_len;        // Length of the current match
    int backref;          // Backreference number
    int g_notempty = 0;   // If the match should not be empty
    int exec_options = 0; // Options passed to pcre_exec
    while (1) {
      int count = 0;
      int options = exec_options | g_notempty;
      if (pce->literal_data && literalOptions(options)) {
        assertx(pce->literal_data->isLiteral());
        count =
          pce->literal_data->matches(subject.get(), start_offset, offsets, options)
          ? 1 : PCRE_ERROR_NOMATCH;
      } else {
        /* Execute the regular expression. */
        count = pcre_exec(pce->re, &extra, subject.data(), subject.size(),
                          start_offset, options, offsets, size_offsets);

        /* The string was already proved to be valid UTF-8 */
        exec_options |= PCRE_NO_UTF8_CHECK;
      }

      /* Check for too many substrings condition. */
      if (count == 0) {
        raise_warning("Matched, but too many substrings");
        count = pce->num_subpats;
      }

      const char* piece = subject.data() + start_offset;
      if (count > 0 && offsets[1] >= offsets[0] &&
          (limit == -1 || limit > 0)) {
        if (replace_count) {
          ++*replace_count;
        }
        /* Set the match location in subject */
        match = subject.data() + offsets[0];

        OptString callable_result;
        if (callable) {
          /* Use custom function to get replacement string and its length. */
          callable_result = preg_do_repl_func(replace_var, subject, offsets,
                                              subpat_names, count);
        } else { /* do regular substitution */
          walk = replace;
          walk_last = 0;
          while (walk < replace_end) {
            if ('\\' == *walk || '$' == *walk) {
              if (walk_last == '\\') {
                walk++;
                walk_last = 0;
                continue;
              }
              if (preg_get_backref(&walk, &backref)) {
                if (backref < count) {
                  match_len = offsets[(backref<<1)+1] - offsets[backref<<1];
                }
                continue;
              }
            }
            walk++;
            walk_last = walk[-1];
          }
        }

        /* copy the part of the string before the match */
        result.append(piece, match-piece);

        if (callable) {
          /* Copy result from custom function to buffer and clean up. */
          result.append(callable_result.data(), callable_result.size());
        } else { /* do regular backreference copying */
          walk = replace;
          walk_last = 0;
          Array params;
          while (walk < replace_end) {
            if ('\\' == *walk || '$' == *walk) {
              if (walk_last == '\\') {
                result.set(result.size() - 1, *walk++);
                walk_last = 0;
                continue;
              }
              if (preg_get_backref(&walk, &backref)) {
                if (backref < count) {
                  match_len = offsets[(backref<<1)+1] - offsets[backref<<1];
                  result.append(
                    subject.data() + offsets[backref<<1],
                    match_len
                  );
                }
                continue;
              }
            }
            result.append(*walk++);
            walk_last = walk[-1];
          }
        }

        if (limit != -1) {
          limit--;
        }

      } else if (count == PCRE_ERROR_NOMATCH || limit == 0) {
        /* If we previously set PCRE_NOTEMPTY after a null match,
           this is not necessarily the end. We need to advance
           the start offset, and continue. Fudge the offset values
           to achieve this, unless we're already at the end of the string. */
        if (g_notempty != 0 && start_offset < subject.size()) {
          offsets[0] = start_offset;
          offsets[1] = start_offset + 1;
          result.append(piece, 1);
        } else {
          /* stick that last bit of string on our output */
          result.append(piece, subject.size() - start_offset);
          break;
        }
      } else {
        if (pcre_need_log_error(count)) {
          const char* s;
          int size;
          OptString stemp;
          if (callable) {
            if (replace_var.isObject()) {
              stemp = replace_var.asCObjRef()->getClassName().asString()
                    + "::__invoke";
            } else {
              stemp = replace_var.toString();
            }
            s = stemp.data();
            size = stemp.size();
          } else {
            s = replace_val.data();
            size = replace_val.size();
          }
          pcre_log_error(__FUNCTION__, __LINE__, count,
                         pattern.data(), pattern.size(),
                         subject.data(), subject.size(),
                         s, size,
                         callable, limit, start_offset, g_notempty);
        }
        return preg_return_pcre_error(count, init_null());
      }

      /* If we have matched an empty string, mimic what Perl's /g options does.
         This turns out to be rather cunning. First we set PCRE_NOTEMPTY and try
         the match again at the same point. If this fails (picked up above) we
         advance to the next character. */
      g_notempty = (offsets[1] == offsets[0])? PCRE_NOTEMPTY | PCRE_ANCHORED : 0;

      /* Advance to the next piece. */
      start_offset = offsets[1];
    }

    return preg_return_no_error(result.detach());
  } catch (...) {
    throw;
  }
}

static Variant php_replace_in_subject(const Variant& regex, const Variant& replace,
                                      OptString subject, int limit, bool callable,
                                      int* replace_count) {
  if (!regex.isArray()) {
    return php_pcre_replace(regex.toString(), subject, replace, callable,
                            limit, replace_count);
  }

  if (callable || !replace.isArray()) {
    Array arr = regex.toDict();
    for (ArrayIter iterRegex(arr); iterRegex; ++iterRegex) {
      OptString regex_entry = iterRegex.second().toString();
      auto ret = php_pcre_replace(regex_entry, subject, replace, callable,
                                  limit, replace_count);
      if (!ret.isString()) {
        assertx(ret.isNull());
        return ret; // php_pcre_replace already set error
      }
      subject = ret.asStrRef();
      assertx(!subject.isNull());
    }
    return preg_return_no_error(std::move(subject));
  }

  Array arrReplace = replace.toDict();
  Array arrRegex = regex.toDict();
  ArrayIter iterReplace(arrReplace);
  for (ArrayIter iterRegex(arrRegex); iterRegex; ++iterRegex) {
    OptString regex_entry = iterRegex.second().toString();
    Variant replace_value;
    if (iterReplace) {
      replace_value = iterReplace.second();
      ++iterReplace;
    }

    auto ret = php_pcre_replace(regex_entry, subject, replace_value, callable,
                                limit, replace_count);
    if (!ret.isString()) {
      assertx(ret.isNull());
      return ret; // php_pcre_replace already set error
    }
    subject = ret.asStrRef();
    assertx(!subject.isNull());
  }
  return preg_return_no_error(std::move(subject));
}

Variant preg_replace_impl(const Variant& pattern, const Variant& replacement,
                          const Variant& subject, int limit, int64_t* count,
                          bool is_callable, bool is_filter) {
  assertx(!(is_callable && is_filter));
  if (!is_callable &&
      replacement.isArray() && !pattern.isArray()) {
    raise_warning("Parameter mismatch, pattern is a string while "
                    "replacement is an array");
    return preg_return_internal_error(false);
  }

  int replace_count = 0;
  if (!isContainer(subject)) {
    auto ret = php_replace_in_subject(pattern, replacement, subject.toString(),
                                      limit, is_callable, &replace_count);

    if (ret.isNull()) return ret; // php_replace_in_subject already set error
    assertx(ret.isString());
    if (count) *count = replace_count;
    if (is_filter && replace_count == 0) {
      return preg_return_internal_error(init_null());
    }
    return preg_return_no_error(std::move(ret));
  }

  Array return_value = Array::CreateDict();
  Array arrSubject = subject.toDict();
  for (ArrayIter iter(arrSubject); iter; ++iter) {
    auto old_replace_count = replace_count;
    OptString subject_entry = iter.second().toString();
    auto ret = php_replace_in_subject(pattern, replacement, subject_entry,
                                      limit, is_callable, &replace_count);

    if (ret.isString() && (!is_filter || replace_count > old_replace_count)) {
      return_value.set(iter.first(), ret.asStrRef());
    }
  }
  if (count) *count = replace_count;
  return preg_return_no_error(std::move(return_value));
}

int preg_replace(Variant& result,
                 const Variant& pattern,
                 const Variant& replacement,
                 const Variant& subject,
                 int limit /* = -1 */) {
  int64_t count;
  result = preg_replace_impl(pattern, replacement, subject,
                             limit, &count, false, false);
  return count;
}

int preg_replace_callback(Variant& result,
                          const Variant& pattern,
                          const Variant& callback,
                          const Variant& subject,
                          int limit /* = -1 */) {
  int64_t count;
  result = preg_replace_impl(pattern, callback, subject,
                             limit, &count, true, false);
  return count;
}

///////////////////////////////////////////////////////////////////////////////

namespace {

const StaticString s_OneUnicodeCharPattern("/./us");

} // namespace

Variant preg_split(const OptString& pattern, const OptString& subject,
                   int limit /* = -1 */, int flags /* = 0 */) {
  PCRECache::Accessor accessor;
  if (!pcre_get_compiled_regex_cache(accessor, pattern.get())) {
    return preg_return_bad_regex_error(false);
  }
  const pcre_cache_entry* pce = accessor.get();

  int no_empty = flags & PREG_SPLIT_NO_EMPTY;
  bool delim_capture = flags & PREG_SPLIT_DELIM_CAPTURE;
  bool offset_capture = flags & PREG_SPLIT_OFFSET_CAPTURE;

  if (limit == 0) {
    limit = -1;
  }

  int size_offsets = 0;
  int* offsets = create_offset_array(pce, size_offsets);
  SmartFreeHelper offsetsFreer(offsets);
  if (offsets == nullptr) {
    return preg_return_internal_error(false);
  }

  /* Start at the beginning of the string */
  int start_offset = 0;
  int next_offset = 0;
  const char* last_match = subject.data();
  pcre_extra extra;
  init_local_extra(&extra, pce->extra);

  // Get next piece if no limit or limit not yet reached and something matched
  Array result = Array::CreateDict();
  int g_notempty = 0;   /* If the match should not be empty */
  int utf8_check = 0;
  PCRECache::Accessor bump_accessor;
  const pcre_cache_entry* bump_pce = nullptr; /* instance for empty matches */
  while ((limit == -1 || limit > 1)) {
    int count = 0;
    int options = g_notempty | utf8_check;
    if (pce->literal_data && literalOptions(options)) {
      assertx(pce->literal_data->isLiteral());
      count =
        pce->literal_data->matches(subject.get(), start_offset, offsets, options)
        ? 1 : PCRE_ERROR_NOMATCH;
    } else {
      count = pcre_exec(pce->re, &extra, subject.data(), subject.size(),
                        start_offset, options, offsets, size_offsets);
      /* Subsequent calls to pcre_exec don't need to bother with the
      * utf8 validity check: if the subject isn't valid, the first
      * call to pcre_exec will have failed, and as long as we only
      * set start_offset to known character boundaries we won't
      * supply an invalid offset. */
      utf8_check = PCRE_NO_UTF8_CHECK;
    }

    /* Check for too many substrings condition. */
    if (count == 0) {
      raise_warning("Matched, but too many substrings");
      count = pce->num_subpats;
    }

    /* If something matched */
    if (count > 0 && offsets[1] >= offsets[0]) {
      if (!no_empty || subject.data() + offsets[0] != last_match) {
        auto const length = subject.data() + offsets[0] - last_match;
        auto const match = OptString(last_match, length, CopyString);
        auto const value = offset_capture
          ? Variant(str_offset_pair(match, next_offset))
          : Variant(match);
        assertx(result->isVectorData());
        result.set(safe_cast<int64_t>(result.size()), value);

        /* One less left to do */
        if (limit != -1) limit--;
      }

      last_match = subject.data() + offsets[1];
      next_offset = offsets[1];

      if (delim_capture) {
        int i, match_len;
        for (i = 1; i < count; i++) {
          match_len = offsets[(i<<1)+1] - offsets[i<<1];
          /* If we have matched a delimiter */
          if (!no_empty || match_len > 0) {
            auto const match = subject.substr(offsets[i<<1], match_len);
            auto const value = offset_capture
              ? Variant(str_offset_pair(match, offsets[i<<1]))
              : Variant(match);
            assertx(result->isVectorData());
            result.set(safe_cast<int64_t>(result.size()), value);
          }
        }
      }
    } else if (count == PCRE_ERROR_NOMATCH) {
      /* If we previously set PCRE_NOTEMPTY after a null match,
         this is not necessarily the end. We need to advance
         the start offset, and continue. Fudge the offset values
         to achieve this, unless we're already at the end of the string. */
      if (g_notempty != 0 && start_offset < subject.size()) {
        if (pce->compile_options & PCRE_UTF8) {
          if (bump_pce == nullptr) {
            auto const DEBUG_ONLY ok = pcre_get_compiled_regex_cache(
              bump_accessor, s_OneUnicodeCharPattern.get());
            assertx(ok);
            bump_pce = bump_accessor.get();
          }
          pcre_extra bump_extra;
          init_local_extra(&bump_extra, bump_pce->extra);
          count = pcre_exec(bump_pce->re, &bump_extra, subject.data(),
                            subject.size(), start_offset,
                            utf8_check, offsets, size_offsets);
          if (count < 1) {
            raise_warning("Unknown error");
            offsets[0] = start_offset;
            offsets[1] = start_offset + 1;
            if (pcre_need_log_error(count)) {
              pcre_log_error(__FUNCTION__, __LINE__, count,
                             pattern.data(), pattern.size(),
                             subject.data(), subject.size(),
                             "", 0,
                             limit, flags, start_offset);
            }
          }
        } else {
          offsets[0] = start_offset;
          offsets[1] = start_offset + 1;
        }
      } else
        break;
    } else {
      if (pcre_need_log_error(count)) {
        pcre_log_error(__FUNCTION__, __LINE__, count,
                       pattern.data(), pattern.size(),
                       subject.data(), subject.size(),
                       "", 0,
                       limit, flags, start_offset, g_notempty);
      }
      // NOTE: this returns an error together with a partial result :-(
      start_offset = last_match - subject.data(); /* offset might have
                                                   * been incremented,
                                                   * but without further
                                                   * successful matches */
      if (!no_empty || start_offset < subject.size()) {
        auto const match = subject.substr(start_offset);
        auto const value = offset_capture
          ? Variant(str_offset_pair(match, start_offset))
          : Variant(match);
        assertx(result->isVectorData());
        result.set(safe_cast<int64_t>(result.size()), value);
      }
      return preg_return_pcre_error(count, std::move(result));
    }

    /* If we have matched an empty string, mimic what Perl's /g options does.
       This turns out to be rather cunning. First we set PCRE_NOTEMPTY and try
       the match again at the same point. If this fails (picked up above) we
       advance to the next character. */
    g_notempty = (offsets[1] == offsets[0])? PCRE_NOTEMPTY | PCRE_ANCHORED : 0;

    /* Advance to the position right after the last full match */
    start_offset = offsets[1];
  }

  start_offset = last_match - subject.data(); /* offset might have
                                                * been incremented,
                                                * but without further
                                                * successful matches */
  if (!no_empty || start_offset < subject.size()) {
    auto const match = subject.substr(start_offset);
    auto const value = offset_capture
      ? Variant(str_offset_pair(match, start_offset))
      : Variant(match);
    assertx(result->isVectorData());
    result.set(safe_cast<int64_t>(result.size()), value);
  }

  return preg_return_no_error(std::move(result));
}

///////////////////////////////////////////////////////////////////////////////

OptString preg_quote(const OptString& str,
                     const OptString& delimiter /* = null_string */) {
  const char* in_str = str.data();
  const char* in_str_end = in_str + str.size();

  /* Nothing to do if we got an empty string */
  if (in_str == in_str_end) {
    return str;
  }

  char delim_char = 0;      /* Delimiter character to be quoted */
  bool quote_delim = false; /* Whether to quote additional delim char */
  if (!delimiter.empty()) {
    delim_char = delimiter.charAt(0);
    quote_delim = true;
  }

  /* Allocate enough memory so that even if each character
     is quoted, we won't run out of room */
  static_assert(
    (StringData::MaxSize * 4 + 1) < std::numeric_limits<int64_t>::max()
  );
  OptString ret(4 * str.size() + 1, ReserveString);
  char* out_str = ret.mutableData();

  /* Go through the string and quote necessary characters */
  const char* p;
  char* q;
  for (p = in_str, q = out_str; p != in_str_end; p++) {
    char c = *p;
    switch (c) {
    case '.': case '\\': case '+': case '*': case '?':
    case '[': case '^':  case ']': case '$': case '(':
    case ')': case '{':  case '}': case '=': case '!':
    case '>': case '<':  case '|': case ':': case '-':
    case '#':
      *q++ = '\\';
      *q++ = c;
      break;

    case '\0':
      *q++ = '\\';
      *q++ = '0';
      *q++ = '0';
      *q++ = '0';
      break;

    default:
      if (quote_delim && c == delim_char)
        *q++ = '\\';
      *q++ = c;
      break;
    }
  }
  *q = '\0';

  return ret.setSize(q - out_str);
}

///////////////////////////////////////////////////////////////////////////////
// last_error

int preg_last_error() {
  return *rl_last_error_code;
}

PregWithErrorGuard::~PregWithErrorGuard() {
  if (*rl_last_error_code == PHP_PCRE_NO_ERROR) {
    error.setNull();
  } else {
    error = *rl_last_error_code;
  }
  *rl_last_error_code = prior_error;
}

size_t preg_pcre_cache_size() {
  return s_pcreCache.size();
}

///////////////////////////////////////////////////////////////////////////////
// regexec

static void php_reg_eprint(int err, regex_t* re) {
  char *buf = nullptr, *message = nullptr;
  size_t len;
  size_t buf_len;

#ifdef REG_ITOA
  /* get the length of the message */
  buf_len = regerror(REG_ITOA | err, re, nullptr, 0);
  if (buf_len) {
    buf = (char *)req::malloc_noptrs(buf_len);
    if (!buf) return; /* fail silently */
    /* finally, get the error message */
    regerror(REG_ITOA | err, re, buf, buf_len);
  }
#else
  buf_len = 0;
#endif
  len = regerror(err, re, nullptr, 0);
  if (len) {
    message = (char *)req::malloc_noptrs(buf_len + len + 2);
    if (!message) {
      return; /* fail silently */
    }
    if (buf_len) {
      snprintf(message, buf_len, "%s: ", buf);
      buf_len += 1; /* so pointer math below works */
    }
    /* drop the message into place */
    regerror(err, re, message + buf_len, len);
    raise_warning("%s", message);
  }
  req::free(buf);
  req::free(message);
}

Variant php_split(const OptString& spliton, const OptString& str, int count,
                  bool icase) {
  const char* strp = str.data();
  const char* endp = strp + str.size();

  regex_t re;
  int copts = icase ? REG_ICASE : 0;
  int err = regcomp(&re, spliton.data(), REG_EXTENDED | copts);
  if (err) {
    php_reg_eprint(err, &re);
    return false;
  }

  Array return_value = Array::CreateVec();
  regmatch_t subs[1];

  /* churn through str, generating array entries as we go */
  while ((count == -1 || count > 1) &&
         !(err = regexec(&re, strp, 1, subs, 0))) {
    if (subs[0].rm_so == 0 && subs[0].rm_eo) {
      /* match is at start of string, return empty string */
      return_value.append("");
      /* skip ahead the length of the regex match */
      strp += subs[0].rm_eo;
    } else if (subs[0].rm_so == 0 && subs[0].rm_eo == 0) {
      /* No more matches */
      regfree(&re);
      raise_warning("Invalid Regular Expression to split()");
      return false;
    } else {
      /* On a real match */

      /* make a copy of the substring */
      int size = subs[0].rm_so;

      /* add it to the array */
      return_value.append(OptString(strp, size, CopyString));

      /* point at our new starting point */
      strp = strp + subs[0].rm_eo;
    }

    /* if we're only looking for a certain number of points,
       stop looking once we hit it */
    if (count != -1) {
      count--;
    }
  }

  /* see if we encountered an error */
  if (err && err != REG_NOMATCH) {
    php_reg_eprint(err, &re);
    regfree(&re);
    return false;
  }

  /* otherwise we just have one last element to add to the array */
  int size = endp - strp;
  return_value.append(OptString(strp, size, CopyString));

  regfree(&re);
  return return_value;
}

///////////////////////////////////////////////////////////////////////////////
}
