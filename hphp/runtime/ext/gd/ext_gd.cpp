/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
#include "hphp/runtime/ext/gd/ext_gd.h"

#include <sys/stat.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/base/request-event-handler.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/zend-printf.h"
#include "hphp/runtime/base/zend-string.h"
#include "hphp/runtime/ext/std/ext_std_file.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/util/alloc.h"
#include "hphp/util/rds-local.h"

#include "hphp/runtime/ext/gd/libgd/gd.h"
#include "hphp/runtime/ext/gd/libgd/gdfontt.h"  /* 1 Tiny font */
#include "hphp/runtime/ext/gd/libgd/gdfonts.h"  /* 2 Small font */
#include "hphp/runtime/ext/gd/libgd/gdfontmb.h" /* 3 Medium bold font */
#include "hphp/runtime/ext/gd/libgd/gdfontl.h"  /* 4 Large font */
#include "hphp/runtime/ext/gd/libgd/gdfontg.h"  /* 5 Giant font */
#include <zlib.h>

#include <folly/ScopeGuard.h>
#include <folly/portability/Stdlib.h>
#include <folly/portability/Unistd.h>
#include <libheif/heif.h>

/* Section Filters Declarations */
/* IMPORTANT NOTE FOR NEW FILTER
 * Do not forget to update:
 * IMAGE_FILTER_MAX: define the last filter index
 * IMAGE_FILTER_MAX_ARGS: define the biggest amount of arguments
 * image_filter array in PHP_FUNCTION(imagefilter)
 */
#define IMAGE_FILTER_NEGATE         0
#define IMAGE_FILTER_GRAYSCALE      1
#define IMAGE_FILTER_BRIGHTNESS     2
#define IMAGE_FILTER_CONTRAST       3
#define IMAGE_FILTER_COLORIZE       4
#define IMAGE_FILTER_EDGEDETECT     5
#define IMAGE_FILTER_EMBOSS         6
#define IMAGE_FILTER_GAUSSIAN_BLUR  7
#define IMAGE_FILTER_SELECTIVE_BLUR 8
#define IMAGE_FILTER_MEAN_REMOVAL   9
#define IMAGE_FILTER_SMOOTH         10
#define IMAGE_FILTER_PIXELATE       11
#define IMAGE_FILTER_MAX            11
#define IMAGE_FILTER_MAX_ARGS       6

#define IMAGE_TYPE_GIF 1
#define IMAGE_TYPE_JPEG 2
#define IMAGE_TYPE_PNG 4
#define IMAGE_TYPE_WBMP 8

//#define IM_MEMORY_CHECK

namespace HPHP {

// libheif >= 1.13 renamed heif_reader_grow_status_error
#if LIBHEIF_NUMERIC_VERSION >= ((1<<24) | (13<<16))
static constexpr auto heif_reader_grow_status_error =
    heif_reader_grow_status_size_beyond_eof;
#endif

///////////////////////////////////////////////////////////////////////////////

#define HAS_GDIMAGESETANTIALIASED

#if defined(HAS_GDIMAGEANTIALIAS)

#define SetAntiAliased(gd, flag) gdImageAntialias(gd, flag)
#define SetupAntiAliasedColor(gd, color) (color)

#elif defined(HAS_GDIMAGESETANTIALIASED)

#define SetAntiAliased(gd, flag) ((gd)->AA = (flag))
#define SetupAntiAliasedColor(gd, color)                \
  ((gd)->AA ?                                           \
   gdImageSetAntiAliased(im, color), gdAntiAliased :    \
   color)

#else

#define SetAntiAliased(gd, flag)
#define SetupAntiAliasedColor(gd, color) (color)

#endif

///////////////////////////////////////////////////////////////////////////////

void Image::reset() {
  if (m_gdImage) {
    gdImageDestroy(m_gdImage);
    m_gdImage = nullptr;
  }
}


struct ImageMemoryAlloc final : RequestEventHandler {
  ImageMemoryAlloc() : m_mallocSize(0) {}

  void requestInit() override {
#ifdef IM_MEMORY_CHECK
    void *ptrs[1000];
    int n = 1000;
    if (m_mallocSize) imDump(ptrs, n);
#endif
    assertx(m_mallocSize == 0);
    m_mallocSize = 0;
  }
  void requestShutdown() override {
#ifdef IM_MEMORY_CHECK
    void *ptrs[1000];
    int n = 1000;
    if (m_mallocSize) imDump(ptrs, n);
    assertx(m_mallocSize == 0);
#endif
    m_mallocSize = 0;
  }
  void *imMalloc(size_t size
#ifdef IM_MEMORY_CHECK
, int ln
#endif
  ) {
    assertx(m_mallocSize <= (size_t)Cfg::Server::ImageMemoryMaxBytes);
    if (size > (size_t)Cfg::Server::ImageMemoryMaxBytes ||
        m_mallocSize + size > (size_t)Cfg::Server::ImageMemoryMaxBytes) {
      return nullptr;
    }

#ifdef IM_MEMORY_CHECK
    void *ptr = local_malloc(sizeof(ln) + sizeof(size) + size);
    if (!ptr) return nullptr;
    memcpy(ptr, &ln, sizeof(ln));
    memcpy((char*)ptr + sizeof(ln), &size, sizeof(size));
    m_mallocSize += size;
    m_alloced.insert(ptr);
    return ((char *)ptr + sizeof(ln) + sizeof(size));
#else
    void *ptr = local_malloc(sizeof(size) + size);
    if (!ptr) return nullptr;
    memcpy(ptr, &size, sizeof(size));
    m_mallocSize += size;
    return ((char *)ptr + sizeof(size));
#endif
  }
  void *imCalloc(size_t nmemb, size_t size
#ifdef IM_MEMORY_CHECK
, int ln
#endif
  ) {
    assertx(m_mallocSize <= (size_t)Cfg::Server::ImageMemoryMaxBytes);
    size_t bytes = nmemb * size;
    if (bytes > (size_t)Cfg::Server::ImageMemoryMaxBytes ||
        m_mallocSize + bytes > (size_t)Cfg::Server::ImageMemoryMaxBytes) {
      return nullptr;
    }

#ifdef IM_MEMORY_CHECK
    void *ptr = local_malloc(sizeof(ln) + sizeof(size) + bytes);
    if (!ptr) return nullptr;
    memset(ptr, 0, sizeof(ln) + sizeof(size) + bytes);
    memcpy(ptr, &ln, sizeof(ln));
    memcpy((char*)ptr + sizeof(ln), &bytes, sizeof(bytes));
    m_mallocSize += bytes;
    m_alloced.insert(ptr);
    return ((char *)ptr + sizeof(ln) + sizeof(size));
#else
    void *ptr = local_malloc(sizeof(size) + bytes);
    if (!ptr) return nullptr;
    memcpy(ptr, &bytes, sizeof(bytes));
    memset((char *)ptr + sizeof(size), 0, bytes);
    m_mallocSize += bytes;
    return ((char *)ptr + sizeof(size));
#endif
  }
  void imFree(void *ptr
#ifdef IM_MEMORY_CHECK
, int ln
#endif
  ) {
    assertx(ptr);
    size_t size;
    void *sizePtr = (char *)ptr - sizeof(size);
    memcpy(&size, sizePtr, sizeof(size));
    m_mallocSize -= size;
#ifdef IM_MEMORY_CHECK
    void *lnPtr = (char *)sizePtr - sizeof(ln);
    int count = m_alloced.erase((char*)sizePtr - sizeof(ln));
    assertx(count == 1); // double free on failure
    assertx(m_mallocSize <= (size_t)Cfg::Server::ImageMemoryMaxBytes);
    local_free(lnPtr);
#else
    assertx(m_mallocSize <= (size_t)Cfg::Server::ImageMemoryMaxBytes);
    local_free(sizePtr);
#endif
  }

  // wrapper of realloc, the original buffer is freed on failure
  void *imRealloc(void *ptr, size_t size
#ifdef IM_MEMORY_CHECK
, int ln
#endif
  ) {
    assertx(m_mallocSize <= (size_t)Cfg::Server::ImageMemoryMaxBytes);

#ifdef IM_MEMORY_CHECK
    if (!ptr) return imMalloc(size, ln);
    if (!size) {
      imFree(ptr, ln);
      return nullptr;
    }
#else
    if (!ptr) return imMalloc(size);
    if (!size) {
      imFree(ptr);
      return nullptr;
    }
#endif
    void *sizePtr = (char *)ptr - sizeof(size);
    size_t oldSize = 0;
    if (ptr) memcpy(&oldSize, sizePtr, sizeof(oldSize));
    ssize_t diff = size - oldSize;
    void *tmp;

#ifdef IM_MEMORY_CHECK
    void *lnPtr = (char *)sizePtr - sizeof(ln);
    if (size > (size_t)Cfg::Server::ImageMemoryMaxBytes ||
        m_mallocSize + diff > (size_t)Cfg::Server::ImageMemoryMaxBytes ||
        !(tmp = local_realloc(lnPtr, sizeof(ln) + sizeof(size) + size))) {
      int count = m_alloced.erase(ptr);
      assertx(count == 1); // double free on failure
      local_free(lnPtr);
      return nullptr;
    }
    memcpy(tmp, &ln, sizeof(ln));
    memcpy((char*)tmp + sizeof(ln), &size, sizeof(size));
    m_mallocSize += diff;
    if (tmp != lnPtr) {
      int count = m_alloced.erase(lnPtr);
      assertx(count == 1);
      m_alloced.insert(tmp);
    }
    return ((char *)tmp + sizeof(ln) + sizeof(size));
#else
    if (size > (size_t)Cfg::Server::ImageMemoryMaxBytes ||
        m_mallocSize + diff > (size_t)Cfg::Server::ImageMemoryMaxBytes ||
        !(tmp = local_realloc(sizePtr, sizeof(size) + size))) {
      local_free(sizePtr);
      return nullptr;
    }
    memcpy(tmp, &size, sizeof(size));
    m_mallocSize += diff;
    return ((char *)tmp + sizeof(size));
#endif
  }

#ifdef IM_MEMORY_CHECK
  void imDump(void *ptrs[], int &n) {
    int i = 0;
    for (auto iter = m_alloced.begin(); iter != m_alloced.end(); ++i, ++iter) {
      void *p = *iter;
      assertx(p);
      if (i < n) ptrs[i] = p;
      int ln;
      size_t size;
      memcpy(&ln, p, sizeof(ln));
      memcpy(&size, (char*)p + sizeof(ln), sizeof(size));
      printf("%d: (%p, %lu)\n", ln, p, size);
    }
    n = (i < n) ? i : n;
  }
#endif

private:
  size_t m_mallocSize;
#ifdef IM_MEMORY_CHECK
  std::set<void *> m_alloced;
#endif
};

IMPLEMENT_STATIC_REQUEST_LOCAL(ImageMemoryAlloc, s_ima);

#ifdef IM_MEMORY_CHECK
  #define IM_MALLOC(size) s_ima->imMalloc((size), __LINE__)
  #define IM_CALLOC(nmemb, size) s_ima->imCalloc((nmemb), (size), __LINE__)
  #define IM_FREE(ptr) s_ima->imFree((ptr), __LINE__)
  #define IM_REALLOC(ptr, size) s_ima->imRealloc((ptr), (size), __LINE__)
#else
  #define IM_MALLOC(size) s_ima->imMalloc((size))
  #define IM_CALLOC(nmemb, size) s_ima->imCalloc((nmemb), (size))
  #define IM_FREE(ptr) s_ima->imFree((ptr))
  #define IM_REALLOC(ptr, size) s_ima->imRealloc((ptr), (size))
#endif

#define CHECK_BUFFER(begin, end, size) \
do { \
  if (((char*)end) - ((char*)(begin)) < (size)) { \
    raise_warning("%s/%d: Buffer overrun (%p, %p, %d)", \
                    __FUNCTION__, __LINE__, begin, end, size); \
    return; \
  } \
} while (0)

#define CHECK_BUFFER_R(begin, end, size, retcod) \
do { \
  if (((char*)(end)) - ((char*)(begin)) < (size)) { \
    raise_warning("%s/%d: Buffer overrun (%p, %p, %d, %d)", \
                    __FUNCTION__, __LINE__, begin, end, size, retcod); \
    return retcod; \
  } \
} while (0)

#define CHECK_ALLOC(ptr, size) \
do { \
  if (!(ptr)) { \
    raise_warning("%s/%d: failed to allocate %lu bytes", \
                    __FUNCTION__, __LINE__, ((size_t)(size))); \
    return; \
  } \
} while (0)

#define CHECK_ALLOC_R(ptr, size, retcod) \
do { \
  if (!(ptr)) { \
    raise_warning("%s/%d: failed to allocate %lu bytes", \
                    __FUNCTION__, __LINE__, ((size_t)(size))); \
    return retcod; \
  } \
} while (0)

// original Zend name is _estrndup
static char *php_strndup_impl(const char* s, uint32_t length
#ifdef IM_MEMORY_CHECK
, int ln
#endif
  ) {
  char *p;

#ifdef IM_MEMORY_CHECK
  p = (char *)s_ima->imMalloc((length+1), ln);
#else
  p = (char *)s_ima->imMalloc((length+1));
#endif
  CHECK_ALLOC_R(p, length+1, nullptr);
  memcpy(p, s, length);
  p[length] = 0;
  return p;
}

static char *php_strdup_impl(const char* s
#ifdef IM_MEMORY_CHECK
, int ln
#endif
  ) {
#ifdef IM_MEMORY_CHECK
  return php_strndup_impl(s, strlen(s), ln);
#else
  return php_strndup_impl(s, strlen(s));
#endif
}

#ifdef IM_MEMORY_CHECK
  #define PHP_STRNDUP(var, s, length) \
  do { \
    if (var) s_ima->imFree((var), __LINE__); \
    (var) = php_strndup_impl((s), (length), __LINE__); \
  } while (0)

  #define PHP_STRDUP(var, s) \
  do { \
    if (var) s_ima->imFree((var), __LINE__); \
    (var) = php_strdup_impl(s, __LINE__); \
  } while (0)
#else
  #define PHP_STRNDUP(var, s, length) \
  do { \
    if (var) IM_FREE(var); \
    (var) = php_strndup_impl((s), (length)); \
  } while (0)

  #define PHP_STRDUP(var, s) \
  do { \
    if (var) IM_FREE(var); \
    (var) = php_strdup_impl(s); \
  } while (0)
#endif

enum image_filetype {
  IMAGE_FILETYPE_UNKNOWN=0,
  IMAGE_FILETYPE_GIF=1,
  IMAGE_FILETYPE_JPEG,
  IMAGE_FILETYPE_PNG,
  IMAGE_FILETYPE_SWF,
  IMAGE_FILETYPE_PSD,
  IMAGE_FILETYPE_BMP,
  IMAGE_FILETYPE_TIFF_II, /* intel */
  IMAGE_FILETYPE_TIFF_MM, /* motorola */
  IMAGE_FILETYPE_JPC,
  IMAGE_FILETYPE_JP2,
  IMAGE_FILETYPE_JPX,
  IMAGE_FILETYPE_JB2,
  IMAGE_FILETYPE_SWC,
  IMAGE_FILETYPE_IFF,
  IMAGE_FILETYPE_WBMP,
  /* IMAGE_FILETYPE_JPEG2000 is a userland alias for IMAGE_FILETYPE_JPC */
  IMAGE_FILETYPE_XBM,
  IMAGE_FILETYPE_ICO,
  IMAGE_FILETYPE_WEBP,
  IMAGE_FILETYPE_AVIF,
  IMAGE_FILETYPE_HEIC,

  IMAGE_FILETYPE_COUNT /* Must remain last */
};


// PHP extension STANDARD: image.c
/* file type markers */
static const char php_sig_gif[3] = {'G', 'I', 'F'};
static const char php_sig_psd[4] = {'8', 'B', 'P', 'S'};
static const char php_sig_bmp[2] = {'B', 'M'};
static const char php_sig_swf[3] = {'F', 'W', 'S'};
static const char php_sig_swc[3] = {'C', 'W', 'S'};
static const char php_sig_jpg[3] = {(char) 0xff, (char) 0xd8, (char) 0xff};
static const char php_sig_png[8] =
  {(char) 0x89, (char) 0x50, (char) 0x4e, (char) 0x47,
   (char) 0x0d, (char) 0x0a, (char) 0x1a, (char) 0x0a};
static const char php_sig_tif_ii[4] = {'I','I', (char)0x2A, (char)0x00};
static const char php_sig_tif_mm[4] = {'M','M', (char)0x00, (char)0x2A};
static const char php_sig_jpc[3] = {(char)0xff, (char)0x4f, (char)0xff};
static const char php_sig_jp2[12] =
  {(char)0x00, (char)0x00, (char)0x00, (char)0x0c,
   (char)0x6a, (char)0x50, (char)0x20, (char)0x20,
   (char)0x0d, (char)0x0a, (char)0x87, (char)0x0a};
static const char php_sig_iff[4] = {'F','O','R','M'};
static const char php_sig_ico[4] = {(char)0x00, (char)0x00, (char)0x01,
                                    (char)0x00};
static const char php_sig_riff[4] = {'R', 'I', 'F', 'F'};
static const char php_sig_webp[4] = {'W', 'E', 'B', 'P'};

static struct gfxinfo *php_handle_gif(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  const unsigned char *s;

  if (!stream->seek(3, SEEK_CUR)) return nullptr;
  OptString dim = stream->read(5);
  if (dim.length() != 5) return nullptr;
  s = (unsigned char *)dim.c_str();
  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, (sizeof(struct gfxinfo)), nullptr);
  result->width = (unsigned int)s[0] | (((unsigned int)s[1])<<8);
  result->height = (unsigned int)s[2] | (((unsigned int)s[3])<<8);
  result->bits = s[4]&0x80 ? ((((unsigned int)s[4])&0x07) + 1) : 0;
  result->channels = 3; /* always */
  return result;
}

static struct gfxinfo *php_handle_psd(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  const unsigned char *s;

  if (!stream->seek(11, SEEK_CUR)) return nullptr;

  OptString dim = stream->read(8);
  if (dim.length() != 8) return nullptr;
  s = (unsigned char *)dim.c_str();
  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, (sizeof(struct gfxinfo)), nullptr);
  result->height = (((unsigned int)s[0]) << 24) +
                   (((unsigned int)s[1]) << 16) +
                   (((unsigned int)s[2]) << 8) +
                   ((unsigned int)s[3]);
  result->width =  (((unsigned int)s[4]) << 24) +
                   (((unsigned int)s[5]) << 16) +
                   (((unsigned int)s[6]) << 8) +
                   ((unsigned int)s[7]);
  return result;
}

static struct gfxinfo *php_handle_bmp(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  const unsigned char *s;
  int size;

  if (!stream->seek(11, SEEK_CUR)) return nullptr;

  OptString dim = stream->read(16);
  if (dim.length() != 16) return nullptr;
  s = (unsigned char *)dim.c_str();

  size = (((unsigned int)s[3]) << 24) +
         (((unsigned int)s[2]) << 16) +
         (((unsigned int)s[1]) << 8) +
         ((unsigned int)s[0]);
  if (size == 12) {
    result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
    CHECK_ALLOC_R(result, sizeof(struct gfxinfo), nullptr);
    result->width = (((unsigned int)s[5]) << 8) + ((unsigned int)s[4]);
    result->height = (((unsigned int)s[7]) << 8) + ((unsigned int)s[6]);
    result->bits = ((unsigned int)s[11]);
  } else if (size > 12 && (size <= 64 || size == 108 || size == 124)) {
    result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
    CHECK_ALLOC_R(result, sizeof(struct gfxinfo), nullptr);
    result->width = (((unsigned int)s[7]) << 24) +
                    (((unsigned int)s[6]) << 16) +
                    (((unsigned int)s[5]) << 8) +
                    ((unsigned int)s[4]);
    result->height = (((unsigned int)s[11]) << 24) +
                     (((unsigned int)s[10]) << 16) +
                     (((unsigned int)s[9]) << 8) +
                     ((unsigned int)s[8]);
    result->height = abs((int32_t)result->height);
    result->bits = (((unsigned int)s[15]) << 8) +
                   ((unsigned int)s[14]);
  } else {
    return nullptr;
  }

  return result;
}

static unsigned long int php_swf_get_bits(unsigned char* buffer,
                                          unsigned int pos,
                                          unsigned int count) {
  unsigned int loop;
  unsigned long int result = 0;

  for (loop = pos; loop < pos + count; loop++)
  {
    result = result +
      ((((buffer[loop / 8]) >> (7 - (loop % 8))) & 0x01) <<
      (count - (loop - pos) - 1));
  }
  return result;
}

static struct gfxinfo *php_handle_swc(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;

  long bits;
  unsigned long len=64, szlength;
  int factor=1,maxfactor=16;
  int slength, status=0;
  unsigned char *b, *buf=nullptr;
  OptString bufz;
  OptString tmp;

  b = (unsigned char *)IM_CALLOC(1, len + 1);
  CHECK_ALLOC_R(b, (len + 1), nullptr);

  if (!stream->seek(5, SEEK_CUR)) {
    IM_FREE(b);
    return nullptr;
  }

  OptString a = stream->read(64);
  if (a.length() != 64) {
    IM_FREE(b);
    return nullptr;
  }

  if (uncompress((Bytef*)b, &len, (const Bytef*)a.c_str(), 64) != Z_OK) {
    /* failed to decompress the file, will try reading the rest of the file */
    if (!stream->seek(8, SEEK_SET)) {
      IM_FREE(b);
      return nullptr;
    }

    while (!(tmp = stream->read(8192)).empty()) {
      bufz += tmp;
    }
    slength = bufz.length();
    /*
     * zlib::uncompress() wants to know the output data length
     * if none was given as a parameter
     * we try from input length * 2 up to input length * 2^8
     * doubling it whenever it wasn't big enough
     * that should be eneugh for all real life cases
    */

    do {
      szlength=slength*(1<<factor++);
      buf = (unsigned char *) IM_REALLOC(buf,szlength);
      if (!buf) IM_FREE(b);
      CHECK_ALLOC_R(buf, szlength, nullptr);
      status = uncompress((Bytef*)buf, &szlength,
                          (const Bytef*)bufz.c_str(), slength);
    } while ((status==Z_BUF_ERROR)&&(factor<maxfactor));

    if (status == Z_OK) {
       memcpy(b, buf, len);
    }

    if (buf) {
      IM_FREE(buf);
    }
  }

  if (!status) {
    result = (struct gfxinfo *)IM_CALLOC(1, sizeof (struct gfxinfo));
    if (!result) IM_FREE(b);
    CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);
    bits = php_swf_get_bits (b, 0, 5);
    result->width = (php_swf_get_bits (b, 5 + bits, bits) -
      php_swf_get_bits (b, 5, bits)) / 20;
    result->height = (php_swf_get_bits (b, 5 + (3 * bits), bits) -
      php_swf_get_bits (b, 5 + (2 * bits), bits)) / 20;
  } else {
    result = nullptr;
  }

  IM_FREE(b);
  return result;
}

static struct gfxinfo *php_handle_swf(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  long bits;
  unsigned char *a;

  if (!stream->seek(5, SEEK_CUR)) return nullptr;

  OptString str = stream->read(32);
  if (str.length() != 32) return nullptr;
  a = (unsigned char *)str.c_str();
  result = (struct gfxinfo *)IM_CALLOC(1, sizeof (struct gfxinfo));
  CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);
  bits = php_swf_get_bits (a, 0, 5);
  result->width = (php_swf_get_bits (a, 5 + bits, bits) -
    php_swf_get_bits (a, 5, bits)) / 20;
  result->height = (php_swf_get_bits (a, 5 + (3 * bits), bits) -
    php_swf_get_bits (a, 5 + (2 * bits), bits)) / 20;
  result->bits = 0;
  result->channels = 0;
  return result;
}

static struct gfxinfo *php_handle_png(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  const unsigned char *s;
  /* Width:              4 bytes
   * Height:             4 bytes
   * Bit depth:          1 byte
   * Color type:         1 byte
   * Compression method: 1 byte
   * Filter method:      1 byte
   * Interlace method:   1 byte
   */

  if (!stream->seek(8, SEEK_CUR)) return nullptr;

  OptString dim = stream->read(9);
  if (dim.length() < 9) return nullptr;

  s = (unsigned char *)dim.c_str();
  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);
  result->width = (((unsigned int)s[0]) << 24) +
                   (((unsigned int)s[1]) << 16) +
                   (((unsigned int)s[2]) << 8) +
                   ((unsigned int)s[3]);
  result->height = (((unsigned int)s[4]) << 24) +
                   (((unsigned int)s[5]) << 16) +
                   (((unsigned int)s[6]) << 8) +
                   ((unsigned int)s[7]);
  result->bits = (unsigned int)s[8];
  return result;
}

/* routines to handle JPEG data */

/* some defines for the different JPEG block types */
#define M_SOF0  0xC0      /* Start Of Frame N */
#define M_SOF1  0xC1      /* N indicates which compression process */
#define M_SOF2  0xC2      /* Only SOF0-SOF2 are now in common use */
#define M_SOF3  0xC3
#define M_SOF5  0xC5      /* NB: codes C4 and CC are NOT SOF markers */
#define M_SOF6  0xC6
#define M_SOF7  0xC7
#define M_SOF9  0xC9
#define M_SOF10 0xCA
#define M_SOF11 0xCB
#define M_SOF13 0xCD
#define M_SOF14 0xCE
#define M_SOF15 0xCF
#define M_SOI   0xD8
#define M_EOI   0xD9      /* End Of Image (end of datastream) */
#define M_SOS   0xDA      /* Start Of Scan (begins compressed data) */
#define M_APP0  0xe0
#define M_APP1  0xe1
#define M_APP2  0xe2
#define M_APP3  0xe3
#define M_APP4  0xe4
#define M_APP5  0xe5
#define M_APP6  0xe6
#define M_APP7  0xe7
#define M_APP8  0xe8
#define M_APP9  0xe9
#define M_APP10 0xea
#define M_APP11 0xeb
#define M_APP12 0xec
#define M_APP13 0xed
#define M_APP14 0xee
#define M_APP15 0xef
#define M_COM   0xFE /* COMment */

#define M_PSEUDO 0xFFD8 /* pseudo marker for start of image(byte 0) */

#define M_EXIF  0xE1      /* Exif Attribute Information               */

static unsigned short php_read2(const req::ptr<File>& stream) {
  unsigned char *a;
  OptString str = stream->read(2);
  /* just return 0 if we hit the end-of-file */
  if (str.length() != 2) return 0;
  a = (unsigned char *)str.c_str();
  return (((unsigned short)a[0]) << 8) + ((unsigned short)a[1]);
}

static unsigned int
php_next_marker(const req::ptr<File>& file, int /*last_marker*/, int ff_read) {
  int a=0, marker;

  // get marker byte, swallowing possible padding
  if (!ff_read) {
    size_t extraneous = 0;

    while ((marker = file->getc()) != 0xff) {
      if (marker == EOF) {
        return M_EOI;/* we hit EOF */
      }
      extraneous++;
    }
    if (extraneous) {
      raise_warning("corrupt JPEG data: %zu extraneous bytes before marker",
                    extraneous);
    }
  }
  a = 1;
  do {
    if ((marker = file->getc()) == EOF)
    {
      return M_EOI;/* we hit EOF */
    }
    ++a;
  } while (marker == 0xff);
  if (a < 2)
  {
    return M_EOI; /* at least one 0xff is needed before marker code */
  }
  return (unsigned int)marker;
}

static int php_skip_variable(const req::ptr<File>& stream) {
  off_t length = (unsigned int)php_read2(stream);

  if (length < 2) {
    return 0;
  }
  length = length - 2;
  stream->seek(length, SEEK_CUR);
  return 1;
}

static int php_read_APP(const req::ptr<File>& stream,
                        unsigned int marker,
                        Array& info) {
  unsigned short length;
  unsigned char markername[16];

  length = php_read2(stream);
  if (length < 2) {
    return 0;
  }
  length -= 2;                /* length includes itself */

  OptString buffer;
  if (length == 0) {
    // avoid stream reads of length 0, they trigger a notice
    buffer = String::Empty();
  } else {
    buffer = stream->read(length);
  }
  if (buffer.length() != length) {
    return 0;
  }

  snprintf((char*)markername, sizeof(markername), "APP%d", marker - M_APP0);

  if (!info.exists(OptString((const char *)markername))) {
    /* XXX we only catch the 1st tag of it's kind! */
    info.set(OptString((char*)markername, CopyString), buffer);
  }

  return 1;
}

static
struct gfxinfo *php_handle_jpeg(const req::ptr<File>& file, Array& info) {
  struct gfxinfo *result = nullptr;
  unsigned int marker = M_PSEUDO;
  unsigned short length, ff_read=1;

  for (;;) {
    marker = php_next_marker(file, marker, ff_read);
    ff_read = 0;
    switch (marker) {
    case M_SOF0:
    case M_SOF1:
    case M_SOF2:
    case M_SOF3:
    case M_SOF5:
    case M_SOF6:
    case M_SOF7:
    case M_SOF9:
    case M_SOF10:
    case M_SOF11:
    case M_SOF13:
    case M_SOF14:
    case M_SOF15:
      if (result == nullptr) {
        /* handle SOFn block */
        result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
        CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);
        length = php_read2(file);
        result->bits = file->getc();
        result->height = php_read2(file);
        result->width = php_read2(file);
        result->channels = file->getc();
        if (info.isNull() || length < 8) {
          /* if we don't want an extanded info -> return */
          return result;
        }
        if (!file->seek(length - 8, SEEK_CUR)) {
          /* file error after info */
          return result;
        }
      } else {
        if (!php_skip_variable(file)) {
          return result;
        }
      }
      break;

    case M_APP0:
    case M_APP1:
    case M_APP2:
    case M_APP3:
    case M_APP4:
    case M_APP5:
    case M_APP6:
    case M_APP7:
    case M_APP8:
    case M_APP9:
    case M_APP10:
    case M_APP11:
    case M_APP12:
    case M_APP13:
    case M_APP14:
    case M_APP15:
      if (!info.isNull()) {
        if (!php_read_APP(file, marker, info)) {
          /* read all the app markes... */
          return result;
        }
      } else {
        if (!php_skip_variable(file)) {
          return result;
        }
      }
      break;

    case M_SOS:
    case M_EOI:
      /* we're about to hit image data, or are at EOF. stop processing. */
      return result;

    default:
      if (!php_skip_variable(file)) {
        /* anything else isn't interesting */
        return result;
      }
      break;
    }
  }

  return result; /* perhaps image broken -> no info but size */
}

static unsigned short php_read4(const req::ptr<File>& stream) {
  unsigned char *a;
  OptString str = stream->read(4);
  /* just return 0 if we hit the end-of-file */
  if (str.length() != 4) return 0;
  a = (unsigned char *)str.c_str();
  return (((unsigned int)a[0]) << 24)
       + (((unsigned int)a[1]) << 16)
       + (((unsigned int)a[2]) <<  8)
       + (((unsigned int)a[3]));
}

/* JPEG 2000 Marker Codes */
#define JPEG2000_MARKER_PREFIX 0xFF /* All marker codes start with this */
#define JPEG2000_MARKER_SOC 0x4F /* Start of Codestream */
#define JPEG2000_MARKER_SOT 0x90 /* Start of Tile part */
#define JPEG2000_MARKER_SOD 0x93 /* Start of Data */
#define JPEG2000_MARKER_EOC 0xD9 /* End of Codestream */
#define JPEG2000_MARKER_SIZ 0x51 /* Image and tile size */
#define JPEG2000_MARKER_COD 0x52 /* Coding style default */
#define JPEG2000_MARKER_COC 0x53 /* Coding style component */
#define JPEG2000_MARKER_RGN 0x5E /* Region of interest */
#define JPEG2000_MARKER_QCD 0x5C /* Quantization default */
#define JPEG2000_MARKER_QCC 0x5D /* Quantization component */
#define JPEG2000_MARKER_POC 0x5F /* Progression order change */
#define JPEG2000_MARKER_TLM 0x55 /* Tile-part lengths */
#define JPEG2000_MARKER_PLM 0x57 /* Packet length, main header */
#define JPEG2000_MARKER_PLT 0x58 /* Packet length, tile-part header */
#define JPEG2000_MARKER_PPM 0x60 /* Packed packet headers, main header */
#define JPEG2000_MARKER_PPT 0x61 /* Packed packet headers, tile part header */
#define JPEG2000_MARKER_SOP 0x91 /* Start of packet */
#define JPEG2000_MARKER_EPH 0x92 /* End of packet header */
#define JPEG2000_MARKER_CRG 0x63 /* Component registration */
#define JPEG2000_MARKER_COM 0x64 /* Comment */

/* Main loop to parse JPEG2000 raw codestream structure */
static struct gfxinfo *php_handle_jpc(const req::ptr<File>& file) {
  struct gfxinfo *result = nullptr;
  int highest_bit_depth, bit_depth;
  unsigned char first_marker_id;
  unsigned int i;

  /* JPEG 2000 components can be vastly different from one another.
     Each component can be sampled at a different resolution, use
     a different colour space, have a separate colour depth, and
     be compressed totally differently! This makes giving a single
     "bit depth" answer somewhat problematic. For this implementation
     we'll use the highest depth encountered. */

  /* Get the single byte that remains after the file type indentification */
  first_marker_id = file->getc();

  /* Ensure that this marker is SIZ (as is mandated by the standard) */
  if (first_marker_id != JPEG2000_MARKER_SIZ) {
    raise_warning("JPEG2000 codestream corrupt(Expected SIZ marker "
                    "not found after SOC)");
    return nullptr;
  }

  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);

  php_read2(file); /* Lsiz */
  php_read2(file); /* Rsiz */
  result->width = php_read4(file); /* Xsiz */
  result->height = php_read4(file); /* Ysiz */

#if MBO_0
  php_read4(file); /* XOsiz */
  php_read4(file); /* YOsiz */
  php_read4(file); /* XTsiz */
  php_read4(file); /* YTsiz */
  php_read4(file); /* XTOsiz */
  php_read4(file); /* YTOsiz */
#else
  if (!file->seek(24, SEEK_CUR)) {
    IM_FREE(result);
    return nullptr;
  }
#endif

  result->channels = php_read2(file); /* Csiz */
  if (result->channels > 256) {
    IM_FREE(result);
    return nullptr;
  }

  /* Collect bit depth info */
  highest_bit_depth = bit_depth = 0;
  for (i = 0; i < result->channels; i++) {
    bit_depth = file->getc(); /* Ssiz[i] */
    bit_depth++;
    if (bit_depth > highest_bit_depth) {
      highest_bit_depth = bit_depth;
    }

    file->getc(); /* XRsiz[i] */
    file->getc(); /* YRsiz[i] */
  }

  result->bits = highest_bit_depth;

  return result;
}

/* main loop to parse JPEG 2000 JP2 wrapper format structure */
static struct gfxinfo *php_handle_jp2(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  unsigned int box_length;
  unsigned int box_type;
  char jp2c_box_id[] = {(char)0x6a, (char)0x70, (char)0x32, (char)0x63};

  /* JP2 is a wrapper format for JPEG 2000. Data is contained within "boxes".
     Boxes themselves can be contained within "super-boxes". Super-Boxes can
     contain super-boxes which provides us with a hierarchical storage system.

     It is valid for a JP2 file to contain multiple individual codestreams.
     We'll just look for the first codestream at the root of the box structure
     and handle that.
  */

  for (;;)
  {
    box_length = php_read4(stream); /* LBox */
    /* TBox */
    OptString str = stream->read(sizeof(box_type));
    if (str.length() != sizeof(box_type)) {
      /* Use this as a general "out of stream" error */
      break;
    }
    memcpy(&box_type, str.c_str(), sizeof(box_type));

    if (box_length == 1) {
      /* We won't handle XLBoxes */
      return nullptr;
    }

    if (!memcmp(&box_type, jp2c_box_id, 4))
    {
      /* Skip the first 3 bytes to emulate the file type examination */
      stream->seek(3, SEEK_CUR);

      result = php_handle_jpc(stream);
      break;
    }

    /* Stop if this was the last box */
    if ((int)box_length <= 0) {
      break;
    }

    /* Skip over LBox (Which includes both TBox and LBox itself */
    if (!stream->seek(box_length - 8, SEEK_CUR)) {
      break;
    }
  }

  if (result == nullptr) {
    raise_warning("JP2 file has no codestreams at root level");
  }

  return result;
}

/* tiff constants */
static const int php_tiff_bytes_per_format[] =
  {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 1};

static int get_php_tiff_bytes_per_format(int format) {
  int size = sizeof(php_tiff_bytes_per_format)/sizeof(int);
  if (format >= size) {
    raise_warning("Invalid format %d", format);
    format = 0;
  }
  return php_tiff_bytes_per_format[format];
}

/* uncompressed only */
#define TAG_IMAGEWIDTH              0x0100
#define TAG_IMAGEHEIGHT             0x0101
/* compressed images only */
#define TAG_COMP_IMAGEWIDTH         0xA002
#define TAG_COMP_IMAGEHEIGHT        0xA003

#define TAG_FMT_BYTE       1
#define TAG_FMT_STRING     2
#define TAG_FMT_USHORT     3
#define TAG_FMT_ULONG      4
#define TAG_FMT_URATIONAL  5
#define TAG_FMT_SBYTE      6
#define TAG_FMT_UNDEFINED  7
#define TAG_FMT_SSHORT     8
#define TAG_FMT_SLONG      9
#define TAG_FMT_SRATIONAL 10
#define TAG_FMT_SINGLE    11
#define TAG_FMT_DOUBLE    12

static int php_vspprintf(char **pbuf, size_t max_len,
  ATTRIBUTE_PRINTF_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(3,4);
static int php_vspprintf(char **pbuf, size_t max_len,
                         const char *fmt, ...) {
  va_list arglist;
  char *buf;
  va_start(arglist, fmt);
  int len = vspprintf_ap(&buf, max_len, fmt, arglist);
  if (buf) {
#ifdef IM_MEMORY_CHECK
    *pbuf = php_strndup_impl(buf, len, __LINE__);
#else
    *pbuf = php_strndup_impl(buf, len);
#endif
    free(buf);
  }
  va_end(arglist);
  return len;
}

static int php_vspprintf_ap(char **pbuf, size_t max_len,
    ATTRIBUTE_PRINTF_STRING const char *fmt, va_list ap) ATTRIBUTE_PRINTF(3,0);
static int php_vspprintf_ap(char **pbuf, size_t max_len,
                            const char *fmt, va_list ap) {
  char *buf;
  int len = vspprintf_ap(&buf, max_len, fmt, ap);
  if (buf) {
#ifdef IM_MEMORY_CHECK
    *pbuf = php_strndup_impl(buf, len, __LINE__);
#else
    *pbuf = php_strndup_impl(buf, len);
#endif
    free(buf);
  }
  return len;
}

/* Convert a 16 bit unsigned value from file's native byte order */
static int php_ifd_get16u(void *Short, int motorola_intel) {
  if (motorola_intel) {
    return (((unsigned char *)Short)[0] << 8) | ((unsigned char *)Short)[1];
  } else {
    return (((unsigned char *)Short)[1] << 8) | ((unsigned char *)Short)[0];
  }
}

/* Convert a 16 bit signed value from file's native byte order */
static signed short php_ifd_get16s(void *Short, int motorola_intel) {
  return (signed short)php_ifd_get16u(Short, motorola_intel);
}

/* Convert a 32 bit signed value from file's native byte order */
static int php_ifd_get32s(void *Long, int motorola_intel) {
  if (motorola_intel) {
    return (((unsigned char *)Long)[0] << 24) |
           (((unsigned char *)Long)[1] << 16) |
           (((unsigned char *)Long)[2] << 8) |
           (((unsigned char *)Long)[3] << 0);
  } else {
    return (((unsigned char *)Long)[3] << 24) |
           (((unsigned char *)Long)[2] << 16) |
           (((unsigned char *)Long)[1] << 8) |
           (((unsigned char *)Long)[0] << 0);
  }
}

/* Convert a 32 bit unsigned value from file's native byte order */
static unsigned php_ifd_get32u(void *Long, int motorola_intel) {
  return (unsigned)php_ifd_get32s(Long, motorola_intel) & 0xffffffff;
}

/* main loop to parse TIFF structure */
static struct gfxinfo *php_handle_tiff(const req::ptr<File>& stream,
                                       int motorola_intel) {
  struct gfxinfo *result = nullptr;
  int i, num_entries;
  unsigned char *dir_entry;
  size_t dir_size, entry_value, width=0, height=0, ifd_addr;
  int entry_tag , entry_type;

  OptString ifd_ptr = stream->read(4);
  if (ifd_ptr.length() != 4) return nullptr;
  ifd_addr = php_ifd_get32u((void*)ifd_ptr.c_str(), motorola_intel);
  if (!stream->seek(ifd_addr-8, SEEK_CUR)) return nullptr;
  OptString ifd_data = stream->read(2);
  if (ifd_data.length() != 2) return nullptr;
  num_entries = php_ifd_get16u((void*)ifd_data.c_str(), motorola_intel);
  dir_size = 2/*num dir entries*/ +12/*length of entry*/*
             num_entries +
             4/* offset to next ifd (points to thumbnail or NULL)*/;
  OptString ifd_data2 = stream->read(dir_size-2);
  if ((size_t)ifd_data2.length() != dir_size-2) return nullptr;
  ifd_data += ifd_data2;
  /* now we have the directory we can look how long it should be */
  for(i=0;i<num_entries;i++) {
    dir_entry = (unsigned char*)ifd_data.c_str()+2+i*12;
    entry_tag = php_ifd_get16u(dir_entry+0, motorola_intel);
    entry_type = php_ifd_get16u(dir_entry+2, motorola_intel);
    switch(entry_type) {
      case TAG_FMT_BYTE:
      case TAG_FMT_SBYTE:
        entry_value = (size_t)(dir_entry[8]);
        break;
      case TAG_FMT_USHORT:
        entry_value = php_ifd_get16u(dir_entry+8, motorola_intel);
        break;
      case TAG_FMT_SSHORT:
        entry_value = php_ifd_get16s(dir_entry+8, motorola_intel);
        break;
      case TAG_FMT_ULONG:
        entry_value = php_ifd_get32u(dir_entry+8, motorola_intel);
        break;
      case TAG_FMT_SLONG:
        entry_value = php_ifd_get32s(dir_entry+8, motorola_intel);
        break;
      default:
        continue;
    }
    switch(entry_tag) {
      case TAG_IMAGEWIDTH:
      case TAG_COMP_IMAGEWIDTH:
        width = entry_value;
        break;
      case TAG_IMAGEHEIGHT:
      case TAG_COMP_IMAGEHEIGHT:
        height = entry_value;
        break;
    }
  }
  if ( width && height) {
    /* not the same when in for-loop */
    result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
    CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);
    result->height = height;
    result->width = width;
    result->bits = 0;
    result->channels = 0;
    return result;
  }
  return nullptr;
}

static struct gfxinfo *php_handle_iff(const req::ptr<File>& stream) {
  struct gfxinfo * result;
  char *a;
  int chunkId;
  int size;
  short width, height, bits;

  OptString str = stream->read(8);
  if (str.length() != 8) return nullptr;
  a = (char *)str.c_str();
  if (strncmp(a+4, "ILBM", 4) && strncmp(a+4, "PBM ", 4)) {
    return nullptr;
  }

  /* loop chunks to find BMHD chunk */
  do {
    str = stream->read(8);
    if (str.length() != 8) return nullptr;
    a = (char *)str.c_str();
    chunkId = php_ifd_get32s(a+0, 1);
    size = php_ifd_get32s(a+4, 1);
    if (size < 0) return nullptr;
    if ((size & 1) == 1) {
      size++;
    }
    if (chunkId == 0x424d4844) { /* BMHD chunk */
      if (size < 9) return nullptr;
      str = stream->read(9);
      if (str.length() != 9) return nullptr;
      a = (char *)str.c_str();
      width = php_ifd_get16s(a+0, 1);
      height = php_ifd_get16s(a+2, 1);
      bits = a[8] & 0xff;
      if (width > 0 && height > 0 && bits > 0 && bits < 33) {
        result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
        CHECK_ALLOC_R(result, sizeof (struct gfxinfo), nullptr);
        result->width = width;
        result->height = height;
        result->bits = bits;
        result->channels = 0;
        return result;
      }
    } else {
      if (!stream->seek(size, SEEK_CUR)) return nullptr;
    }
  } while (1);
}

/*
 * int WBMP file format type
 * byte Header Type
 *  byte Extended Header
 *    byte Header Data (type 00 = multibyte)
 *    byte Header Data (type 11 = name/pairs)
 * int Number of columns
 * int Number of rows
 */
static
int php_get_wbmp(const req::ptr<File>& file,
                 struct gfxinfo **result,
                 int check) {
  int i, width = 0, height = 0;

  if (!file->rewind()) {
    return 0;
  }

  /* get type */
  if (file->getc() != 0) {
    return 0;
  }

  /* skip header */
  do {
    i = file->getc();
    if (i < 0) {
      return 0;
    }
  } while (i & 0x80);

  /* get width */
  do {
    i = file->getc();
    if (i < 0) {
      return 0;
    }
    width = (width << 7) | (i & 0x7f);
  } while (i & 0x80);

  /* get height */
  do {
    i = file->getc();
    if (i < 0) {
      return 0;
    }
    height = (height << 7) | (i & 0x7f);
  } while (i & 0x80);

  // maximum valid sizes for wbmp (although 127x127 may be a
  // more accurate one)
  if (!height || !width || height > 2048 || width > 2048) {
    return 0;
  }

  if (!check) {
    (*result)->width = width;
    (*result)->height = height;
  }

  return IMAGE_FILETYPE_WBMP;
}

static struct gfxinfo *php_handle_wbmp(const req::ptr<File>& stream) {
  struct gfxinfo *result =
   (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, (sizeof(struct gfxinfo)), nullptr);

  if (!php_get_wbmp(stream, &result, 0)) {
    IM_FREE(result);
    return nullptr;
  }

  return result;
}

static int php_get_xbm(const req::ptr<File>& stream, struct gfxinfo **result) {
  OptString fline;
  char *iname;
  char *type;
  int value;
  unsigned int width = 0, height = 0;

  if (result) {
    *result = nullptr;
  }
  if (!stream->rewind()) {
    return 0;
  }
  while (!(fline = HHVM_FN(fgets)(OptResource(stream), 0).toString()).empty()) {
    iname = (char *)IM_MALLOC(fline.size() + 1);
    CHECK_ALLOC_R(iname, (fline.size() + 1), 0);
    if (sscanf(fline.c_str(), "#define %s %d", iname, &value) == 2) {
      if (!(type = strrchr(iname, '_'))) {
        type = iname;
      } else {
        type++;
      }

      if (!strcmp("width", type)) {
        width = (unsigned int)value;
        if (height) {
          IM_FREE(iname);
          break;
        }
      }
      if (!strcmp("height", type)) {
        height = (unsigned int)value;
        if (width) {
          IM_FREE(iname);
          break;
        }
      }
    }
    IM_FREE(iname);
  }

  if (width && height) {
    if (result) {
      *result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
      CHECK_ALLOC_R(*result, sizeof(struct gfxinfo), 0);
      (*result)->width = width;
      (*result)->height = height;
    }
    return IMAGE_FILETYPE_XBM;
  }

  return 0;
}

static struct gfxinfo *php_handle_xbm(const req::ptr<File>& stream) {
  struct gfxinfo *result;
  php_get_xbm(stream, &result);
  return result;
}

static struct gfxinfo *php_handle_ico(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  const unsigned char *s;
  int num_icons = 0;

  OptString dim = stream->read(2);
  if (dim.length() != 2) {
    return nullptr;
  }

  s = (unsigned char *)dim.c_str();
  num_icons = (((unsigned int)s[1]) << 8) + ((unsigned int)s[0]);

  if (num_icons < 1 || num_icons > 255) {
    return nullptr;
  }

  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, (sizeof(struct gfxinfo)), nullptr);

  while (num_icons > 0) {
    dim = stream->read(16);
    if (dim.length() != 16) {
      break;
    }

    s = (unsigned char *)dim.c_str();

    if ((((unsigned int)s[7]) << 8) + ((unsigned int)s[6]) >= result->bits) {
      result->width  = (unsigned int)s[0];
      result->height = (unsigned int)s[1];
      result->bits   = (((unsigned int)s[7]) << 8) + ((unsigned int)s[6]);
    }
    num_icons--;
  }

  return result;
}

static struct gfxinfo *php_handle_webp(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;
  const char sig[3] = {'V', 'P', '8'};
  char format;

  OptString buf_str = stream->read(18);
  if (buf_str.length() != 18) {
    return nullptr;
  }

  const unsigned char *buf = (unsigned char *)buf_str.c_str();
  if (memcmp(buf, sig, 3)) {
    return nullptr;
  }
  switch (buf[3]) {
    case ' ':
    case 'L':
    case 'X':
      format = buf[3];
      break;
    default:
      return nullptr;
  }

  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, (sizeof(struct gfxinfo)), nullptr);

  switch (format) {
    case ' ':
      result->width = buf[14] + ((buf[15] & 0x3F) << 8);
      result->height = buf[16] + ((buf[17] & 0x3F) << 8);
      break;
    case 'L':
      result->width = buf[9] + ((buf[10] & 0x3F) << 8) + 1;
      result->height = (buf[10] >> 6) + (buf[11] << 2) + ((buf[12] & 0xF) << 10) + 1;
      break;
    case 'X':
      result->width = buf[12] + (buf[13] << 8) + (buf[14] << 16) + 1;
      result->height = buf[15] + (buf[16] << 8) + (buf[17] << 16) + 1;
      break;
  }
  result->bits = 8; /* always 1 byte */

  return result;
}

static int64_t heif_get_position(void* userdata) {
  auto stream = static_cast<File*>(userdata);
  return stream->tell();
}

static int heif_read(void* data, size_t size, void* userdata) {
  auto stream = static_cast<File*>(userdata);

  // Special handling as read doesn't like 0 passed in for size
  if (size == 0) {
    return heif_error_Ok;
  }
  // An attempt was made to use readImpl instead to avoid memcpy, but
  // unfortunately, tell() returns wrong position if readImpl is used.
  OptString result = stream->read(size);
  if (result.length() != size) {
    return 1; // Failure
  }
  memcpy(data, result.data(), size);
  return heif_error_Ok;
}

static int heif_seek(int64_t position, void* userdata) {
  auto stream = static_cast<File*>(userdata);
  if (!stream->seekable()) {
    return 1; // Failure
  }
  if (stream->seek(position, SEEK_SET)) {
    return heif_error_Ok;
  }
  return 1; // Failure
}

static heif_reader_grow_status heif_wait_for_file_size(
    int64_t targetSize,
    void* userdata) {
  const auto currentPos = heif_get_position(userdata);
  if (targetSize <= currentPos) {
    return heif_reader_grow_status_size_reached;
  }

  auto stream = static_cast<File*>(userdata);
  if (!stream->seek(0, SEEK_END)) {
    return heif_reader_grow_status_error;
  }
  const auto fileSize = stream->tell();
  if (fileSize < 0) {
    return heif_reader_grow_status_error;
  }
  // Restore current position
  if (!stream->seek(currentPos, SEEK_SET)) {
    return heif_reader_grow_status_error;
  }
  if (targetSize > fileSize) {
    return heif_reader_grow_status_error;
  }
  return heif_reader_grow_status_size_reached;
}

static heif_reader create_heif_reader() {
  return heif_reader{
    .reader_api_version = 1,
    .get_position = &heif_get_position,
    .read = &heif_read,
    .seek = &heif_seek,
    .wait_for_file_size = &heif_wait_for_file_size};
}

static struct gfxinfo *php_handle_heif(const req::ptr<File>& stream) {
  struct gfxinfo *result = nullptr;

  if (!stream->rewind()) {
    return nullptr;
  }

  heif_context* ctx = heif_context_alloc();
  SCOPE_EXIT {
    heif_context_free(ctx);
  };

  heif_reader reader = create_heif_reader();
  auto err = heif_context_read_from_reader(ctx, &reader, stream.get(), nullptr);
  if (err.code != 0) {
    raise_notice("Heif context read failed: %s", err.message);
    return nullptr;
  }

  heif_image_handle* handle;
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if (err.code != 0) {
    // Unable to get image handle
    raise_notice("Failed to get primary image handle: %s", err.message);
    return nullptr;
  }

  SCOPE_EXIT {
    heif_image_handle_release(handle);
  };

  result = (struct gfxinfo *)IM_CALLOC(1, sizeof(struct gfxinfo));
  CHECK_ALLOC_R(result, (sizeof(struct gfxinfo)), nullptr);

  result->width = heif_image_handle_get_width(handle);
  result->height = heif_image_handle_get_height(handle);
  // It is easier to not set bits/channels because it will require actual
  // decoding of the image bitstream to get the information using libheif.
  result->bits = 0;
  result->channels = 0;
  return result;
}

static heif_brand2 php_get_heif(const req::ptr<File>& stream) {
  if (!stream->rewind()) {
    return heif_unknown_brand;
  }

  OptString fileType = stream->read(12);
  if (fileType.length() != 12) {
    return heif_unknown_brand;
  }

  return heif_read_main_brand(reinterpret_cast<const uint8_t*>(
    fileType.c_str()), 12);
}

/* Convert internal image_type to mime type */
static const char *php_image_type_to_mime_type(int image_type) {
  switch( image_type) {
  case IMAGE_FILETYPE_GIF:
    return "image/gif";
  case IMAGE_FILETYPE_JPEG:
    return "image/jpeg";
  case IMAGE_FILETYPE_PNG:
    return "image/png";
  case IMAGE_FILETYPE_SWF:
  case IMAGE_FILETYPE_SWC:
    return "application/x-shockwave-flash";
  case IMAGE_FILETYPE_PSD:
    return "image/psd";
  case IMAGE_FILETYPE_BMP:
    return "image/x-ms-bmp";
  case IMAGE_FILETYPE_TIFF_II:
  case IMAGE_FILETYPE_TIFF_MM:
    return "image/tiff";
  case IMAGE_FILETYPE_IFF:
    return "image/iff";
  case IMAGE_FILETYPE_WBMP:
    return "image/vnd.wap.wbmp";
  case IMAGE_FILETYPE_JPC:
    return "application/octet-stream";
  case IMAGE_FILETYPE_JP2:
    return "image/jp2";
  case IMAGE_FILETYPE_XBM:
    return "image/xbm";
  case IMAGE_FILETYPE_ICO:
    return "image/vnd.microsoft.icon";
  case IMAGE_FILETYPE_WEBP:
    return "image/webp";
  case IMAGE_FILETYPE_AVIF:
    return "image/avif";
  case IMAGE_FILETYPE_HEIC:
    return "image/heic";
  default:
  case IMAGE_FILETYPE_UNKNOWN:
    return "application/octet-stream"; /* suppose binary format */
  }
}

/* detect filetype from first bytes */
static int php_getimagetype(const req::ptr<File>& file) {
  OptString fileType = file->read(3);
  if (fileType.length() != 3) {
    raise_notice("Read error!");
    return IMAGE_FILETYPE_UNKNOWN;
  }

  /* BYTES READ: 3 */
  if (!memcmp(fileType.c_str(), php_sig_gif, 3)) {
    return IMAGE_FILETYPE_GIF;
  } else if (!memcmp(fileType.c_str(), php_sig_jpg, 3)) {
    return IMAGE_FILETYPE_JPEG;
  } else if (!memcmp(fileType.c_str(), php_sig_png, 3)) {
    OptString data = file->read(5);
    if (data.length() != 5) {
      raise_notice("Read error!");
      return IMAGE_FILETYPE_UNKNOWN;
    }
    if (!memcmp((fileType + data).c_str(), php_sig_png, 8)) {
      return IMAGE_FILETYPE_PNG;
    } else {
      raise_warning("PNG file corrupted by ASCII conversion");
      return IMAGE_FILETYPE_UNKNOWN;
    }
  } else if (!memcmp(fileType.c_str(), php_sig_swf, 3)) {
    return IMAGE_FILETYPE_SWF;
  } else if (!memcmp(fileType.c_str(), php_sig_swc, 3)) {
    return IMAGE_FILETYPE_SWC;
  } else if (!memcmp(fileType.c_str(), php_sig_psd, 3)) {
    return IMAGE_FILETYPE_PSD;
  } else if (!memcmp(fileType.c_str(), php_sig_bmp, 2)) {
    return IMAGE_FILETYPE_BMP;
  } else if (!memcmp(fileType.c_str(), php_sig_jpc, 3)) {
    return IMAGE_FILETYPE_JPC;
  } else if (!memcmp(fileType.c_str(), php_sig_riff, 3)) {
    OptString data = file->read(9);
    if (data.length() != 9) {
      raise_notice("Read error!");
      return IMAGE_FILETYPE_UNKNOWN;
    }
    fileType += data;
    if (!memcmp(fileType.c_str() + 8, php_sig_webp, 4)) {
      return IMAGE_FILETYPE_WEBP;
    } else {
      return IMAGE_FILETYPE_UNKNOWN;
    }
  }

  OptString data = file->read(1);
  if (data.length() != 1) {
    raise_notice("Read error!");
    return IMAGE_FILETYPE_UNKNOWN;
  }

  /* BYTES READ: 4 */
  fileType += data;
  if (!memcmp(fileType.c_str(), php_sig_tif_ii, 4)) {
    return IMAGE_FILETYPE_TIFF_II;
  } else if (!memcmp(fileType.c_str(), php_sig_tif_mm, 4)) {
    return IMAGE_FILETYPE_TIFF_MM;
  } else if (!memcmp(fileType.c_str(), php_sig_iff, 4)) {
    return IMAGE_FILETYPE_IFF;
  } else if (!memcmp(fileType.c_str(), php_sig_ico, 4)) {
    return IMAGE_FILETYPE_ICO;
  }

  data = file->read(8);
  if (data.length() != 8) {
    raise_notice("Read error!");
    return IMAGE_FILETYPE_UNKNOWN;
  }

  /* BYTES READ: 12 */
  fileType += data;
  if (!memcmp(fileType.c_str(), php_sig_jp2, 12)) {
    return IMAGE_FILETYPE_JP2;
  }

  /* AFTER ALL ABOVE FAILED */
  auto heifBrand = php_get_heif(file);
  if (heifBrand == heif_brand2_heic || heifBrand == heif_brand2_heix) {
    return IMAGE_FILETYPE_HEIC;
  } else if (heifBrand == heif_brand2_avif) {
    return IMAGE_FILETYPE_AVIF;
  }
  if (php_get_wbmp(file, nullptr, 1)) {
    return IMAGE_FILETYPE_WBMP;
  }
  if (php_get_xbm(file, nullptr)) {
    return IMAGE_FILETYPE_XBM;
  }
  return IMAGE_FILETYPE_UNKNOWN;
}

OptString HHVM_FUNCTION(image_type_to_mime_type, int64_t imagetype) {
  switch (imagetype) {
    case IMAGE_FILETYPE_GIF:
      return "image/gif";
    case IMAGE_FILETYPE_JPEG:
      return "image/jpeg";
    case IMAGE_FILETYPE_PNG:
      return "image/png";
    case IMAGE_FILETYPE_SWF:
    case IMAGE_FILETYPE_SWC:
      return "application/x-shockwave-flash";
    case IMAGE_FILETYPE_PSD:
      return "image/psd";
    case IMAGE_FILETYPE_BMP:
      return "image/x-ms-bmp";
    case IMAGE_FILETYPE_TIFF_II:
    case IMAGE_FILETYPE_TIFF_MM:
      return "image/tiff";
    case IMAGE_FILETYPE_IFF:
      return "image/iff";
    case IMAGE_FILETYPE_WBMP:
      return "image/vnd.wap.wbmp";
    case IMAGE_FILETYPE_JPC:
      return "application/octet-stream";
    case IMAGE_FILETYPE_JP2:
      return "image/jp2";
    case IMAGE_FILETYPE_XBM:
      return "image/xbm";
    case IMAGE_FILETYPE_ICO:
      return "image/vnd.microsoft.icon";
    case IMAGE_FILETYPE_WEBP:
      return "image/webp";
    case IMAGE_FILETYPE_AVIF:
      return "image/avif";
    case IMAGE_FILETYPE_HEIC:
      return "image/heic";
    default:
    case IMAGE_FILETYPE_UNKNOWN:
      return "application/octet-stream"; /* suppose binary format */
  }
}

Variant HHVM_FUNCTION(image_type_to_extension,
                     int64_t imagetype, bool include_dot /*=true */) {
  switch (imagetype) {
  case IMAGE_FILETYPE_GIF:
    return include_dot ? OptString(".gif") : OptString("gif");
  case IMAGE_FILETYPE_JPEG:
    return include_dot ? OptString(".jpeg") : OptString("jpeg");
  case IMAGE_FILETYPE_PNG:
    return include_dot ? OptString(".png") : OptString("png");
  case IMAGE_FILETYPE_SWF:
  case IMAGE_FILETYPE_SWC:
    return include_dot ? OptString(".swf") : OptString("swf");
  case IMAGE_FILETYPE_PSD:
    return include_dot ? OptString(".psd") : OptString("psd");
  case IMAGE_FILETYPE_BMP:
  case IMAGE_FILETYPE_WBMP:
    return include_dot ? OptString(".bmp") : OptString("bmp");
  case IMAGE_FILETYPE_TIFF_II:
  case IMAGE_FILETYPE_TIFF_MM:
    return include_dot ? OptString(".tiff") : OptString("tiff");
  case IMAGE_FILETYPE_IFF:
    return include_dot ? OptString(".iff") : OptString("iff");
  case IMAGE_FILETYPE_JPC:
    return include_dot ? OptString(".jpc") : OptString("jpc");
  case IMAGE_FILETYPE_JP2:
    return include_dot ? OptString(".jp2") : OptString("jp2");
  case IMAGE_FILETYPE_JPX:
    return include_dot ? OptString(".jpx") : OptString("jpx");
  case IMAGE_FILETYPE_JB2:
    return include_dot ? OptString(".jb2") : OptString("jb2");
  case IMAGE_FILETYPE_XBM:
    return include_dot ? OptString(".xbm") : OptString("xbm");
  case IMAGE_FILETYPE_ICO:
    return include_dot ? OptString(".ico") : OptString("ico");
  case IMAGE_FILETYPE_WEBP:
    return include_dot ? OptString(".webp") : OptString("webp");
  case IMAGE_FILETYPE_AVIF:
    return include_dot ? OptString(".avif") : OptString("avif");
  case IMAGE_FILETYPE_HEIC:
    return include_dot ? OptString(".heic") : OptString("heic");
  default:
    return false;
  }
}

const StaticString
  s_bits("bits"),
  s_channels("channels"),
  s_mime("mime"),
  s_linespacing("linespacing");


gdImagePtr get_valid_image_resource(const OptResource& image) {
  auto img_res = dyn_cast_or_null<Image>(image);
  if (!img_res || !img_res->get()) {
    raise_warning("supplied resource is not a valid Image resource");
    return nullptr;
  }
  return img_res->get();
}

Variant getImageSize(const req::ptr<File>& stream, Array& imageinfo) {
  int itype = 0;
  struct gfxinfo *result = nullptr;

  imageinfo = Array::CreateDict();
  itype = php_getimagetype(stream);
  switch( itype) {
  case IMAGE_FILETYPE_GIF:
    result = php_handle_gif(stream);
    break;
  case IMAGE_FILETYPE_JPEG:
    result = php_handle_jpeg(stream, imageinfo);
    break;
  case IMAGE_FILETYPE_PNG:
    result = php_handle_png(stream);
    break;
  case IMAGE_FILETYPE_SWF:
    result = php_handle_swf(stream);
    break;
  case IMAGE_FILETYPE_SWC:
    result = php_handle_swc(stream);
    break;
  case IMAGE_FILETYPE_PSD:
    result = php_handle_psd(stream);
    break;
  case IMAGE_FILETYPE_BMP:
    result = php_handle_bmp(stream);
    break;
  case IMAGE_FILETYPE_TIFF_II:
    result = php_handle_tiff(stream, 0);
    break;
  case IMAGE_FILETYPE_TIFF_MM:
    result = php_handle_tiff(stream, 1);
    break;
  case IMAGE_FILETYPE_JPC:
    result = php_handle_jpc(stream);
    break;
  case IMAGE_FILETYPE_JP2:
    result = php_handle_jp2(stream);
    break;
  case IMAGE_FILETYPE_IFF:
    result = php_handle_iff(stream);
    break;
  case IMAGE_FILETYPE_WBMP:
    result = php_handle_wbmp(stream);
    break;
  case IMAGE_FILETYPE_XBM:
    result = php_handle_xbm(stream);
    break;
  case IMAGE_FILETYPE_ICO:
    result = php_handle_ico(stream);
    break;
  case IMAGE_FILETYPE_WEBP:
    result = php_handle_webp(stream);
    break;
  case IMAGE_FILETYPE_AVIF:
    result = php_handle_heif(stream);
    break;
  case IMAGE_FILETYPE_HEIC:
    result = php_handle_heif(stream);
    break;
  default:
  case IMAGE_FILETYPE_UNKNOWN:
    break;
  }

  if (result) {
    DictInit ret(7);
    ret.set((int64_t)0, (int64_t)result->width);
    ret.set((int64_t)1, (int64_t)result->height);
    ret.set(2, itype);
    char *temp;
    php_vspprintf(&temp, 0, "width=\"%d\" height=\"%d\"",
                  result->width, result->height);
    ret.set(3, OptString(temp, CopyString));
    if (temp) IM_FREE(temp);
    if (result->bits != 0) {
      ret.set(s_bits, (int64_t)result->bits);
    }
    if (result->channels != 0) {
      ret.set(s_channels, (int64_t)result->channels);
    }
    ret.set(s_mime, (char*)php_image_type_to_mime_type(itype));
    IM_FREE(result);
    return ret.toVariant();
  } else {
    return false;
  }
}

Variant HHVM_FUNCTION(getimagesize, const OptString& filename,
                      Array& imageinfo) {
  if (auto stream = File::Open(filename, "rb")) {
    return getImageSize(stream, imageinfo);
  }
  return false;
}

Variant HHVM_FUNCTION(getimagesizefromstring, const OptString& imagedata,
                      Array& imageinfo) {
  OptString data = "data://text/plain;base64,";
  data += StringUtil::Base64Encode(imagedata);
  if (auto stream = File::Open(data, "r")) {
    return getImageSize(stream, imageinfo);
  }
  return false;
}

#define PHP_GDIMG_TYPE_GIF      1
#define PHP_GDIMG_TYPE_PNG      2
#define PHP_GDIMG_TYPE_JPG      3
#define PHP_GDIMG_TYPE_WBM      4
#define PHP_GDIMG_TYPE_XBM      5
#define PHP_GDIMG_TYPE_XPM      6
#define PHP_GDIMG_CONVERT_WBM   7
#define PHP_GDIMG_TYPE_GD       8
#define PHP_GDIMG_TYPE_GD2      9
#define PHP_GDIMG_TYPE_GD2PART 10
#define PHP_GDIMG_TYPE_WEBP    11
#define PHP_GD_VERSION_STRING "bundled (2.0.34 compatible)"

#define USE_GD_IOCTX 1

#define CTX_PUTC(c,ctx) ctx->putC(ctx, c)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static req::ptr<File>
php_open_plain_file(const OptString& filename, const char *mode, FILE **fpp) {
  auto file = File::Open(filename, mode);
  auto plain_file = dyn_cast_or_null<PlainFile>(file);
  if (!plain_file) return nullptr;
  if (FILE* fp = plain_file->getStream()) {
  if (fpp) *fpp = fp;
    return file;
  }
  file->close();
  return nullptr;
}

static int php_write(void *buf, uint32_t size) {
  g_context->write((const char *)buf, size);
  return size;
}

static void _php_image_output_putc(struct gdIOCtx* /*ctx*/, int c) {
  /* without the following downcast, the write will fail
   * (i.e., will write a zero byte) for all
   * big endian architectures:
   */
  unsigned char ch = (unsigned char) c;
  php_write(&ch, 1);
}

static int
_php_image_output_putbuf(struct gdIOCtx* /*ctx*/, const void* buf, int len) {
  return php_write((void *)buf, len);
}

static void _php_image_output_ctxfree(struct gdIOCtx *ctx) {
  if (ctx) {
    IM_FREE(ctx);
  }
}
static bool _php_image_output_ctx(const OptResource& image, const OptString& filename,
                                  int quality, int basefilter, int image_type,
                                  char* /*tn*/, void (*func_p)()) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  req::ptr<File> file;
  FILE *fp = nullptr;
  int q = quality, i;
  int f = basefilter;
  gdIOCtx *ctx;

  /* The third (quality) parameter for Wbmp stands for the threshold
     when called from image2wbmp(). The third (quality) parameter for
     Wbmp and Xbm stands for the foreground color index when called
     from imagey<type>().
   */

  if (!filename.empty()) {
    file = php_open_plain_file(filename, "wb", &fp);
    if (!file) {
      raise_warning("Unable to open '%s' for writing", filename.c_str());
      return false;
    }
    ctx = gdNewFileCtx(fp);
  } else {
    ctx = (gdIOCtx *)IM_MALLOC(sizeof(gdIOCtx));
    CHECK_ALLOC_R(ctx, sizeof(gdIOCtx), false);
    ctx->putC = _php_image_output_putc;
    ctx->putBuf = _php_image_output_putbuf;
    ctx->gd_free = _php_image_output_ctxfree;
  }

  switch(image_type) {
  case PHP_GDIMG_CONVERT_WBM:
    if (q<0||q>255) {
      raise_warning("Invalid threshold value '%d'. "
                      "It must be between 0 and 255", q);
    }
    [[fallthrough]];
  case PHP_GDIMG_TYPE_JPG:
    ((void(*)(gdImagePtr, gdIOCtx *, int))(func_p))(im, ctx, q);
    break;
  case PHP_GDIMG_TYPE_PNG:
    ((void(*)(gdImagePtr, gdIOCtx *, int, int))(func_p))(im, ctx, q, f);
    break;
  case PHP_GDIMG_TYPE_WEBP:
    ((void(*)(gdImagePtr, gdIOCtx *, int64_t, int))(func_p))(im, ctx, q, f);
    break;
  case PHP_GDIMG_TYPE_XBM:
  case PHP_GDIMG_TYPE_WBM:
    if (q == -1) { // argc < 3
      for(i=0; i < gdImageColorsTotal(im); i++) {
        if (!gdImageRed(im, i) &&
            !gdImageGreen(im, i) &&
            !gdImageBlue(im, i)) break;
      }
      q = i;
    }
    if (image_type == PHP_GDIMG_TYPE_XBM) {
      ((void(*)(gdImagePtr, char *, int, gdIOCtx *))(func_p))
        (im, (char*)filename.c_str(), q, ctx);
    } else {
      ((void(*)(gdImagePtr, int, gdIOCtx *))(func_p))(im, q, ctx);
    }
    break;
  default:
    ((void(*)(gdImagePtr, gdIOCtx *))(func_p))(im, ctx);
    break;
  }

  ctx->gd_free(ctx);

  if (fp) {
    fflush(fp);
    file->close();
  }

  return true;
}

/* It converts a gd Image to bw using a threshold value */
static void _php_image_bw_convert(gdImagePtr im_org, gdIOCtx *out,
                                  int threshold) {
  gdImagePtr im_dest;
  int white, black;
  int color, color_org, median;
  int dest_height = gdImageSY(im_org);
  int dest_width = gdImageSX(im_org);
  int x, y;

  im_dest = gdImageCreate(dest_width, dest_height);
  if (im_dest == nullptr) {
    raise_warning("Unable to allocate temporary buffer");
    return;
  }

  white = gdImageColorAllocate(im_dest, 255, 255, 255);
  if (white == -1) {
    raise_warning("Unable to allocate the colors for "
                    "the destination buffer");
    return;
  }

  black = gdImageColorAllocate(im_dest, 0, 0, 0);
  if (black == -1) {
    raise_warning("Unable to allocate the colors for "
                    "the destination buffer");
    return;
  }

  if (im_org->trueColor) {
    gdImageTrueColorToPalette(im_org, 1, 256);
  }

  for (y = 0; y < dest_height; y++) {
    for (x = 0; x < dest_width; x++) {
      color_org = gdImageGetPixel(im_org, x, y);
      median = (im_org->red[color_org] +
                im_org->green[color_org] +
                im_org->blue[color_org]) / 3;
      if (median < threshold) {
        color = black;
      } else {
        color = white;
      }
      gdImageSetPixel (im_dest, x, y, color);
    }
  }
  gdImageWBMPCtx (im_dest, black, out);
}

/*
 * converts jpeg/png images to wbmp and resizes them as needed
 */
static bool _php_image_convert(const OptString& f_org, const OptString& f_dest,
                               int dest_height, int dest_width,
                               int threshold, int image_type) {
  gdImagePtr im_org, im_dest, im_tmp;
  req::ptr<File> org_file, dest_file;
  FILE *org, *dest;
  int org_height, org_width;
  int white, black;
  int color, color_org, median;
  int x, y;
  float x_ratio, y_ratio;

  /* Check threshold value */
  if (threshold < 0 || threshold > 8) {
    raise_warning("Invalid threshold value '%d'", threshold);
    return false;
  }

  /* Open origin file */
  org_file = php_open_plain_file(f_org, "rb", &org);
  if (!org_file) {
    return false;
  }

  /* Open destination file */
  dest_file = php_open_plain_file(f_dest, "wb", &dest);
  if (!dest_file) {
    return false;
  }

  switch (image_type) {
  case PHP_GDIMG_TYPE_GIF:
    im_org = gdImageCreateFromGif(org);
    if (im_org == nullptr) {
      raise_warning("Unable to open '%s' Not a valid GIF file",
                      f_org.c_str());
      return false;
    }
    break;

  case PHP_GDIMG_TYPE_JPG:
    im_org = gdImageCreateFromJpeg(org);
    if (im_org == nullptr) {
      raise_warning("Unable to open '%s' Not a valid JPEG file",
                      f_org.c_str());
      return false;
    }
    break;


  case PHP_GDIMG_TYPE_PNG:
    im_org = gdImageCreateFromPng(org);
    if (im_org == nullptr) {
      raise_warning("Unable to open '%s' Not a valid PNG file",
                      f_org.c_str());
      return false;
    }
    break;

  default:
    raise_warning("Format not supported");
    return false;
  }

  org_width  = gdImageSX (im_org);
  org_height = gdImageSY (im_org);

  x_ratio = (float) org_width / (float) dest_width;
  y_ratio = (float) org_height / (float) dest_height;

  if (x_ratio > 1 && y_ratio > 1) {
    if (y_ratio > x_ratio) {
      x_ratio = y_ratio;
    } else {
      y_ratio = x_ratio;
    }
    dest_width = (int) (org_width / x_ratio);
    dest_height = (int) (org_height / y_ratio);
  } else {
    x_ratio = (float) dest_width / (float) org_width;
    y_ratio = (float) dest_height / (float) org_height;

    if (y_ratio < x_ratio) {
      x_ratio = y_ratio;
    } else {
      y_ratio = x_ratio;
    }
    dest_width = (int) (org_width * x_ratio);
    dest_height = (int) (org_height * y_ratio);
  }

  im_tmp = gdImageCreate (dest_width, dest_height);
  if (im_tmp == nullptr) {
    raise_warning("Unable to allocate temporary buffer");
    return false;
  }

  gdImageCopyResized (im_tmp, im_org, 0, 0, 0, 0,
                      dest_width, dest_height, org_width, org_height);

  gdImageDestroy(im_org);

  org_file->close();

  im_dest = gdImageCreate(dest_width, dest_height);
  if (im_dest == nullptr) {
    raise_warning("Unable to allocate destination buffer");
    return false;
  }

  white = gdImageColorAllocate(im_dest, 255, 255, 255);
  if (white == -1) {
    raise_warning("Unable to allocate the colors for "
                    "the destination buffer");
    return false;
  }

  black = gdImageColorAllocate(im_dest, 0, 0, 0);
  if (black == -1) {
    raise_warning("Unable to allocate the colors for "
                    "the destination buffer");
    return false;
  }

  threshold = threshold * 32;

  for (y = 0; y < dest_height; y++) {
    for (x = 0; x < dest_width; x++) {
      color_org = gdImageGetPixel (im_tmp, x, y);
      median = (im_tmp->red[color_org] +
                im_tmp->green[color_org] +
                im_tmp->blue[color_org]) / 3;
      if (median < threshold) {
        color = black;
      } else {
        color = white;
      }
      gdImageSetPixel(im_dest, x, y, color);
    }
  }

  gdImageDestroy(im_tmp);

  gdImageWBMP(im_dest, black , dest);

  fflush(dest);
  dest_file->close();

  gdImageDestroy(im_dest);

  return true;
}

// For quality and type, -1 means that the argument does not exist
static bool
_php_image_output(const OptResource& image, const OptString& filename, int quality,
                  int type, int image_type, char* /*tn*/, void (*func_p)()) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  req::ptr<File> file;
  FILE *fp;
  int q = quality, i, t = type;

  /* The quality parameter for Wbmp stands for the threshold when
     called from image2wbmp() */
  /* When called from imagewbmp() the quality parameter stands
     for the foreground color. Default: black. */
  /* The quality parameter for gd2 stands for chunk size */

  if (!filename.empty()) {
    file = php_open_plain_file(filename, "wb", &fp);
    if (!file) {
      raise_warning("Unable to open '%s' for writing", filename.c_str());
      return false;
    }

    switch (image_type) {
    case PHP_GDIMG_CONVERT_WBM:
      if (q == -1) {
        q = 0;
      } else if (q < 0 || q > 255) {
        raise_warning("Invalid threshold value '%d'. "
                        "It must be between 0 and 255", q);
        q = 0;
      }
      gdImageWBMP(im, q, fp);
      break;
    case PHP_GDIMG_TYPE_JPG: {
      // gdImageJpeg
      ((void(*)(gdImagePtr, FILE *, int))(func_p))(im, fp, q);
      break;
    }
    case PHP_GDIMG_TYPE_WBM:
      for (i = 0; i < gdImageColorsTotal(im); i++) {
        if (gdImageRed(im, i) == 0) break;
      }
      // gdImageWBMP
      ((void(*)(gdImagePtr, int, FILE *))(func_p))(im, i, fp);
      break;
    case PHP_GDIMG_TYPE_GD:
      if (im->trueColor) {
        gdImageTrueColorToPalette(im,1,256);
      }
      // gdImageGd
      ((void(*)(gdImagePtr, FILE *))(func_p))(im, fp);
      break;
    case PHP_GDIMG_TYPE_GD2:
      if (q == -1) {
        q = 128;
      }
      // gdImageGd2
      ((void(*)(gdImagePtr, FILE *, int, int))(func_p))(im, fp, q, t);
      break;
    default:
      if (q == -1) {
        q = 128;
      }
      ((void(*)(gdImagePtr, FILE *, int, int))(func_p))(im, fp, q, t);
      break;
    }
    fflush(fp);
    file->close();
  } else {
    int   b;
    FILE *tmp;
    char  buf[4096];
    char path[PATH_MAX];

    // open a temporary file
    snprintf(path, sizeof(path), "/tmp/XXXXXX");
    int fd = mkstemp(path);
    if (fd == -1 || (tmp = fdopen(fd, "r+b")) == nullptr) {
      if (fd != -1) close(fd);
      raise_warning("Unable to open temporary file");
      return false;
    }

    switch (image_type) {
    case PHP_GDIMG_CONVERT_WBM:
       if (q == -1) {
          q = 0;
        } else if (q < 0 || q > 255) {
          raise_warning("Invalid threshold value '%d'. "
                          "It must be between 0 and 255", q);
         q = 0;
        }
      gdImageWBMP(im, q, tmp);
      break;
    case PHP_GDIMG_TYPE_JPG:
      ((void(*)(gdImagePtr, FILE *, int))(func_p))(im, tmp, q);
      break;
    case PHP_GDIMG_TYPE_WBM:
      for (i = 0; i < gdImageColorsTotal(im); i++) {
        if (gdImageRed(im, i) == 0) {
          break;
        }
      }
      ((void(*)(gdImagePtr, int, FILE *))(func_p))(im, q, tmp);
      break;
    case PHP_GDIMG_TYPE_GD:
      if (im->trueColor) {
        gdImageTrueColorToPalette(im,1,256);
      }
      ((void(*)(gdImagePtr, FILE *))(func_p))(im, tmp);
      break;
    case PHP_GDIMG_TYPE_GD2:
      if (q == -1) {
        q = 128;
      }
      ((void(*)(gdImagePtr, FILE *, int, int))(func_p))(im, tmp, q, t);
      break;
    default:
      ((void(*)(gdImagePtr, FILE *))(func_p))(im, tmp);
      break;
    }

    fseek(tmp, 0, SEEK_SET);

    while ((b = fread(buf, 1, sizeof(buf), tmp)) > 0) {
      g_context->write(buf, b);
    }

    fclose(tmp);
    /* make sure that the temporary file is removed */
    unlink((const char *)path);
  }
  return true;
}

static gdImagePtr _php_image_create_from(const OptString& filename,
                                         int srcX, int srcY,
                                         int width, int height,
                                         int image_type, const char *tn,
                                         gdImagePtr(*func_p)(),
                                         gdImagePtr(*ioctx_func_p)()) {
  VMRegAnchor _;
  gdImagePtr im = nullptr;

  if (image_type == PHP_GDIMG_TYPE_GD2PART) {
    if (width < 1 || height < 1) {
      raise_warning("Zero width or height not allowed");
      return nullptr;
    }
  }
  auto file = File::Open(filename, "rb");
  if (!file) {
    raise_warning("failed to open stream: %s", filename.c_str());
    return nullptr;
  }

  FILE *fp = nullptr;
  auto plain_file = dyn_cast<PlainFile>(file);
  if (plain_file) {
    fp = plain_file->getStream();
  } else  if (ioctx_func_p) {
    /* we can create an io context */
    gdIOCtx* io_ctx;

    // copy all
    OptString buff = file->read(8192);
    OptString str;
    do {
      str = file->read(8192);
      buff += str;
    } while (!str.empty());

    if (buff.empty()) {
      raise_warning("Cannot read image data");
      return nullptr;
    }

    io_ctx = gdNewDynamicCtxEx(buff.length(), (char *)buff.c_str(), 0);
    if (!io_ctx) {
      raise_warning("Cannot allocate GD IO context");
      return nullptr;
    }

    if (image_type == PHP_GDIMG_TYPE_GD2PART) {
      im =
        ((gdImagePtr(*)(gdIOCtx *, int, int, int, int))(ioctx_func_p))
          (io_ctx, srcX, srcY, width, height);
    } else {
      im = ((gdImagePtr(*)(gdIOCtx *))(ioctx_func_p))(io_ctx);
    }
    io_ctx->gd_free(io_ctx);
  }
  else {
    /* TODO: try and force the stream to be FILE* */
    assertx(false);
  }

  if (!im && fp) {
    switch (image_type) {
    case PHP_GDIMG_TYPE_GD2PART:
      im = ((gdImagePtr(*)(FILE *, int, int, int, int))(func_p))
             (fp, srcX, srcY, width, height);
      break;

    case PHP_GDIMG_TYPE_JPG:
      im = gdImageCreateFromJpeg(fp);
      break;

    default:
      im = ((gdImagePtr(*)(FILE*))(func_p))(fp);
      break;
    }

    fflush(fp);
  }

  if (im) {
    file->close();
    return im;
  }

  raise_warning("'%s' is not a valid %s file", filename.c_str(), tn);
  file->close();
  return nullptr;
}

static const char php_sig_gd2[3] = {'g', 'd', '2'};

/* getmbi
   ** ------
   ** Get a multibyte integer from a generic getin function
   ** 'getin' can be getc, with in = NULL
   ** you can find getin as a function just above the main function
   ** This way you gain a lot of flexibilty about how this package
   ** reads a wbmp file.
 */
static int getmbi(gdIOCtx *ctx) {
  int i, mbi = 0;

  do {
    i = (ctx->getC)(ctx);
    if (i < 0)
      return (-1);
    mbi = (mbi << 7) | (i & 0x7f);
  } while (i & 0x80);

  return (mbi);
}

/* skipheader
   ** ----------
   ** Skips the ExtHeader. Not needed for the moment
   **
 */
int skipheader (gdIOCtx *ctx) {
  int i;

  do {
    i = (ctx->getC)(ctx);
    if (i < 0) return (-1);
  }
  while (i & 0x80);

  return (0);
}

static int _php_image_type (char data[8]) {
  if (data == nullptr) {
    return -1;
  }

  if (!memcmp(data, php_sig_gd2, 3)) {
    return PHP_GDIMG_TYPE_GD2;
  } else if (!memcmp(data, php_sig_jpg, 3)) {
    return PHP_GDIMG_TYPE_JPG;
  } else if (!memcmp(data, php_sig_png, 3)) {
    if (!memcmp(data, php_sig_png, 8)) {
      return PHP_GDIMG_TYPE_PNG;
    }
  } else if (!memcmp(data, php_sig_gif, 3)) {
    return PHP_GDIMG_TYPE_GIF;
  }
  else {
    gdIOCtx *io_ctx;
    io_ctx = gdNewDynamicCtxEx(8, data, 0);
    if (io_ctx) {
      if (getmbi(io_ctx) == 0 &&
          skipheader(io_ctx) == 0 ) {
        io_ctx->gd_free(io_ctx);
        return PHP_GDIMG_TYPE_WBM;
      } else {
        io_ctx->gd_free(io_ctx);
      }
    }
  }
  return -1;
}

gdImagePtr _php_image_create_from_string(const OptString& image, const char *tn,
                                         gdImagePtr (*ioctx_func_p)()) {
  VMRegAnchor _;
  gdIOCtx *io_ctx;

  io_ctx = gdNewDynamicCtxEx(image.length(), (char *)image.c_str(), 0);

  if (!io_ctx) {
    return nullptr;
  }

  gdImagePtr im = (*(gdImagePtr (*)(gdIOCtx *))ioctx_func_p)(io_ctx);
  if (!im) {
    raise_warning("Passed data is not in '%s' format", tn);
    io_ctx->gd_free(io_ctx);
    return nullptr;
  }

  io_ctx->gd_free(io_ctx);

  return im;
}

static gdFontPtr php_find_gd_font(int size) {
  gdFontPtr font;

  switch (size) {
  case 1:
    font = gdFontTiny;
    break;
  case 2:
    font = gdFontSmall;
    break;
  case 3:
    font = gdFontMediumBold;
    break;
  case 4:
    font = gdFontLarge;
    break;
  case 5:
    font = gdFontGiant;
    break;
  default:
    raise_warning("Unsupported font: %d", size);
    // font = zend_list_find(size - 5, &ind_type);
    // if (!font || ind_type != le_gd_font) {
    if (size < 1) {
      font = gdFontTiny;
    } else {
      font = gdFontGiant;
    }
    break;
  }
  return font;
}

/* workaround for a bug in gd 1.2 */
static void php_gdimagecharup(gdImagePtr im, gdFontPtr f, int x, int y,
                              int c, int color) {
  int cx, cy, px, py, fline;
  cx = 0;
  cy = 0;

  if ((c < f->offset) || (c >= (f->offset + f->nchars))) {
    return;
  }

  fline = (c - f->offset) * f->h * f->w;
  for (py = y; (py > (y - f->w)); py--) {
    for (px = x; (px < (x + f->h)); px++) {
      if (f->data[fline + cy * f->w + cx]) {
        gdImageSetPixel(im, px, py, color);
      }
      cy++;
    }
    cy = 0;
    cx++;
  }
}

/*
 * arg = 0  ImageChar
 * arg = 1  ImageCharUp
 * arg = 2  ImageString
 * arg = 3  ImageStringUp
 */
static bool php_imagechar(const OptResource& image, int size, int x, int y,
                          const OptString& c, int color, int mode) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  int ch = 0;
  gdFontPtr font;

  if (mode < 2) {
    ch = (int)((unsigned char)(c.charAt(0)));
  }

  font = php_find_gd_font(size);

  switch (mode) {
  case 0:
    gdImageChar(im, font, x, y, ch, color);
    break;
  case 1:
    php_gdimagecharup(im, font, x, y, ch, color);
    break;
  case 2:
    for (int i = 0; (i < c.length()); i++) {
      gdImageChar(im, font, x, y, (int)((unsigned char)c.charAt(i)), color);
      x += font->w;
    }
    break;
  case 3:
    for (int i = 0; (i < c.length()); i++) {
      gdImageCharUp(im, font, x, y, (int)c.charAt(i), color);
      y -= font->w;
    }
    break;
  }
  return true;
}

/* arg = 0  normal polygon
   arg = 1  filled polygon */
static bool php_imagepolygon(const OptResource& image,
                             const Array& points, int num_points,
                             int color, int filled) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdPointPtr pts;
  int nelem, i;

  nelem = points.size();
  if (nelem < 6) {
    raise_warning("You must have at least 3 points in your array");
    return false;
  }

  if (nelem < num_points * 2) {
    raise_warning("Trying to use %d points in array with only %d points",
                    num_points, nelem/2);
    return false;
  }

  pts = (gdPointPtr)IM_MALLOC(num_points * sizeof(gdPoint));
  CHECK_ALLOC_R(pts, (num_points * sizeof(gdPoint)), false);

  for (i = 0; i < num_points; i++) {
    if (points.exists(i * 2)) {
      pts[i].x = (int)points[i * 2].toInt64();
    }
    if (points.exists(i * 2 + 1)) {
      pts[i].y = (int)points[i * 2 + 1].toInt64();
    }
  }

  if (filled) {
    gdImageFilledPolygon(im, pts, num_points, color);
  } else {
    color = SetupAntiAliasedColor(im, color);
    gdImagePolygon(im, pts, num_points, color);
  }

  IM_FREE(pts);
  return true;
}

static bool
php_image_filter_negate(gdImagePtr im, int /*arg1*/ /* = 0 */,
                        int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                        int /*arg4*/ /* = 0 */) {
  return gdImageNegate(im) == 1;
}

static bool
php_image_filter_grayscale(gdImagePtr im, int /*arg1*/ /* = 0 */,
                           int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                           int /*arg4*/ /* = 0 */) {
  return gdImageGrayScale(im) == 1;
}

static bool
php_image_filter_brightness(gdImagePtr im, int arg1 /* = 0 */,
                            int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                            int /*arg4*/ /* = 0 */) {
  int brightness = arg1;
  return gdImageBrightness(im, brightness) == 1;
}

static bool
php_image_filter_contrast(gdImagePtr im, int arg1 /* = 0 */,
                          int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                          int /*arg4*/ /* = 0 */) {
  int contrast = arg1;
  return gdImageContrast(im, contrast) == 1;
}

static bool
php_image_filter_colorize(gdImagePtr im, int arg1 /* = 0 */, int arg2 /* = 0 */,
                          int arg3 /* = 0 */, int /*arg4*/ /* = 0 */) {
  int r = arg1;
  int g = arg2;
  int b = arg3;
  int a = arg1;
  return gdImageColor(im, r, g, b, a) == 1;
}

static bool
php_image_filter_edgedetect(gdImagePtr im, int /*arg1*/ /* = 0 */,
                            int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                            int /*arg4*/ /* = 0 */) {
  return gdImageEdgeDetectQuick(im) == 1;
}

static bool
php_image_filter_emboss(gdImagePtr im, int /*arg1*/ /* = 0 */,
                        int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                        int /*arg4*/ /* = 0 */) {
  return gdImageEmboss(im) == 1;
}

static bool
php_image_filter_gaussian_blur(gdImagePtr im, int /*arg1*/ /* = 0 */,
                               int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                               int /*arg4*/ /* = 0 */) {
  return gdImageGaussianBlur(im) == 1;
}

static bool
php_image_filter_selective_blur(gdImagePtr im, int /*arg1*/ /* = 0 */,
                                int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                                int /*arg4*/ /* = 0 */) {
  return gdImageSelectiveBlur(im) == 1;
}

static bool
php_image_filter_mean_removal(gdImagePtr im, int /*arg1*/ /* = 0 */,
                              int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                              int /*arg4*/ /* = 0 */) {
  return gdImageMeanRemoval(im) == 1;
}

static bool
php_image_filter_smooth(gdImagePtr im, int arg1 /* = 0 */,
                        int /*arg2*/ /* = 0 */, int /*arg3*/ /* = 0 */,
                        int /*arg4*/ /* = 0 */) {
  int weight = arg1;
  return gdImageSmooth(im, weight) == 1;
}

static bool
php_image_filter_pixelate(gdImagePtr im, int arg1 /* = 0 */, int arg2 /* = 0 */,
                          int /*arg3*/ /* = 0 */, int /*arg4*/ /* = 0 */) {
  int blocksize = arg1;
  unsigned mode = arg2;
  return gdImagePixelate(im, blocksize, mode) == 1;
}

/*
 * arg = 0  ImageFontWidth
 * arg = 1  ImageFontHeight
 */
static int php_imagefontsize(int size, int arg) {
  gdFontPtr font = php_find_gd_font(size);
  return (arg ? font->h : font->w);
}

#define TTFTEXT_DRAW 0
#define TTFTEXT_BBOX 1

static Variant php_imagettftext_common(int mode, int extended,
                                       const Variant& arg1,
                                       const Variant& arg2,
                                       const Variant& arg3,
                                       const Variant& arg4,
                                       const Variant& arg5 = uninit_variant,
                                       const Variant& arg6 = uninit_variant,
                                       const Variant& arg7 = uninit_variant,
                                       const Variant& arg8 = uninit_variant,
                                       const Variant& arg9 = uninit_variant) {
  gdImagePtr im=nullptr;
  long col = -1, x = -1, y = -1;
  int brect[8];
  double ptsize, angle;
  OptString str;
  OptString fontname;
  Array extrainfo;
  char *error = nullptr;
  gdFTStringExtra strex = {0};

  if (mode == TTFTEXT_BBOX) {
    ptsize = arg1.toDouble();
    angle = arg2.toDouble();
    fontname = arg3.toString();
    str = arg4.toString();
    extrainfo = arg5;
  } else {
    OptResource image = arg1.toResource();
    ptsize = arg2.toDouble();
    angle = arg3.toDouble();
    x = arg4.toInt64();
    y = arg5.toInt64();
    col = arg6.toInt64();
    fontname = arg7.toString();
    str = arg8.toString();
    extrainfo = arg9;
    im = get_valid_image_resource(image);
    if (!im) return false;
  }

  /* convert angle to radians */
  angle = angle * (M_PI/180);

  if (extended && !extrainfo.empty()) {  /* parse extended info */

    /* walk the assoc array */
    for (ArrayIter iter(extrainfo); iter; ++iter) {
      Variant key = iter.first();
      if (!key.isString()) continue;
      Variant item = iter.second();
      if (equal(key, s_linespacing.get())) {
        strex.flags |= gdFTEX_LINESPACE;
        strex.linespacing = item.toDouble();
      }
    }
  }

  FILE *fp = nullptr;
  if (!Cfg::Server::FontPath.empty()) {
    fontname = OptString(Cfg::Server::FontPath.c_str()) +
               HHVM_FN(basename)(fontname);
  }
  auto stream = php_open_plain_file(fontname, "rb", &fp);
  if (!stream) {
    raise_warning("Invalid font filename %s", fontname.c_str());
    return false;
  }
  stream->close();

  if (extended) {
    error = gdImageStringFTEx(im, brect, col, (char*)fontname.c_str(),
                              ptsize, angle, x, y, (char*)str.c_str(),
                              &strex);
  }
  else {
    error = gdImageStringFT(im, brect, col, (char*)fontname.c_str(),
                            ptsize, angle, x, y, (char*)str.c_str());
  }

  if (error) {
    raise_warning("%s", error);
    return false;
  }

  /* return array with the text's bounding box */
  return make_vec_array(
    brect[0],
    brect[1],
    brect[2],
    brect[3],
    brect[4],
    brect[5],
    brect[6],
    brect[7]
  );
}

const StaticString
  s_GD_Version("GD Version"),
  s_FreeType_Support("FreeType Support"),
  s_FreeType_Linkage("FreeType Linkage"),
  s_with_freetype("with freetype"),
  s_with_TTF_library("with TTF library"),
  s_with_unknown_library("with unknown library"),
  s_T1Lib_Support("T1Lib_Support"),
  s_GIF_Read_Support("GIF Read Support"),
  s_GIF_Create_Support("GIF Create Support"),
  s_JPG_Support("JPEG Support"),
  s_PNG_Support("PNG Support"),
  s_WBMP_Support("WBMP Support"),
  s_XPM_Support("XPM Support"),
  s_XBM_Support("XBM Support"),
  s_JIS_mapped_Japanese_Font_Support("JIS-mapped Japanese Font Support");

Array HHVM_FUNCTION(gd_info) {
  Array ret = Array::CreateDict();

  ret.set(s_GD_Version, PHP_GD_VERSION_STRING);

  ret.set(s_FreeType_Support, true);
  ret.set(s_FreeType_Linkage, s_with_freetype);

#ifdef HAVE_LIBT1
  ret.set(s_T1Lib_Support, true);
#else
  ret.set(s_T1Lib_Support, false);
#endif
  ret.set(s_GIF_Read_Support, true);
  ret.set(s_GIF_Create_Support, true);
  ret.set(s_JPG_Support, true);
  ret.set(s_PNG_Support, true);
  ret.set(s_WBMP_Support, true);
  ret.set(s_XPM_Support, false);
  ret.set(s_XBM_Support, true);
#if defined(USE_GD_JISX0208) && defined(HAVE_GD_BUNDLED)
  ret.set(s_JIS_mapped_Japanese_Font_Support, true);
#else
  ret.set(s_JIS_mapped_Japanese_Font_Support, false);
#endif
  return ret;
}

#define FLIPWORD(a) (((a & 0xff000000) >> 24) | \
                     ((a & 0x00ff0000) >> 8) | \
                     ((a & 0x0000ff00) << 8) | \
                     ((a & 0x000000ff) << 24))

Variant HHVM_FUNCTION(imageloadfont, const OptString& /*file*/) {
  throw_not_supported(__func__, "NYI");
  // If you decide to implement this, be careful to avoid the crash from
  // https://github.com/php/php-src/commit/9a60aed6d1925c98b1b40c19b40f5b4b65baa
}

bool HHVM_FUNCTION(imagesetstyle, const OptResource& image, const Array& style) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  int *stylearr;
  int index;
  size_t malloc_size = sizeof(int) * style.size();
  stylearr = (int *)IM_MALLOC(malloc_size);
  CHECK_ALLOC_R(stylearr, malloc_size, false);
  index = 0;
  for (ArrayIter iter(style); iter; ++iter) {
    stylearr[index++] = tvToInt(iter.secondVal());
  }
  gdImageSetStyle(im, stylearr, index);
  IM_FREE(stylearr);
  return true;
}

const StaticString
  s_x("x"),
  s_y("y"),
  s_width("width"),
  s_height("height");

Variant HHVM_FUNCTION(imagecrop, const OptResource& image, const Array& rect) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImagePtr imcropped = nullptr;
  gdRect gdrect;
  if (rect.exists(s_x)) {
    gdrect.x = rect[s_x].toInt64();
  } else {
    raise_warning("imagecrop(): Missing x position");
    return false;
  }
  if (rect.exists(s_y)) {
    gdrect.y = rect[s_y].toInt64();
  } else {
    raise_warning("imagecrop(): Missing y position");
    return false;
  }
  if (rect.exists(s_width)) {
    gdrect.width = rect[s_width].toInt64();
  } else {
    raise_warning("imagecrop(): Missing width position");
    return false;
  }
  if (rect.exists(s_height)) {
    gdrect.height = rect[s_height].toInt64();
  } else {
    raise_warning("imagecrop(): Missing height position");
    return false;
  }

  imcropped = gdImageCrop(im, &gdrect);

  if (!imcropped) {
    return false;
  }
  return Variant(req::make<Image>(imcropped));
}

Variant HHVM_FUNCTION(imagecropauto,
                      const OptResource& image,
                      int64_t mode /* = -1 */,
                      double threshold /* = 0.5f */,
                      int64_t color /* = -1 */) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImagePtr imcropped = nullptr;
  switch (mode) {
    case -1:
      mode = GD_CROP_DEFAULT;
      [[fallthrough]];
    case GD_CROP_DEFAULT:
    case GD_CROP_TRANSPARENT:
    case GD_CROP_BLACK:
    case GD_CROP_WHITE:
    case GD_CROP_SIDES:
      imcropped = gdImageCropAuto(im, mode);
      break;

    case GD_CROP_THRESHOLD:
      if (color < 0) {
        raise_warning("imagecropauto(): Color argument missing "
                      "with threshold mode");
        return false;
      }
      imcropped = gdImageCropThreshold(im, color, (float) threshold);
      break;

    default:
      raise_warning("imagecropauto(): Unknown crop mode");
      return false;
  }
  if (!imcropped) {
    return false;
  }
  return Variant(req::make<Image>(imcropped));
}

Variant HHVM_FUNCTION(imagecreatetruecolor, int64_t width, int64_t height) {
  gdImagePtr im;

  if (width <= 0 || height <= 0 || width >= INT_MAX || height >= INT_MAX) {
    raise_warning("Invalid image dimensions");
    return false;
  }

  im = gdImageCreateTrueColor(width, height);

  if (!im) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

bool HHVM_FUNCTION(imageistruecolor, const OptResource& image) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return im->trueColor;
}

Variant HHVM_FUNCTION(imagetruecolortopalette, const OptResource& image,
    bool dither, int64_t ncolors) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;

  if (ncolors <= 0 || ncolors >= INT_MAX) {
    raise_warning("Number of colors has to be greater than zero");
    return false;
  }
  gdImageTrueColorToPalette(im, dither, ncolors);
  return true;
}

Variant HHVM_FUNCTION(imagecolormatch, const OptResource& image1,
                                       const OptResource& image2) {
  gdImagePtr im1 = get_valid_image_resource(image1);
  if (!im1) return false;
  gdImagePtr im2 = get_valid_image_resource(image2);
  if (!im2) return false;
  int result;

  result = gdImageColorMatch(im1, im2);
  switch (result) {
  case -1:
    raise_warning("Image1 must be TrueColor");
    return false;
  case -2:
    raise_warning("Image2 must be Palette");
    return false;
  case -3:
    raise_warning("Image1 and Image2 must be the same size");
    return false;
  case -4:
    raise_warning("Image2 must have at least one color");
    return false;
  }

  return true;
}

bool HHVM_FUNCTION(imagesetthickness,
    const OptResource& image, int64_t thickness) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageSetThickness(im, thickness);
  return true;
}

bool HHVM_FUNCTION(imagefilledellipse, const OptResource& image,
    int64_t cx, int64_t cy, int64_t width, int64_t height, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageFilledEllipse(im, cx, cy, width, height, color);
  return true;
}

bool HHVM_FUNCTION(imagefilledarc, const OptResource& image,
    int64_t cx, int64_t cy, int64_t width, int64_t height,
    int64_t start, int64_t end, int64_t color, int64_t style) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (end < 0) end %= 360;
  if (start < 0) start %= 360;
  gdImageFilledArc(im, cx, cy, width, height, start, end, color, style);
  return true;
}

Variant HHVM_FUNCTION(imageaffine,
                      const OptResource& image,
                      const Array& affine /* = Array() */,
                      const Array& clip /* = Array() */) {
  gdImagePtr src = get_valid_image_resource(image);
  if (!src) return false;
  gdImagePtr dst = nullptr;
  gdRect rect;
  gdRectPtr pRect = nullptr;
  int nelem = affine.size();
  int i;
  double daffine[6];

  if (nelem != 6) {
    raise_warning("imageaffine(): Affine array must have six elements");
    return false;
  }

  for (i = 0; i < nelem; i++) {
    if (affine[i].isInteger()) {
      daffine[i] = affine[i].toInt64();
    } else if (affine[i].isDouble() || affine[i].isString()) {
      daffine[i] = affine[i].toDouble();
    } else {
      raise_warning("imageaffine(): Invalid type for element %i", i);
      return false;
    }
  }

  if (!clip.empty()) {
    if (clip.exists(s_x)) {
      rect.x = clip[s_x].toInt64();
    } else {
      raise_warning("imageaffine(): Missing x position");
      return false;
    }
    if (clip.exists(s_y)) {
      rect.y = clip[s_y].toInt64();
    } else {
      raise_warning("imageaffine(): Missing y position");
      return false;
    } if (clip.exists(s_width)) {
      rect.width = clip[s_width].toInt64();
    } else {
      raise_warning("imageaffine(): Missing width position");
      return false;
    }
    if (clip.exists(s_height)) {
      rect.height = clip[s_height].toInt64();
    } else {
      raise_warning("imageaffine(): Missing height position");
      return false;
    }
    pRect = &rect;
  } else {
    rect.x = -1;
    rect.y = -1;
    rect.width = gdImageSX(src);
    rect.height = gdImageSY(src);
    pRect = nullptr;
  }

  if (gdTransformAffineGetImage(&dst, src, pRect, daffine) != GD_TRUE) {
    return false;
  }
  return Variant(req::make<Image>(dst));
}

Variant HHVM_FUNCTION(imageaffinematrixconcat,
                      const Array& m1,
                      const Array& m2) {
  int nelem1 = m1.size();
  int nelem2 = m2.size();
  int i;
  double dm1[6];
  double dm2[6];
  double dmr[6];
  Array ret = Array::CreateDict();

  if (nelem1 != 6 || nelem2 != 6) {
    raise_warning("imageaffinematrixconcat(): Affine array must "
                  "have six elements");
    return false;
  }

  for (i = 0; i < 6; i++) {
    if (m1[i].isInteger()) {
      dm1[i] = m1[i].toInt64();
    } else if (m1[i].isDouble() || m1[i].isString()) {
      dm1[i] = m1[i].toDouble();
    } else {
      raise_warning("imageaffinematrixconcat(): Invalid type for "
                    "element %i", i);
      return false;
    }
    if (m2[i].isInteger()) {
      dm2[i] = m2[i].toInt64();
    } else if (m2[i].isDouble() || m2[i].isString()) {
      dm2[i] = m2[i].toDouble();
    } else {
      raise_warning("imageaffinematrixconcat():Invalid type for"
                    "element %i", i);
      return false;
    }
  }
  if (gdAffineConcat(dmr, dm1, dm2) != GD_TRUE) {
    return false;
  }

  for (i = 0; i < 6; i++) {
    ret.set(OptString::FromInt64(i), dmr[i]);
  }
  return ret;
}

Variant HHVM_FUNCTION(imageaffinematrixget,
                      int64_t type,
                      const Variant& options /* = Array() */) {
  Array ret = Array::CreateDict();
  double affine[6];
  int res = GD_FALSE, i;

  switch((gdAffineStandardMatrix)type) {
    case GD_AFFINE_TRANSLATE:
    case GD_AFFINE_SCALE: {
      double x, y;
      Array aoptions = options.toArray();
      if (aoptions.empty()) {
        raise_warning("imageaffinematrixget(): Array expected as options");
        return false;
      }
      if (aoptions.exists(s_x)) {
        x = aoptions[s_x].toDouble();
      } else {
        raise_warning("imageaffinematrixget(): Missing x position");
        return false;
      }
      if (aoptions.exists(s_y)) {
        y = aoptions[s_y].toDouble();
      } else {
        raise_warning("imageaffinematrixget(): Missing x position");
        return false;
      }

      if (type == GD_AFFINE_TRANSLATE) {
        res = gdAffineTranslate(affine, x, y);
      } else {
        res = gdAffineScale(affine, x, y);
      }
      break;
    }

    case GD_AFFINE_ROTATE:
    case GD_AFFINE_SHEAR_HORIZONTAL:
    case GD_AFFINE_SHEAR_VERTICAL: {
      double angle;
      double doptions = options.toDouble();
      if (!doptions) {
        raise_warning("imageaffinematrixget(): Number is expected as option");
        return false;
      }

      angle = doptions;

      if (type == GD_AFFINE_SHEAR_HORIZONTAL) {
        res = gdAffineShearHorizontal(affine, angle);
      } else if (type == GD_AFFINE_SHEAR_VERTICAL) {
        res = gdAffineShearVertical(affine, angle);
      } else {
        res = gdAffineRotate(affine, angle);
      }
      break;
    }

    default:
      raise_warning("imageaffinematrixget():Invalid type for "
                    "element %" PRId64, type);
      return false;
  }

  if (res == GD_FALSE) {
    return false;
  } else {
    for (i = 0; i < 6; i++) {
      ret.set(OptString::FromInt64(i), affine[i]);
    }
  }
  return ret;
}

bool HHVM_FUNCTION(imagealphablending, const OptResource& image,
                                       bool blendmode) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageAlphaBlending(im, blendmode);
  return true;
}

bool HHVM_FUNCTION(imagesavealpha, const OptResource& image, bool saveflag) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageSaveAlpha(im, saveflag);
  return true;
}

bool HHVM_FUNCTION(imagelayereffect, const OptResource& image, int64_t effect) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageAlphaBlending(im, effect);
  return true;
}

Variant HHVM_FUNCTION(imagecolorallocatealpha,
    const OptResource& image,
    int64_t red, int64_t green, int64_t blue, int64_t alpha) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  int ct = gdImageColorAllocateAlpha(im, red, green, blue, alpha);
  if (ct < 0) {
    return false;
  }
  return ct;
}

Variant HHVM_FUNCTION(imagecolorresolvealpha, const OptResource& image,
    int64_t red, int64_t green, int64_t blue, int64_t alpha) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorResolveAlpha(im, red, green, blue, alpha);
}

Variant HHVM_FUNCTION(imagecolorclosestalpha,
    const OptResource& image,
    int64_t red, int64_t green, int64_t blue, int64_t alpha) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorClosestAlpha(im, red, green, blue, alpha);
}

Variant HHVM_FUNCTION(imagecolorexactalpha, const OptResource& image,
    int64_t red, int64_t green, int64_t blue, int64_t alpha) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorExactAlpha(im, red, green, blue, alpha);
}

bool HHVM_FUNCTION(imagecopyresampled,
    const OptResource& dst_im, const OptResource& src_im,
    int64_t dst_x, int64_t dst_y, int64_t src_x, int64_t src_y,
    int64_t dst_w, int64_t dst_h, int64_t src_w, int64_t src_h) {
  gdImagePtr im_src = get_valid_image_resource(src_im);
  if (!im_src) return false;
  gdImagePtr im_dst = get_valid_image_resource(dst_im);
  if (!im_dst) return false;
  gdImageCopyResampled(im_dst, im_src, dst_x, dst_y, src_x, src_y,
                       dst_w, dst_h, src_w, src_h);
  return true;
}

Variant
HHVM_FUNCTION(imagerotate, const OptResource& source_image, double angle,
              int64_t bgd_color, int64_t /*ignore_transparent*/ /* = 0 */) {
  gdImagePtr im_src = get_valid_image_resource(source_image);
  if (!im_src) return false;
  gdImagePtr im_dst = gdImageRotateInterpolated(im_src, angle, bgd_color);
  if (!im_dst) return false;
  return Variant(req::make<Image>(im_dst));
}

bool HHVM_FUNCTION(imagesettile, const OptResource& image, const OptResource& tile) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImagePtr til = get_valid_image_resource(tile);
  if (!til) return false;
  gdImageSetTile(im, til);
  unsafe_cast_or_null<Image>(image)->m_tile = unsafe_cast_or_null<Image>(tile);
  return true;
}

bool HHVM_FUNCTION(imagesetbrush,
    const OptResource& image, const OptResource& brush) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImagePtr tile = get_valid_image_resource(brush);
  if (!tile) return false;
  gdImageSetBrush(im, tile);
  unsafe_cast_or_null<Image>(image)->m_brush =
    unsafe_cast_or_null<Image>(brush);
  return true;
}

bool HHVM_FUNCTION(imagesetinterpolation,
    const OptResource& image, int64_t method /*=GD_BILINEAR_FIXED*/) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (method == -1) method = GD_BILINEAR_FIXED;
  return gdImageSetInterpolationMethod(im, (gdInterpolationMethod) method);
}

Variant HHVM_FUNCTION(imagecreate, int64_t width, int64_t height) {
  gdImagePtr im;
  if (width <= 0 || height <= 0 || width >= INT_MAX || height >= INT_MAX) {
    raise_warning("Invalid image dimensions");
    return false;
  }
  im = gdImageCreate(width, height);
  if (!im) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

int64_t HHVM_FUNCTION(imagetypes) {
  int ret=0;
  ret = IMAGE_TYPE_GIF;
  ret |= IMAGE_TYPE_JPEG;
  ret |= IMAGE_TYPE_PNG;
  ret |= IMAGE_TYPE_WBMP;
  return ret;
}

Variant HHVM_FUNCTION(imagecreatefromstring, const OptString& data) {
  gdImagePtr im;
  int imtype;
  char sig[8];

  if (data.length() < 8) {
    raise_warning("Empty string or invalid image");
    return false;
  }
  memcpy(sig, data.c_str(), 8);
  imtype = _php_image_type(sig);
  switch (imtype) {
  case PHP_GDIMG_TYPE_JPG:
    im = _php_image_create_from_string(data, "JPEG",
      (gdImagePtr(*)())gdImageCreateFromJpegCtx);
    break;

  case PHP_GDIMG_TYPE_PNG:
    im = _php_image_create_from_string(data, "PNG",
      (gdImagePtr(*)())gdImageCreateFromPngCtx);
    break;

  case PHP_GDIMG_TYPE_GIF:
    im = _php_image_create_from_string(data, "GIF",
      (gdImagePtr(*)())gdImageCreateFromGifCtx);
    break;

  case PHP_GDIMG_TYPE_WBM:
    im = _php_image_create_from_string(data, "WBMP",
      (gdImagePtr(*)())gdImageCreateFromWBMPCtx);
    break;

  case PHP_GDIMG_TYPE_GD2:
    im = _php_image_create_from_string(data, "GD2",
      (gdImagePtr(*)())gdImageCreateFromGd2Ctx);
    break;

  default:
    raise_warning("Data is not in a recognized format");
    return false;
  }

  if (!im) {
    raise_warning("Couldn't create GD Image Stream out of Data");
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromgif, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_GIF, "GIF",
                           (gdImagePtr(*)())gdImageCreateFromGif,
                           (gdImagePtr(*)())gdImageCreateFromGifCtx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromjpeg, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_JPG, "JPEG",
                           (gdImagePtr(*)())gdImageCreateFromJpeg,
                           (gdImagePtr(*)())gdImageCreateFromJpegCtx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefrompng, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_PNG, "PNG",
                           (gdImagePtr(*)())gdImageCreateFromPng,
                           (gdImagePtr(*)())gdImageCreateFromPngCtx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromxbm, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_XBM, "XBM",
                           (gdImagePtr(*)())gdImageCreateFromXbm,
                           (gdImagePtr(*)())nullptr);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromwbmp, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_WBM, "WBMP",
                           (gdImagePtr(*)())gdImageCreateFromWBMP,
                           (gdImagePtr(*)())gdImageCreateFromWBMPCtx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromgd, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_GD, "GD",
                           (gdImagePtr(*)())gdImageCreateFromGd,
                           (gdImagePtr(*)())gdImageCreateFromGdCtx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromgd2, const OptString& filename) {
  gdImagePtr im =
    _php_image_create_from(filename, -1, -1, -1, -1,
                           PHP_GDIMG_TYPE_GD2, "GD2",
                           (gdImagePtr(*)())gdImageCreateFromGd2,
                           (gdImagePtr(*)())gdImageCreateFromGd2Ctx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

Variant HHVM_FUNCTION(imagecreatefromgd2part,
    const OptString& filename,
    int64_t srcx, int64_t srcy, int64_t width, int64_t height) {
  gdImagePtr im =
    _php_image_create_from(filename, srcx, srcy, width, height,
                           PHP_GDIMG_TYPE_GD2PART, "GD2",
                           (gdImagePtr(*)())gdImageCreateFromGd2Part,
                           (gdImagePtr(*)())gdImageCreateFromGd2PartCtx);
  if (im == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(im));
}

bool HHVM_FUNCTION(imagegif, const OptResource& image,
    const OptString& filename /* = null_string */) {
  return _php_image_output_ctx(image, filename, -1, -1,
                               PHP_GDIMG_TYPE_GIF, "GIF",
                               (void (*)())gdImageGifCtx);
}

bool HHVM_FUNCTION(imagepng, const OptResource& image,
    const OptString& filename /* = null_string */,
    int64_t quality /* = -1 */, int64_t filters /* = -1 */) {
  return _php_image_output_ctx(image, filename, quality, filters,
                               PHP_GDIMG_TYPE_PNG, "PNG",
                               (void (*)())gdImagePngCtxEx);
}

bool HHVM_FUNCTION(imagejpeg, const OptResource& image,
    const OptString& filename /* = null_string */, int64_t quality /* = -1 */) {
  return _php_image_output_ctx(image, filename, quality, -1,
                               PHP_GDIMG_TYPE_JPG, "JPEG",
                               (void (*)())gdImageJpegCtx);
}

bool HHVM_FUNCTION(imagewbmp, const OptResource& image,
    const OptString& filename /* = null_string */,
    int64_t foreground /* = -1 */) {
  return _php_image_output_ctx(image, filename, foreground, -1,
                               PHP_GDIMG_TYPE_WBM, "WBMP",
                               (void (*)())gdImageWBMPCtx);
}

bool HHVM_FUNCTION(imagegd, const OptResource& image,
    const OptString& filename /* = null_string */) {
  return _php_image_output(image, filename, -1, -1, PHP_GDIMG_TYPE_GD, "GD",
                           (void (*)())gdImageGd);
}

bool HHVM_FUNCTION(imagegd2, const OptResource& image,
    const OptString& filename /* = null_string */,
    int64_t chunk_size /* = 0 */, int64_t type /* = 0 */) {
  return _php_image_output(image, filename, chunk_size, type,
                           PHP_GDIMG_TYPE_GD2, "GD2",
                           (void (*)())gdImageGd2);
}

bool HHVM_FUNCTION(imagedestroy, const OptResource& image) {
  auto img_res = cast<Image>(image);
  gdImagePtr im = img_res->get();
  if (!im) return false;
  img_res->reset();
  return true;
}

Variant HHVM_FUNCTION(imagecolorallocate,
    const OptResource& image,
    int64_t red, int64_t green, int64_t blue) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  int ct = gdImageColorAllocate(im, red, green, blue);
  if (ct < 0) {
    return false;
  }
  return ct;
}

Variant HHVM_FUNCTION(imagepalettecopy,
    const OptResource& dst,
    const OptResource& src) {
  gdImagePtr dstim = cast<Image>(dst)->get();
  gdImagePtr srcim = cast<Image>(src)->get();
  if (!dstim || !srcim)
    return false;
  gdImagePaletteCopy(dstim, srcim);
  return true;
}

Variant HHVM_FUNCTION(imagecolorat,
    const OptResource& image, int64_t x, int64_t y) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (gdImageTrueColor(im)) {
    if (im->tpixels && gdImageBoundsSafe(im, x, y)) {
      return gdImageTrueColorPixel(im, x, y);
    } else {
      raise_notice("%" PRId64 ",%" PRId64 " is out of bounds", x, y);
      return false;
    }
  } else {
    if (im->pixels && gdImageBoundsSafe(im, x, y)) {
      return (im->pixels[y][x]);
    } else {
      raise_notice("%" PRId64 ",%" PRId64 " is out of bounds", x, y);
      return false;
    }
  }
}

Variant HHVM_FUNCTION(imagecolorclosest,
    const OptResource& image, int64_t red, int64_t green, int64_t blue) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorClosest(im, red, green, blue);
}

Variant HHVM_FUNCTION(imagecolorclosesthwb, const OptResource& image,
    int64_t red, int64_t green, int64_t blue) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorClosestHWB(im, red, green, blue);
}

bool HHVM_FUNCTION(imagecolordeallocate, const OptResource& image,
                                         int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  /* We can return right away for a truecolor image as deallocating colours
     is meaningless here */
  if (gdImageTrueColor(im)) return true;

  if (color >= 0 && color < gdImageColorsTotal(im)) {
    gdImageColorDeallocate(im, color);
    return true;
  } else {
    raise_warning("Color index %" PRId64 " out of range", color);
    return false;
  }
}

Variant HHVM_FUNCTION(imagecolorresolve, const OptResource& image,
    int64_t red, int64_t green, int64_t blue) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorResolve(im, red, green, blue);
}

Variant HHVM_FUNCTION(imagecolorexact, const OptResource& image,
    int64_t red, int64_t green, int64_t blue) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageColorExact(im, red, green, blue);
}

Variant HHVM_FUNCTION(imagecolorset, const OptResource& image,
    int64_t index, int64_t red, int64_t green, int64_t blue) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (index >= 0 && index < gdImageColorsTotal(im)) {
    im->red[index] = red;
    im->green[index] = green;
    im->blue[index] = blue;
    return true;
  } else {
    return false;
  }
}

const StaticString
  s_red("red"),
  s_green("green"),
  s_blue("blue"),
  s_alpha("alpha");

Variant HHVM_FUNCTION(imagecolorsforindex, const OptResource& image,
                                           int64_t index) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (index >= 0 &&
      (gdImageTrueColor(im) || index < gdImageColorsTotal(im))) {
    return make_dict_array(
      s_red,  gdImageRed(im,index),
      s_green, gdImageGreen(im,index),
      s_blue, gdImageBlue(im,index),
      s_alpha, gdImageAlpha(im,index)
    );
  }
  raise_warning("Color index %" PRId64 " out of range", index);
  return false;
}

bool HHVM_FUNCTION(imagegammacorrect, const OptResource& image,
    double inputgamma, double outputgamma) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (inputgamma <= 0.0 || outputgamma <= 0.0) {
    raise_warning("Gamma values should be positive");
    return false;
  }
  if (gdImageTrueColor(im))   {
    int x, y, c;

    for (y = 0; y < gdImageSY(im); y++) {
      for (x = 0; x < gdImageSX(im); x++) {
        c = gdImageGetPixel(im, x, y);
        gdImageSetPixel(im, x, y,
          gdTrueColor((int)((pow((pow((gdTrueColorGetRed(c)/255.0),
                      inputgamma)),1.0/outputgamma)*255) + .5),
                      (int)((pow((pow((gdTrueColorGetGreen(c)/255.0),
                      inputgamma)),1.0/outputgamma) * 255) + .5),
                      (int)((pow((pow((gdTrueColorGetBlue(c)/255.0),
                      inputgamma)),1.0/outputgamma) * 255) + .5)));
      }
    }
    return true;
  }
  for (int i = 0; i < gdImageColorsTotal(im); i++) {
    im->red[i] = (int)((pow((pow((im->red[i]/255.0), inputgamma)),
                        1.0/outputgamma)*255) + .5);
    im->green[i] = (int)((pow((pow((im->green[i]/255.0), inputgamma)),
                          1.0/outputgamma)*255) + .5);
    im->blue[i] = (int)((pow((pow((im->blue[i]/255.0), inputgamma)),
                         1.0/outputgamma)*255) + .5);
  }

  return true;
}

bool HHVM_FUNCTION(imagesetpixel, const OptResource& image,
    int64_t x, int64_t y, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageSetPixel(im, x, y, color);
  return true;
}

bool HHVM_FUNCTION(imageline, const OptResource& image,
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  color = SetupAntiAliasedColor(im, color);
  gdImageLine(im, x1, y1, x2, y2, color);
  return true;
}

bool HHVM_FUNCTION(imagedashedline,
    const OptResource& image,
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageDashedLine(im, x1, y1, x2, y2, color);
  return true;
}

bool HHVM_FUNCTION(imagerectangle, const OptResource& image,
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageRectangle(im, x1, y1, x2, y2, color);
  return true;
}

bool HHVM_FUNCTION(imagefilledrectangle, const OptResource& image,
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageFilledRectangle(im, x1, y1, x2, y2, color);
  return true;
}

bool HHVM_FUNCTION(imagearc, const OptResource& image,
    int64_t cx, int64_t cy, int64_t width, int64_t height,
    int64_t start, int64_t end, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (end < 0) end %= 360;
  if (start < 0) start %= 360;
  color = SetupAntiAliasedColor(im, color);
  gdImageArc(im, cx, cy, width, height, start, end, color);
  return true;
}

bool HHVM_FUNCTION(imageellipse, const OptResource& image,
    int64_t cx, int64_t cy, int64_t width, int64_t height, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  color = SetupAntiAliasedColor(im, color);
  gdImageArc(im, cx, cy, width, height, 0, 360, color);
  return true;
}

bool HHVM_FUNCTION(imagefilltoborder, const OptResource& image,
    int64_t x, int64_t y, int64_t border, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageFillToBorder(im, x, y, border, color);
  return true;
}

bool HHVM_FUNCTION(imagefill, const OptResource& image,
    int64_t x, int64_t y, int64_t color) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImageFill(im, x, y, color);
  return true;
}

Variant HHVM_FUNCTION(imagecolorstotal, const OptResource& image) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return (gdImageColorsTotal(im));
}

Variant HHVM_FUNCTION(imagecolortransparent, const OptResource& image,
                                             int64_t color /* = -1 */) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (color != -1) {
    // has color argument
    gdImageColorTransparent(im, color);
  }
  return gdImageGetTransparent(im);
}

TypedValue HHVM_FUNCTION(imageinterlace, const OptResource& image,
                         TypedValue interlace /* = 0 */) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return make_tv<KindOfBoolean>(false);
  if (!tvIsNull(interlace)) {
    // has interlace argument
    gdImageInterlace(im, tvAssertInt(interlace));
  }
  return make_tv<KindOfInt64>(gdImageGetInterlaced(im));
}

bool HHVM_FUNCTION(imagepolygon, const OptResource& image,
    const Array& points, int64_t num_points, int64_t color) {
  return php_imagepolygon(image, points, num_points, color, 0);
}

bool HHVM_FUNCTION(imagefilledpolygon, const OptResource& image,
    const Array& points, int64_t num_points, int64_t color) {
  return php_imagepolygon(image, points, num_points, color, 1);
}

int64_t HHVM_FUNCTION(imagefontwidth, int64_t font) {
  return php_imagefontsize(font, 0);
}

int64_t HHVM_FUNCTION(imagefontheight, int64_t font) {
  return php_imagefontsize(font, 1);
}

bool HHVM_FUNCTION(imagechar, const OptResource& image,
    int64_t font, int64_t x, int64_t y,
    const OptString& c, int64_t color) {
  return php_imagechar(image, font, x, y, c, color, 0);
}

bool HHVM_FUNCTION(imagecharup, const OptResource& image,
    int64_t font, int64_t x, int64_t y,
    const OptString& c, int64_t color) {
  return php_imagechar(image, font, x, y, c, color, 1);
}

bool HHVM_FUNCTION(imagestring, const OptResource& image,
    int64_t font, int64_t x, int64_t y,
    const OptString& str, int64_t color) {
  return php_imagechar(image, font, x, y, str, color, 2);
}

bool HHVM_FUNCTION(imagestringup, const OptResource& image,
    int64_t font, int64_t x, int64_t y,
    const OptString& str, int64_t color) {
  return php_imagechar(image, font, x, y, str, color, 3);
}

bool HHVM_FUNCTION(imagecopy, const OptResource& dst_im, const OptResource& src_im,
    int64_t dst_x, int64_t dst_y,
    int64_t src_x, int64_t src_y, int64_t src_w, int64_t src_h) {
  gdImagePtr im_src = cast<Image>(src_im)->get();
  if (!im_src) return false;
  gdImagePtr im_dst = cast<Image>(dst_im)->get();
  if (!im_dst) return false;
  gdImageCopy(im_dst, im_src, dst_x, dst_y, src_x, src_y, src_w, src_h);
  return true;
}

bool HHVM_FUNCTION(imagecopymerge, const OptResource& dst_im,
    const OptResource& src_im,
    int64_t dst_x, int64_t dst_y, int64_t src_x, int64_t src_y,
    int64_t src_w, int64_t src_h, int64_t pct) {
  gdImagePtr im_src = cast<Image>(src_im)->get();
  if (!im_src) return false;
  gdImagePtr im_dst = cast<Image>(dst_im)->get();
  if (!im_dst) return false;
  gdImageCopyMerge(im_dst, im_src, dst_x, dst_y,
                   src_x, src_y, src_w, src_h, pct);
  return true;
}

bool HHVM_FUNCTION(imagecopymergegray, const OptResource& dst_im,
    const OptResource& src_im,
    int64_t dst_x, int64_t dst_y,
    int64_t src_x, int64_t src_y,
    int64_t src_w, int64_t src_h, int64_t pct) {
  gdImagePtr im_src = cast<Image>(src_im)->get();
  if (!im_src) return false;
  gdImagePtr im_dst = cast<Image>(dst_im)->get();
  if (!im_dst) return false;
  gdImageCopyMergeGray(im_dst, im_src, dst_x, dst_y,
                       src_x, src_y, src_w, src_h, pct);
  return true;
}

bool HHVM_FUNCTION(imagecopyresized, const OptResource& dst_im,
    const OptResource& src_im,
    int64_t dst_x, int64_t dst_y, int64_t src_x, int64_t src_y,
    int64_t dst_w, int64_t dst_h, int64_t src_w, int64_t src_h) {
  gdImagePtr im_src = cast<Image>(src_im)->get();
  if (!im_src) return false;
  gdImagePtr im_dst = cast<Image>(dst_im)->get();
  if (!im_dst) return false;
  if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) {
    raise_warning("Invalid image dimensions");
    return false;
  }
  gdImageCopyResized(im_dst, im_src,
                     dst_x, dst_y, src_x, src_y,
                     dst_w, dst_h, src_w, src_h);
  return true;
}

Variant HHVM_FUNCTION(imagesx, const OptResource& image) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageSX(im);
}

Variant HHVM_FUNCTION(imagesy, const OptResource& image) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  return gdImageSY(im);
}

Variant HHVM_FUNCTION(imageftbbox, double size, double angle,
    const OptString& font_file, const OptString& text,
    const Array& extrainfo /*=[] */) {
  return php_imagettftext_common(TTFTEXT_BBOX, 1,
                                 size, angle, font_file, text, extrainfo);
}

Variant HHVM_FUNCTION(imagefttext,
    const OptResource& image,
    const Variant& size, const Variant& angle,
    int64_t x, int64_t y, int64_t col,
    const OptString& font_file, const OptString& text,
    const Array& extrainfo) {
  return php_imagettftext_common(TTFTEXT_DRAW, 1,
    image, size, angle, x, y, col, font_file, text, extrainfo);
}

Variant HHVM_FUNCTION(imagettfbbox, double size, double angle,
                     const OptString& fontfile, const OptString& text) {
  return php_imagettftext_common(TTFTEXT_BBOX, 0,
                                 size, angle, fontfile, text);
}

Variant HHVM_FUNCTION(imagettftext, const OptResource& image,
    const Variant& size, const Variant& angle,
    int64_t x, int64_t y, int64_t color,
    const OptString& fontfile, const OptString& text) {
  return php_imagettftext_common(TTFTEXT_DRAW, 0,
                                 image, size.toDouble(), angle.toDouble(),
                                 x, y, color, fontfile, text);
}

bool HHVM_FUNCTION(image2wbmp, const OptResource& image,
                   const OptString& filename /* = null_string */,
                   int64_t threshold /* = -1 */) {
  return _php_image_output(image, filename, threshold, -1,
                           PHP_GDIMG_CONVERT_WBM, "WBMP",
                           (void (*)())_php_image_bw_convert);
}

bool HHVM_FUNCTION(jpeg2wbmp, const OptString& jpegname, const OptString& wbmpname,
    int64_t dest_height, int64_t dest_width, int64_t threshold) {
  return _php_image_convert(jpegname, wbmpname, dest_height, dest_width,
                            threshold, PHP_GDIMG_TYPE_JPG);
}

bool HHVM_FUNCTION(png2wbmp, const OptString& pngname, const OptString& wbmpname,
    int64_t dest_height, int64_t dest_width, int64_t threshold) {
  return _php_image_convert(pngname, wbmpname, dest_height, dest_width,
                            threshold, PHP_GDIMG_TYPE_PNG);
}

bool HHVM_FUNCTION(imagefilter, const OptResource& res,
    int64_t filtertype,
    const Variant& arg1 /*=0*/, const Variant& arg2 /*=0*/,
    const Variant& arg3 /*=0*/, const Variant& arg4 /*=0*/) {
  gdImagePtr im = get_valid_image_resource(res);
  if (!im) return false;

/* Exists purely to mirror PHP5's invalid arg logic for this function */
#define IMFILT_TYPECHK(n) \
  if (!arg##n.isBoolean() && !arg##n.isNumeric(true)) { \
    raise_warning("imagefilter() expected boolean/numeric for argument %d", \
                  (n+2)); \
    return false; \
  }
  IMFILT_TYPECHK(1)
  IMFILT_TYPECHK(2)
  IMFILT_TYPECHK(3)
  IMFILT_TYPECHK(4)
#undef IMFILT_TYPECHECK

  using image_filter = bool (*)(gdImagePtr, int, int, int, int);
  image_filter filters[] = {
    php_image_filter_negate,
    php_image_filter_grayscale,
    php_image_filter_brightness,
    php_image_filter_contrast,
    php_image_filter_colorize,
    php_image_filter_edgedetect,
    php_image_filter_emboss,
    php_image_filter_gaussian_blur,
    php_image_filter_selective_blur,
    php_image_filter_mean_removal,
    php_image_filter_smooth,
    php_image_filter_pixelate,
  };
  auto const num_filters = sizeof(filters) / sizeof(image_filter);
  if (filtertype >= 0 && filtertype < num_filters) {
    return filters[filtertype](im, arg1.toInt64(), arg2.toInt64(),
                                          arg3.toInt64(), arg4.toInt64());
  }
  return false;
}

bool HHVM_FUNCTION(imageflip, const OptResource& image, int64_t mode /* = -1 */) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  if (mode == -1) mode = GD_FLIP_HORINZONTAL;

  switch (mode) {
    case GD_FLIP_VERTICAL:
      gdImageFlipVertical(im);
      break;

    case GD_FLIP_HORINZONTAL:
      gdImageFlipHorizontal(im);
      break;

    case GD_FLIP_BOTH:
      gdImageFlipBoth(im);
      break;

    default:
      raise_warning("imageflip(): Unknown flip mode");
      return false;
  }

  return true;
}

// gdImageConvolution does not exist in our libgd.a, copied from
// php's libgd/gd.c

/* Filters function added on 2003/12
 * by Pierre-Alain Joye (pajoye@pearfr.org)
 **/

static int hphp_gdImageConvolution(gdImagePtr src, float filter[3][3],
                                   float filter_div, float offset) {
  int x, y, i, j, new_a;
  float new_r, new_g, new_b;
  int new_pxl, pxl=0;
  gdImagePtr srcback;

  if (src==nullptr) {
    return 0;
  }

  /* We need the orinal image with each safe neoghb. pixel */
  srcback = gdImageCreateTrueColor (src->sx, src->sy);
  gdImageCopy(srcback, src,0,0,0,0,src->sx,src->sy);

  if (srcback==nullptr) {
    return 0;
  }

  for ( y=0; y<src->sy; y++) {
    for(x=0; x<src->sx; x++) {
      new_r = new_g = new_b = 0;
      new_a = gdImageAlpha(srcback, pxl);

      for (j=0; j<3; j++) {
        int yv = std::min(std::max(y - 1 + j, 0), src->sy - 1);
        for (i=0; i<3; i++) {
          pxl = gdImageGetPixel(srcback, std::min(std::max(x - 1 + i, 0),
                                                  src->sx - 1), yv);
          new_r += (float)gdImageRed(srcback, pxl) * filter[j][i];
          new_g += (float)gdImageGreen(srcback, pxl) * filter[j][i];
          new_b += (float)gdImageBlue(srcback, pxl) * filter[j][i];
        }
      }

      new_r = (new_r/filter_div)+offset;
      new_g = (new_g/filter_div)+offset;
      new_b = (new_b/filter_div)+offset;

      new_r = (new_r > 255.0f)? 255.0f : ((new_r < 0.0f)? 0.0f:new_r);
      new_g = (new_g > 255.0f)? 255.0f : ((new_g < 0.0f)? 0.0f:new_g);
      new_b = (new_b > 255.0f)? 255.0f : ((new_b < 0.0f)? 0.0f:new_b);

      new_pxl = gdImageColorAllocateAlpha(src, (int)new_r, (int)new_g,
                                          (int)new_b, new_a);
      if (new_pxl == -1) {
        new_pxl = gdImageColorClosestAlpha(src, (int)new_r, (int)new_g,
                                          (int)new_b, new_a);
      }
      gdImageSetPixel (src, x, y, new_pxl);
    }
  }
  gdImageDestroy(srcback);
  return 1;
}

bool HHVM_FUNCTION(imageconvolution, const OptResource& image,
    const Array& matrix, double div, double offset) {
  gdImagePtr im_src = cast<Image>(image)->get();
  if (!im_src) return false;
  int nelem = matrix.size();
  int i, j;
  float mtx[3][3] = {{0,0,0}, {0,0,0}, {0,0,0}};
  Variant v;
  Array row;

  if (nelem != 3) {
    raise_warning("You must have 3x3 array");
    return false;
  }
  for (i=0; i<3; i++) {
    if (matrix.exists(i) && (v = matrix[i]).isArray()) {
      if ((row = v.toArray()).size() != 3) {
        raise_warning("You must have 3x3 array");
        return false;
      }

      for (j=0; j<3; j++) {
        if (row.exists(j)) {
          mtx[i][j] = row[j].toDouble();
        } else {
          raise_warning("You must have a 3x3 matrix");
          return false;
        }
      }
    }
  }
  if (hphp_gdImageConvolution(im_src, mtx, div, offset)) {
    return true;
  } else {
    return false;
  }
}

bool HHVM_FUNCTION(imageantialias, const OptResource& image, bool on) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  SetAntiAliased(im, on);
  return true;
}

Variant HHVM_FUNCTION(imagescale, const OptResource& image, int64_t newwidth,
  int64_t newheight /* =-1 */, int64_t method /*=GD_BILINEAR_FIXED*/) {
  gdImagePtr im = get_valid_image_resource(image);
  if (!im) return false;
  gdImagePtr imscaled = nullptr;
  gdInterpolationMethod old_method;
  if (method == -1) method = GD_BILINEAR_FIXED;

  if (newheight < 0) {
    /* preserve ratio */
    long src_x, src_y;
    src_x = gdImageSX(im);
    src_y = gdImageSY(im);
    if (src_x) {
      newheight = newwidth * src_y / src_x;
    }
  }
  if (newheight <= 0 || newheight > INT_MAX || newwidth <= 0 || newwidth > INT_MAX) {
    return false;
  }

  old_method = im->interpolation_id;
  if (gdImageSetInterpolationMethod(im, (gdInterpolationMethod) method)) {
    imscaled = gdImageScale(im, newwidth, newheight);
  }
  gdImageSetInterpolationMethod(im, old_method);

  if (imscaled == nullptr) {
    return false;
  }
  return Variant(req::make<Image>(imscaled));
}


namespace {

// PHP extension STANDARD: iptc.c
inline int php_iptc_put1(req::ptr<File> /*file*/, int spool, unsigned char c,
                         unsigned char** spoolbuf) {
  if (spool > 0) {
    g_context->write((const char *)&c, 1);
  }

  if (spoolbuf) *(*spoolbuf)++ = c;

  return c;
}

inline int php_iptc_get1(const req::ptr<File>& file,
                         int spool,
                         unsigned char **spoolbuf) {
  int c;
  char cc;

  c = file->getc();

  if (c == EOF) return EOF;

  if (spool > 0) {
    cc = c;
    g_context->write((const char *)&cc, 1);
  }

  if (spoolbuf) *(*spoolbuf)++ = c;

  return c;
}

inline int php_iptc_read_remaining(const req::ptr<File>& file,
                                   int spool,
                                   unsigned char **spoolbuf) {
  while (php_iptc_get1(file, spool, spoolbuf) != EOF) continue;

  return M_EOI;
}

int php_iptc_skip_variable(const req::ptr<File>& file,
                           int spool,
                           unsigned char **spoolbuf) {
  unsigned int length;
  int c1, c2;

  if ((c1 = php_iptc_get1(file, spool, spoolbuf)) == EOF) return M_EOI;

  if ((c2 = php_iptc_get1(file, spool, spoolbuf)) == EOF) return M_EOI;

  length = (((unsigned char) c1) << 8) + ((unsigned char) c2);

  length -= 2;

  while (length--) {
    if (php_iptc_get1(file, spool, spoolbuf) == EOF) return M_EOI;
  }
  return 0;
}

int php_iptc_next_marker(const req::ptr<File>& file,
                         int spool,
                         unsigned char **spoolbuf) {
  int c;

  /* skip unimportant stuff */

  c = php_iptc_get1(file, spool, spoolbuf);

  if (c == EOF) return M_EOI;

  while (c != 0xff) {
    if ((c = php_iptc_get1(file, spool, spoolbuf)) == EOF) {
      return M_EOI; /* we hit EOF */
    }
  }

  /* get marker byte, swallowing possible padding */
  do {
    c = php_iptc_get1(file, 0, 0);
    if (c == EOF)
      return M_EOI;       /* we hit EOF */
    else if (c == 0xff)
      php_iptc_put1(file, spool, (unsigned char)c, spoolbuf);
  } while (c == 0xff);

  return (unsigned int) c;
}

}

const StaticString s_size("size");

Variant HHVM_FUNCTION(iptcembed, const OptString& iptcdata,
    const OptString& jpeg_file_name, int64_t spool /* = 0 */) {
  char psheader[] = "\xFF\xED\0\0Photoshop 3.0\08BIM\x04\x04\0\0\0";
  static_assert(sizeof(psheader) == 28, "psheader must be 28 bytes");
  unsigned int iptcdata_len = iptcdata.length();
  unsigned int marker, inx;
  unsigned char *spoolbuf = nullptr, *poi = nullptr;
  bool done = false;
  bool written = false;

  auto file = File::Open(jpeg_file_name, "rb");
  if (!file) {
    raise_warning("failed to open file: %s", jpeg_file_name.c_str());
    return false;
  }

  if (spool < 2) {
    auto stat = HHVM_FN(fstat)(OptResource(file));
    // TODO(t7561579) until we can properly handle non-file streams here, don't
    // pretend we can and crash.
    if (!stat.isArray()) {
      raise_warning("unable to stat input");
      return false;
    }

    auto& stat_arr = stat.asCArrRef();
    auto st_size = stat_arr[s_size].toInt64();
    if (st_size < 0) {
      raise_warning("unsupported stream type");
      return false;
    }

    if (iptcdata_len >= (INT64_MAX - sizeof(psheader) - st_size - 1024 - 1)) {
      raise_warning("iptcdata too long");
      return false;
    }

    auto malloc_size = iptcdata_len + sizeof(psheader) + st_size + 1024 + 1;
    poi = spoolbuf = (unsigned char *)IM_MALLOC(malloc_size);
    CHECK_ALLOC_R(poi, malloc_size, false);
    memset(poi, 0, malloc_size);
  }
  if (php_iptc_get1(file, spool, poi?&poi:0) != 0xFF) {
    file->close();
    if (spoolbuf) {
      IM_FREE(spoolbuf);
    }
    return false;
  }

  if (php_iptc_get1(file, spool, poi?&poi:0) != 0xD8) {
    file->close();
    if (spoolbuf) {
      IM_FREE(spoolbuf);
    }
    return false;
  }

  while (!done) {
    marker = php_iptc_next_marker(file, spool, poi?&poi:0);
    if (marker == M_EOI) { /* EOF */
      break;
    } else if (marker != M_APP13) {
      php_iptc_put1(file, spool, (unsigned char)marker, poi?&poi:0);
    }
    switch (marker) {
    case M_APP13:
      /* we are going to write a new APP13 marker, so don't
         output the old one */
      php_iptc_skip_variable(file, 0, 0);
      php_iptc_read_remaining(file, spool, poi?&poi:0);
      done = true;
      break;

    case M_APP0:
      /* APP0 is in each and every JPEG, so when we hit APP0
         we insert our new APP13! */
    case M_APP1:
      if (written) {
        /* don't try to write the data twice */
        break;
      }
      written = true;

      php_iptc_skip_variable(file, spool, poi?&poi:0);

      if (iptcdata_len & 1) {
        iptcdata_len++; /* make the length even */
      }

      psheader[2] = (iptcdata_len + sizeof(psheader)) >> 8;
      psheader[3] = (iptcdata_len + sizeof(psheader)) & 0xff;

      for (inx = 0; inx < sizeof(psheader); inx++) {
        php_iptc_put1(file, spool, psheader[inx], poi ? &poi : 0);
      }

      php_iptc_put1(file, spool, (unsigned char)(iptcdata_len>>8),
                    poi?&poi:0);
      php_iptc_put1(file, spool, (unsigned char)(iptcdata_len&0xff),
                    poi?&poi:0);

      for (inx = 0; inx < iptcdata_len; inx++) {
        php_iptc_put1(file, spool, iptcdata.c_str()[inx], poi?&poi:0);
      }
      break;

    case M_SOS:
      /* we hit data, no more marker-inserting can be done! */
      php_iptc_read_remaining(file, spool, poi?&poi:0);
      done = true;
      break;

    default:
      php_iptc_skip_variable(file, spool, poi?&poi:0);
      break;
    }
  }

  file->close();

  if (spool < 2) {
    return OptString((char *)spoolbuf, poi - spoolbuf, AttachString);
  }
  return true;
}

Variant HHVM_FUNCTION(iptcparse, const OptString& iptcblock) {
  unsigned int inx = 0, len, tagsfound = 0;
  unsigned char *buffer, recnum, dataset, key[16];
  unsigned int str_len = iptcblock.length();
  Array ret;

  buffer = (unsigned char *)iptcblock.c_str();
  while (inx < str_len) { /* find 1st tag */
    if ((buffer[inx] == 0x1c) &&
        ((buffer[inx+1] == 0x01) || (buffer[inx+1] == 0x02))) {
      break;
    } else {
      inx++;
    }
  }

  while (inx < str_len) {
    if (buffer[ inx++ ] != 0x1c) {
     /* we ran against some data which does not conform to IPTC -
        stop parsing! */
      break;
    }

    if ((inx + 4) >= str_len)
      break;

    dataset = buffer[inx++];
    recnum = buffer[inx++];

    if (buffer[inx] & (unsigned char) 0x80) { /* long tag */
      if (inx + 6 >= str_len) break;

      len = (((long)buffer[inx + 2]) << 24) +
            (((long)buffer[inx + 3]) << 16) +
            (((long)buffer[inx + 4]) <<  8) +
            (((long)buffer[inx + 5]));
      inx += 6;
    } else { /* short tag */
      len = (((unsigned short)buffer[inx])<<8) |
            (unsigned short)buffer[inx+1];
      inx += 2;
    }

    snprintf((char *)key, sizeof(key), "%d#%03d",
             (unsigned int)dataset, (unsigned int)recnum);

    if ((len > str_len) || (inx + len) > str_len) {
      break;
    }

    OptString skey((const char *)key, CopyString);
    if (!ret.exists(skey)) {
      ret.set(skey, Array::CreateVec());
    }
    auto const lval = ret.lval(skey);
    forceToArray(lval).append(
      OptString((const char *)(buffer+inx), len, CopyString));
    inx += len;
    tagsfound++;
  }

  if (!tagsfound) {
    return false;
  }
  return ret;
}

// PHP extension exif.c
#define NUM_FORMATS 13

#define TAG_FMT_BYTE       1
#define TAG_FMT_STRING     2
#define TAG_FMT_USHORT     3
#define TAG_FMT_ULONG      4
#define TAG_FMT_URATIONAL  5
#define TAG_FMT_SBYTE      6
#define TAG_FMT_UNDEFINED  7
#define TAG_FMT_SSHORT     8
#define TAG_FMT_SLONG      9
#define TAG_FMT_SRATIONAL 10
#define TAG_FMT_SINGLE    11
#define TAG_FMT_DOUBLE    12
#define TAG_FMT_IFD       13

/* Describes tag values */
#define TAG_GPS_VERSION_ID              0x0000
#define TAG_GPS_LATITUDE_REF            0x0001
#define TAG_GPS_LATITUDE                0x0002
#define TAG_GPS_LONGITUDE_REF           0x0003
#define TAG_GPS_LONGITUDE               0x0004
#define TAG_GPS_ALTITUDE_REF            0x0005
#define TAG_GPS_ALTITUDE                0x0006
#define TAG_GPS_TIME_STAMP              0x0007
#define TAG_GPS_SATELLITES              0x0008
#define TAG_GPS_STATUS                  0x0009
#define TAG_GPS_MEASURE_MODE            0x000A
#define TAG_GPS_DOP                     0x000B
#define TAG_GPS_SPEED_REF               0x000C
#define TAG_GPS_SPEED                   0x000D
#define TAG_GPS_TRACK_REF               0x000E
#define TAG_GPS_TRACK                   0x000F
#define TAG_GPS_IMG_DIRECTION_REF       0x0010
#define TAG_GPS_IMG_DIRECTION           0x0011
#define TAG_GPS_MAP_DATUM               0x0012
#define TAG_GPS_DEST_LATITUDE_REF       0x0013
#define TAG_GPS_DEST_LATITUDE           0x0014
#define TAG_GPS_DEST_LONGITUDE_REF      0x0015
#define TAG_GPS_DEST_LONGITUDE          0x0016
#define TAG_GPS_DEST_BEARING_REF        0x0017
#define TAG_GPS_DEST_BEARING            0x0018
#define TAG_GPS_DEST_DISTANCE_REF       0x0019
#define TAG_GPS_DEST_DISTANCE           0x001A
#define TAG_GPS_PROCESSING_METHOD       0x001B
#define TAG_GPS_AREA_INFORMATION        0x001C
#define TAG_GPS_DATE_STAMP              0x001D
#define TAG_GPS_DIFFERENTIAL            0x001E
#define TAG_TIFF_COMMENT                0x00FE /* SHOUDLNT HAPPEN */
#define TAG_NEW_SUBFILE                 0x00FE /* New version of subfile tag */
#define TAG_SUBFILE_TYPE                0x00FF /* Old version of subfile tag */
#define TAG_IMAGEWIDTH                  0x0100
#define TAG_IMAGEHEIGHT                 0x0101
#define TAG_BITS_PER_SAMPLE             0x0102
#define TAG_COMPRESSION                 0x0103
#define TAG_PHOTOMETRIC_INTERPRETATION  0x0106
#define TAG_TRESHHOLDING                0x0107
#define TAG_CELL_WIDTH                  0x0108
#define TAG_CELL_HEIGHT                 0x0109
#define TAG_FILL_ORDER                  0x010A
#define TAG_DOCUMENT_NAME               0x010D
#define TAG_IMAGE_DESCRIPTION           0x010E
#define TAG_MAKE                        0x010F
#define TAG_MODEL                       0x0110
#define TAG_STRIP_OFFSETS               0x0111
#define TAG_ORIENTATION                 0x0112
#define TAG_SAMPLES_PER_PIXEL           0x0115
#define TAG_ROWS_PER_STRIP              0x0116
#define TAG_STRIP_BYTE_COUNTS           0x0117
#define TAG_MIN_SAMPPLE_VALUE           0x0118
#define TAG_MAX_SAMPLE_VALUE            0x0119
#define TAG_X_RESOLUTION                0x011A
#define TAG_Y_RESOLUTION                0x011B
#define TAG_PLANAR_CONFIGURATION        0x011C
#define TAG_PAGE_NAME                   0x011D
#define TAG_X_POSITION                  0x011E
#define TAG_Y_POSITION                  0x011F
#define TAG_FREE_OFFSETS                0x0120
#define TAG_FREE_BYTE_COUNTS            0x0121
#define TAG_GRAY_RESPONSE_UNIT          0x0122
#define TAG_GRAY_RESPONSE_CURVE         0x0123
#define TAG_RESOLUTION_UNIT             0x0128
#define TAG_PAGE_NUMBER                 0x0129
#define TAG_TRANSFER_FUNCTION           0x012D
#define TAG_SOFTWARE                    0x0131
#define TAG_DATETIME                    0x0132
#define TAG_ARTIST                      0x013B
#define TAG_HOST_COMPUTER               0x013C
#define TAG_PREDICTOR                   0x013D
#define TAG_WHITE_POINT                 0x013E
#define TAG_PRIMARY_CHROMATICITIES      0x013F
#define TAG_COLOR_MAP                   0x0140
#define TAG_HALFTONE_HINTS              0x0141
#define TAG_TILE_WIDTH                  0x0142
#define TAG_TILE_LENGTH                 0x0143
#define TAG_TILE_OFFSETS                0x0144
#define TAG_TILE_BYTE_COUNTS            0x0145
#define TAG_SUB_IFD                     0x014A
#define TAG_INK_SETMPUTER               0x014C
#define TAG_INK_NAMES                   0x014D
#define TAG_NUMBER_OF_INKS              0x014E
#define TAG_DOT_RANGE                   0x0150
#define TAG_TARGET_PRINTER              0x0151
#define TAG_EXTRA_SAMPLE                0x0152
#define TAG_SAMPLE_FORMAT               0x0153
#define TAG_S_MIN_SAMPLE_VALUE          0x0154
#define TAG_S_MAX_SAMPLE_VALUE          0x0155
#define TAG_TRANSFER_RANGE              0x0156
#define TAG_JPEG_TABLES                 0x015B
#define TAG_JPEG_PROC                   0x0200
#define TAG_JPEG_INTERCHANGE_FORMAT     0x0201
#define TAG_JPEG_INTERCHANGE_FORMAT_LEN 0x0202
#define TAG_JPEG_RESTART_INTERVAL       0x0203
#define TAG_JPEG_LOSSLESS_PREDICTOR     0x0205
#define TAG_JPEG_POINT_TRANSFORMS       0x0206
#define TAG_JPEG_Q_TABLES               0x0207
#define TAG_JPEG_DC_TABLES              0x0208
#define TAG_JPEG_AC_TABLES              0x0209
#define TAG_YCC_COEFFICIENTS            0x0211
#define TAG_YCC_SUB_SAMPLING            0x0212
#define TAG_YCC_POSITIONING             0x0213
#define TAG_REFERENCE_BLACK_WHITE       0x0214
/* 0x0301 - 0x0302 */
/* 0x0320 */
/* 0x0343 */
/* 0x5001 - 0x501B */
/* 0x5021 - 0x503B */
/* 0x5090 - 0x5091 */
/* 0x5100 - 0x5101 */
/* 0x5110 - 0x5113 */
/* 0x80E3 - 0x80E6 */
/* 0x828d - 0x828F */
#define TAG_COPYRIGHT                   0x8298
#define TAG_EXPOSURETIME                0x829A
#define TAG_FNUMBER                     0x829D
#define TAG_EXIF_IFD_POINTER            0x8769
#define TAG_ICC_PROFILE                 0x8773
#define TAG_EXPOSURE_PROGRAM            0x8822
#define TAG_SPECTRAL_SENSITY            0x8824
#define TAG_GPS_IFD_POINTER             0x8825
#define TAG_ISOSPEED                    0x8827
#define TAG_OPTOELECTRIC_CONVERSION_F   0x8828
/* 0x8829 - 0x882b */
#define TAG_EXIFVERSION                 0x9000
#define TAG_DATE_TIME_ORIGINAL          0x9003
#define TAG_DATE_TIME_DIGITIZED         0x9004
#define TAG_COMPONENT_CONFIG            0x9101
#define TAG_COMPRESSED_BITS_PER_PIXEL   0x9102
#define TAG_SHUTTERSPEED                0x9201
#define TAG_APERTURE                    0x9202
#define TAG_BRIGHTNESS_VALUE            0x9203
#define TAG_EXPOSURE_BIAS_VALUE         0x9204
#define TAG_MAX_APERTURE                0x9205
#define TAG_SUBJECT_DISTANCE            0x9206
#define TAG_METRIC_MODULE               0x9207
#define TAG_LIGHT_SOURCE                0x9208
#define TAG_FLASH                       0x9209
#define TAG_FOCAL_LENGTH                0x920A
/* 0x920B - 0x920D */
/* 0x9211 - 0x9216 */
#define TAG_SUBJECT_AREA                0x9214
#define TAG_MAKER_NOTE                  0x927C
#define TAG_USERCOMMENT                 0x9286
#define TAG_SUB_SEC_TIME                0x9290
#define TAG_SUB_SEC_TIME_ORIGINAL       0x9291
#define TAG_SUB_SEC_TIME_DIGITIZED      0x9292
/* 0x923F */
/* 0x935C */
#define TAG_XP_TITLE                    0x9C9B
#define TAG_XP_COMMENTS                 0x9C9C
#define TAG_XP_AUTHOR                   0x9C9D
#define TAG_XP_KEYWORDS                 0x9C9E
#define TAG_XP_SUBJECT                  0x9C9F
#define TAG_FLASH_PIX_VERSION           0xA000
#define TAG_COLOR_SPACE                 0xA001
#define TAG_COMP_IMAGE_WIDTH            0xA002 /* compressed images only */
#define TAG_COMP_IMAGE_HEIGHT           0xA003
#define TAG_RELATED_SOUND_FILE          0xA004
#define TAG_INTEROP_IFD_POINTER         0xA005 /* IFD pointer */
#define TAG_FLASH_ENERGY                0xA20B
#define TAG_SPATIAL_FREQUENCY_RESPONSE  0xA20C
#define TAG_FOCALPLANE_X_RES            0xA20E
#define TAG_FOCALPLANE_Y_RES            0xA20F
#define TAG_FOCALPLANE_RESOLUTION_UNIT  0xA210
#define TAG_SUBJECT_LOCATION            0xA214
#define TAG_EXPOSURE_INDEX              0xA215
#define TAG_SENSING_METHOD              0xA217
#define TAG_FILE_SOURCE                 0xA300
#define TAG_SCENE_TYPE                  0xA301
#define TAG_CFA_PATTERN                 0xA302
#define TAG_CUSTOM_RENDERED             0xA401
#define TAG_EXPOSURE_MODE               0xA402
#define TAG_WHITE_BALANCE               0xA403
#define TAG_DIGITAL_ZOOM_RATIO          0xA404
#define TAG_FOCAL_LENGTH_IN_35_MM_FILM  0xA405
#define TAG_SCENE_CAPTURE_TYPE          0xA406
#define TAG_GAIN_CONTROL                0xA407
#define TAG_CONTRAST                    0xA408
#define TAG_SATURATION                  0xA409
#define TAG_SHARPNESS                   0xA40A
#define TAG_DEVICE_SETTING_DESCRIPTION  0xA40B
#define TAG_SUBJECT_DISTANCE_RANGE      0xA40C
#define TAG_IMAGE_UNIQUE_ID             0xA420

/* Olympus specific tags */
#define TAG_OLYMPUS_SPECIALMODE         0x0200
#define TAG_OLYMPUS_JPEGQUAL            0x0201
#define TAG_OLYMPUS_MACRO               0x0202
#define TAG_OLYMPUS_DIGIZOOM            0x0204
#define TAG_OLYMPUS_SOFTWARERELEASE     0x0207
#define TAG_OLYMPUS_PICTINFO            0x0208
#define TAG_OLYMPUS_CAMERAID            0x0209
/* end Olympus specific tags */

/* Internal */
#define TAG_NONE                        -1 /* note that -1 <> 0xFFFF */
#define TAG_COMPUTED_VALUE              -2
#define TAG_END_OF_LIST                 0xFFFD

/* Values for TAG_PHOTOMETRIC_INTERPRETATION */
#define PMI_BLACK_IS_ZERO       0
#define PMI_WHITE_IS_ZERO       1
#define PMI_RGB                 2
#define PMI_PALETTE_COLOR       3
#define PMI_TRANSPARENCY_MASK   4
#define PMI_SEPARATED           5
#define PMI_YCBCR               6
#define PMI_CIELAB              8

struct _tag_info_type {
  unsigned short Tag;
  const char *Desc;
};
using tag_info_type = const struct _tag_info_type;

using tag_info_array = tag_info_type[];
using tag_table_type = tag_info_type *;

#define TAG_TABLE_END \
  {((unsigned short)TAG_NONE),           "No tag value"},\
  {((unsigned short)TAG_COMPUTED_VALUE), "Computed value"},\
  {TAG_END_OF_LIST,    ""}  /* Important for exif_get_tagname()
                               IF value != "" function result is != false */

static const tag_info_array tag_table_IFD = {
  { 0x000B, "ACDComment"},
  { 0x00FE, "NewSubFile"}, /* better name it 'ImageType' ? */
  { 0x00FF, "SubFile"},
  { 0x0100, "ImageWidth"},
  { 0x0101, "ImageLength"},
  { 0x0102, "BitsPerSample"},
  { 0x0103, "Compression"},
  { 0x0106, "PhotometricInterpretation"},
  { 0x010A, "FillOrder"},
  { 0x010D, "DocumentName"},
  { 0x010E, "ImageDescription"},
  { 0x010F, "Make"},
  { 0x0110, "Model"},
  { 0x0111, "StripOffsets"},
  { 0x0112, "Orientation"},
  { 0x0115, "SamplesPerPixel"},
  { 0x0116, "RowsPerStrip"},
  { 0x0117, "StripByteCounts"},
  { 0x0118, "MinSampleValue"},
  { 0x0119, "MaxSampleValue"},
  { 0x011A, "XResolution"},
  { 0x011B, "YResolution"},
  { 0x011C, "PlanarConfiguration"},
  { 0x011D, "PageName"},
  { 0x011E, "XPosition"},
  { 0x011F, "YPosition"},
  { 0x0120, "FreeOffsets"},
  { 0x0121, "FreeByteCounts"},
  { 0x0122, "GrayResponseUnit"},
  { 0x0123, "GrayResponseCurve"},
  { 0x0124, "T4Options"},
  { 0x0125, "T6Options"},
  { 0x0128, "ResolutionUnit"},
  { 0x0129, "PageNumber"},
  { 0x012D, "TransferFunction"},
  { 0x0131, "Software"},
  { 0x0132, "DateTime"},
  { 0x013B, "Artist"},
  { 0x013C, "HostComputer"},
  { 0x013D, "Predictor"},
  { 0x013E, "WhitePoint"},
  { 0x013F, "PrimaryChromaticities"},
  { 0x0140, "ColorMap"},
  { 0x0141, "HalfToneHints"},
  { 0x0142, "TileWidth"},
  { 0x0143, "TileLength"},
  { 0x0144, "TileOffsets"},
  { 0x0145, "TileByteCounts"},
  { 0x014A, "SubIFD"},
  { 0x014C, "InkSet"},
  { 0x014D, "InkNames"},
  { 0x014E, "NumberOfInks"},
  { 0x0150, "DotRange"},
  { 0x0151, "TargetPrinter"},
  { 0x0152, "ExtraSample"},
  { 0x0153, "SampleFormat"},
  { 0x0154, "SMinSampleValue"},
  { 0x0155, "SMaxSampleValue"},
  { 0x0156, "TransferRange"},
  { 0x0157, "ClipPath"},
  { 0x0158, "XClipPathUnits"},
  { 0x0159, "YClipPathUnits"},
  { 0x015A, "Indexed"},
  { 0x015B, "JPEGTables"},
  { 0x015F, "OPIProxy"},
  { 0x0200, "JPEGProc"},
  { 0x0201, "JPEGInterchangeFormat"},
  { 0x0202, "JPEGInterchangeFormatLength"},
  { 0x0203, "JPEGRestartInterval"},
  { 0x0205, "JPEGLosslessPredictors"},
  { 0x0206, "JPEGPointTransforms"},
  { 0x0207, "JPEGQTables"},
  { 0x0208, "JPEGDCTables"},
  { 0x0209, "JPEGACTables"},
  { 0x0211, "YCbCrCoefficients"},
  { 0x0212, "YCbCrSubSampling"},
  { 0x0213, "YCbCrPositioning"},
  { 0x0214, "ReferenceBlackWhite"},
  { 0x02BC, "ExtensibleMetadataPlatform"},
  /* XAP: Extensible Authoring Publishing, obsoleted by XMP:
     Extensible Metadata Platform */
  { 0x0301, "Gamma"},
  { 0x0302, "ICCProfileDescriptor"},
  { 0x0303, "SRGBRenderingIntent"},
  { 0x0320, "ImageTitle"},
  { 0x5001, "ResolutionXUnit"},
  { 0x5002, "ResolutionYUnit"},
  { 0x5003, "ResolutionXLengthUnit"},
  { 0x5004, "ResolutionYLengthUnit"},
  { 0x5005, "PrintFlags"},
  { 0x5006, "PrintFlagsVersion"},
  { 0x5007, "PrintFlagsCrop"},
  { 0x5008, "PrintFlagsBleedWidth"},
  { 0x5009, "PrintFlagsBleedWidthScale"},
  { 0x500A, "HalftoneLPI"},
  { 0x500B, "HalftoneLPIUnit"},
  { 0x500C, "HalftoneDegree"},
  { 0x500D, "HalftoneShape"},
  { 0x500E, "HalftoneMisc"},
  { 0x500F, "HalftoneScreen"},
  { 0x5010, "JPEGQuality"},
  { 0x5011, "GridSize"},
  { 0x5012, "ThumbnailFormat"},
  { 0x5013, "ThumbnailWidth"},
  { 0x5014, "ThumbnailHeight"},
  { 0x5015, "ThumbnailColorDepth"},
  { 0x5016, "ThumbnailPlanes"},
  { 0x5017, "ThumbnailRawBytes"},
  { 0x5018, "ThumbnailSize"},
  { 0x5019, "ThumbnailCompressedSize"},
  { 0x501A, "ColorTransferFunction"},
  { 0x501B, "ThumbnailData"},
  { 0x5020, "ThumbnailImageWidth"},
  { 0x5021, "ThumbnailImageHeight"},
  { 0x5022, "ThumbnailBitsPerSample"},
  { 0x5023, "ThumbnailCompression"},
  { 0x5024, "ThumbnailPhotometricInterp"},
  { 0x5025, "ThumbnailImageDescription"},
  { 0x5026, "ThumbnailEquipMake"},
  { 0x5027, "ThumbnailEquipModel"},
  { 0x5028, "ThumbnailStripOffsets"},
  { 0x5029, "ThumbnailOrientation"},
  { 0x502A, "ThumbnailSamplesPerPixel"},
  { 0x502B, "ThumbnailRowsPerStrip"},
  { 0x502C, "ThumbnailStripBytesCount"},
  { 0x502D, "ThumbnailResolutionX"},
  { 0x502E, "ThumbnailResolutionY"},
  { 0x502F, "ThumbnailPlanarConfig"},
  { 0x5030, "ThumbnailResolutionUnit"},
  { 0x5031, "ThumbnailTransferFunction"},
  { 0x5032, "ThumbnailSoftwareUsed"},
  { 0x5033, "ThumbnailDateTime"},
  { 0x5034, "ThumbnailArtist"},
  { 0x5035, "ThumbnailWhitePoint"},
  { 0x5036, "ThumbnailPrimaryChromaticities"},
  { 0x5037, "ThumbnailYCbCrCoefficients"},
  { 0x5038, "ThumbnailYCbCrSubsampling"},
  { 0x5039, "ThumbnailYCbCrPositioning"},
  { 0x503A, "ThumbnailRefBlackWhite"},
  { 0x503B, "ThumbnailCopyRight"},
  { 0x5090, "LuminanceTable"},
  { 0x5091, "ChrominanceTable"},
  { 0x5100, "FrameDelay"},
  { 0x5101, "LoopCount"},
  { 0x5110, "PixelUnit"},
  { 0x5111, "PixelPerUnitX"},
  { 0x5112, "PixelPerUnitY"},
  { 0x5113, "PaletteHistogram"},
  { 0x1000, "RelatedImageFileFormat"},
  { 0x800D, "ImageID"},
  { 0x80E3, "Matteing"},   /* obsoleted by ExtraSamples */
  { 0x80E4, "DataType"},   /* obsoleted by SampleFormat */
  { 0x80E5, "ImageDepth"},
  { 0x80E6, "TileDepth"},
  { 0x828D, "CFARepeatPatternDim"},
  { 0x828E, "CFAPattern"},
  { 0x828F, "BatteryLevel"},
  { 0x8298, "Copyright"},
  { 0x829A, "ExposureTime"},
  { 0x829D, "FNumber"},
  { 0x83BB, "IPTC/NAA"},
  { 0x84E3, "IT8RasterPadding"},
  { 0x84E5, "IT8ColorTable"},
  { 0x8649, "ImageResourceInformation"}, /* PhotoShop */
  { 0x8769, "Exif_IFD_Pointer"},
  { 0x8773, "ICC_Profile"},
  { 0x8822, "ExposureProgram"},
  { 0x8824, "SpectralSensity"},
  { 0x8828, "OECF"},
  { 0x8825, "GPS_IFD_Pointer"},
  { 0x8827, "ISOSpeedRatings"},
  { 0x8828, "OECF"},
  { 0x9000, "ExifVersion"},
  { 0x9003, "DateTimeOriginal"},
  { 0x9004, "DateTimeDigitized"},
  { 0x9101, "ComponentsConfiguration"},
  { 0x9102, "CompressedBitsPerPixel"},
  { 0x9201, "ShutterSpeedValue"},
  { 0x9202, "ApertureValue"},
  { 0x9203, "BrightnessValue"},
  { 0x9204, "ExposureBiasValue"},
  { 0x9205, "MaxApertureValue"},
  { 0x9206, "SubjectDistance"},
  { 0x9207, "MeteringMode"},
  { 0x9208, "LightSource"},
  { 0x9209, "Flash"},
  { 0x920A, "FocalLength"},
  { 0x920B, "FlashEnergy"},                 /* 0xA20B  in JPEG   */
  { 0x920C, "SpatialFrequencyResponse"},    /* 0xA20C    -  -    */
  { 0x920D, "Noise"},
  { 0x920E, "FocalPlaneXResolution"},       /* 0xA20E    -  -    */
  { 0x920F, "FocalPlaneYResolution"},       /* 0xA20F    -  -    */
  { 0x9210, "FocalPlaneResolutionUnit"},    /* 0xA210    -  -    */
  { 0x9211, "ImageNumber"},
  { 0x9212, "SecurityClassification"},
  { 0x9213, "ImageHistory"},
  { 0x9214, "SubjectLocation"},             /* 0xA214    -  -    */
  { 0x9215, "ExposureIndex"},               /* 0xA215    -  -    */
  { 0x9216, "TIFF/EPStandardID"},
  { 0x9217, "SensingMethod"},               /* 0xA217    -  -    */
  { 0x923F, "StoNits"},
  { 0x927C, "MakerNote"},
  { 0x9286, "UserComment"},
  { 0x9290, "SubSecTime"},
  { 0x9291, "SubSecTimeOriginal"},
  { 0x9292, "SubSecTimeDigitized"},
  { 0x935C, "ImageSourceData"},
    /* "Adobe Photoshop Document Data Block": 8BIM... */
  { 0x9c9b, "Title" },          /* Win XP specific, Unicode  */
  { 0x9c9c, "Comments" },       /* Win XP specific, Unicode  */
  { 0x9c9d, "Author" },         /* Win XP specific, Unicode  */
  { 0x9c9e, "Keywords" },       /* Win XP specific, Unicode  */
  { 0x9c9f, "Subject" },        /* Win XP specific, Unicode,
                                   not to be confused with SubjectDistance
                                   and SubjectLocation */
  { 0xA000, "FlashPixVersion"},
  { 0xA001, "ColorSpace"},
  { 0xA002, "ExifImageWidth"},
  { 0xA003, "ExifImageLength"},
  { 0xA004, "RelatedSoundFile"},
  { 0xA005, "InteroperabilityOffset"},
  { 0xA20B, "FlashEnergy"},                 /* 0x920B in TIFF/EP */
  { 0xA20C, "SpatialFrequencyResponse"},    /* 0x920C    -  -    */
  { 0xA20D, "Noise"},
  { 0xA20E, "FocalPlaneXResolution"},       /* 0x920E    -  -    */
  { 0xA20F, "FocalPlaneYResolution"},       /* 0x920F    -  -    */
  { 0xA210, "FocalPlaneResolutionUnit"},    /* 0x9210    -  -    */
  { 0xA211, "ImageNumber"},
  { 0xA212, "SecurityClassification"},
  { 0xA213, "ImageHistory"},
  { 0xA214, "SubjectLocation"},             /* 0x9214    -  -    */
  { 0xA215, "ExposureIndex"},               /* 0x9215    -  -    */
  { 0xA216, "TIFF/EPStandardID"},
  { 0xA217, "SensingMethod"},               /* 0x9217    -  -    */
  { 0xA300, "FileSource"},
  { 0xA301, "SceneType"},
  { 0xA302, "CFAPattern"},
  { 0xA401, "CustomRendered"},
  { 0xA402, "ExposureMode"},
  { 0xA403, "WhiteBalance"},
  { 0xA404, "DigitalZoomRatio"},
  { 0xA405, "FocalLengthIn35mmFilm"},
  { 0xA406, "SceneCaptureType"},
  { 0xA407, "GainControl"},
  { 0xA408, "Contrast"},
  { 0xA409, "Saturation"},
  { 0xA40A, "Sharpness"},
  { 0xA40B, "DeviceSettingDescription"},
  { 0xA40C, "SubjectDistanceRange"},
  { 0xA420, "ImageUniqueID"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_GPS = {
  { 0x0000, "GPSVersion"},
  { 0x0001, "GPSLatitudeRef"},
  { 0x0002, "GPSLatitude"},
  { 0x0003, "GPSLongitudeRef"},
  { 0x0004, "GPSLongitude"},
  { 0x0005, "GPSAltitudeRef"},
  { 0x0006, "GPSAltitude"},
  { 0x0007, "GPSTimeStamp"},
  { 0x0008, "GPSSatellites"},
  { 0x0009, "GPSStatus"},
  { 0x000A, "GPSMeasureMode"},
  { 0x000B, "GPSDOP"},
  { 0x000C, "GPSSpeedRef"},
  { 0x000D, "GPSSpeed"},
  { 0x000E, "GPSTrackRef"},
  { 0x000F, "GPSTrack"},
  { 0x0010, "GPSImgDirectionRef"},
  { 0x0011, "GPSImgDirection"},
  { 0x0012, "GPSMapDatum"},
  { 0x0013, "GPSDestLatitudeRef"},
  { 0x0014, "GPSDestLatitude"},
  { 0x0015, "GPSDestLongitudeRef"},
  { 0x0016, "GPSDestLongitude"},
  { 0x0017, "GPSDestBearingRef"},
  { 0x0018, "GPSDestBearing"},
  { 0x0019, "GPSDestDistanceRef"},
  { 0x001A, "GPSDestDistance"},
  { 0x001B, "GPSProcessingMode"},
  { 0x001C, "GPSAreaInformation"},
  { 0x001D, "GPSDateStamp"},
  { 0x001E, "GPSDifferential"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_IOP = {
  { 0x0001, "InterOperabilityIndex"}, /* should be 'R98' or 'THM' */
  { 0x0002, "InterOperabilityVersion"},
  { 0x1000, "RelatedFileFormat"},
  { 0x1001, "RelatedImageWidth"},
  { 0x1002, "RelatedImageHeight"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_VND_CANON = {
  { 0x0001, "ModeArray"}, /* guess */
  { 0x0004, "ImageInfo"}, /* guess */
  { 0x0006, "ImageType"},
  { 0x0007, "FirmwareVersion"},
  { 0x0008, "ImageNumber"},
  { 0x0009, "OwnerName"},
  { 0x000C, "Camera"},
  { 0x000F, "CustomFunctions"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_VND_CASIO = {
  { 0x0001, "RecordingMode"},
  { 0x0002, "Quality"},
  { 0x0003, "FocusingMode"},
  { 0x0004, "FlashMode"},
  { 0x0005, "FlashIntensity"},
  { 0x0006, "ObjectDistance"},
  { 0x0007, "WhiteBalance"},
  { 0x000A, "DigitalZoom"},
  { 0x000B, "Sharpness"},
  { 0x000C, "Contrast"},
  { 0x000D, "Saturation"},
  { 0x0014, "CCDSensitivity"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_VND_FUJI = {
  { 0x0000, "Version"},
  { 0x1000, "Quality"},
  { 0x1001, "Sharpness"},
  { 0x1002, "WhiteBalance"},
  { 0x1003, "Color"},
  { 0x1004, "Tone"},
  { 0x1010, "FlashMode"},
  { 0x1011, "FlashStrength"},
  { 0x1020, "Macro"},
  { 0x1021, "FocusMode"},
  { 0x1030, "SlowSync"},
  { 0x1031, "PictureMode"},
  { 0x1100, "ContTake"},
  { 0x1300, "BlurWarning"},
  { 0x1301, "FocusWarning"},
  { 0x1302, "AEWarning "},
  TAG_TABLE_END
};

static const tag_info_array tag_table_VND_NIKON = {
  { 0x0003, "Quality"},
  { 0x0004, "ColorMode"},
  { 0x0005, "ImageAdjustment"},
  { 0x0006, "CCDSensitivity"},
  { 0x0007, "WhiteBalance"},
  { 0x0008, "Focus"},
  { 0x000a, "DigitalZoom"},
  { 0x000b, "Converter"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_VND_NIKON_990 = {
  { 0x0001, "Version"},
  { 0x0002, "ISOSetting"},
  { 0x0003, "ColorMode"},
  { 0x0004, "Quality"},
  { 0x0005, "WhiteBalance"},
  { 0x0006, "ImageSharpening"},
  { 0x0007, "FocusMode"},
  { 0x0008, "FlashSetting"},
  { 0x000F, "ISOSelection"},
  { 0x0080, "ImageAdjustment"},
  { 0x0082, "AuxiliaryLens"},
  { 0x0085, "ManualFocusDistance"},
  { 0x0086, "DigitalZoom"},
  { 0x0088, "AFFocusPosition"},
  { 0x0010, "DataDump"},
  TAG_TABLE_END
};

static const tag_info_array tag_table_VND_OLYMPUS = {
  { 0x0200, "SpecialMode"},
  { 0x0201, "JPEGQuality"},
  { 0x0202, "Macro"},
  { 0x0204, "DigitalZoom"},
  { 0x0207, "SoftwareRelease"},
  { 0x0208, "PictureInfo"},
  { 0x0209, "CameraId"},
  { 0x0F00, "DataDump"},
  TAG_TABLE_END
};

enum mn_byte_order_t {
  MN_ORDER_INTEL = 0,
  MN_ORDER_MOTOROLA = 1,
  MN_ORDER_NORMAL
};

enum mn_offset_mode_t {
  MN_OFFSET_NORMAL,
  MN_OFFSET_MAKER,
  MN_OFFSET_GUESS
};

struct maker_note_type {
  tag_table_type tag_table;
  const char *make;
  const char *model;
  const char *id_string;
  int id_string_len;
  int offset;
  mn_byte_order_t byte_order;
  mn_offset_mode_t offset_mode;
};

static const maker_note_type maker_note_array[] = {
  { tag_table_VND_CANON, "Canon", nullptr, nullptr,
    0, 0, MN_ORDER_INTEL, MN_OFFSET_NORMAL},
/*  { tag_table_VND_CANON, "Canon", nullptr, nullptr,
      0,  0,  MN_ORDER_NORMAL,   MN_OFFSET_NORMAL},*/
  { tag_table_VND_CASIO, "CASIO", nullptr, nullptr,
    0, 0, MN_ORDER_MOTOROLA, MN_OFFSET_NORMAL},
  { tag_table_VND_FUJI, "FUJIFILM", nullptr, "FUJIFILM\x0C\x00\x00\x00",
    12, 12, MN_ORDER_INTEL, MN_OFFSET_MAKER},
  { tag_table_VND_NIKON, "NIKON", nullptr, "Nikon\x00\x01\x00",
    8, 8, MN_ORDER_NORMAL, MN_OFFSET_NORMAL},
  { tag_table_VND_NIKON_990, "NIKON", nullptr, nullptr,
    0, 0, MN_ORDER_NORMAL, MN_OFFSET_NORMAL},
  { tag_table_VND_OLYMPUS, "OLYMPUS OPTICAL CO.,LTD",
    nullptr, "OLYMP\x00\x01\x00", 8, 8, MN_ORDER_NORMAL, MN_OFFSET_NORMAL},
};

/* Get headername for tag_num or nullptr if not defined */
static const char * exif_get_tagname(int tag_num, char *ret, int len,
                               tag_table_type tag_table) {
  int i, t;
  char tmp[32];

  for (i = 0; (t = tag_table[i].Tag) != TAG_END_OF_LIST; i++) {
    if (t == tag_num) {
      if (ret && len)  {
        string_copy(ret, tag_table[i].Desc, abs(len));
        if (len < 0) {
          memset(ret + strlen(ret), ' ', -len - strlen(ret) - 1);
          ret[-len - 1] = '\0';
        }
        return ret;
      }
      return tag_table[i].Desc;
    }
  }

  if (ret && len) {
    snprintf(tmp, sizeof(tmp), "UndefinedTag:0x%04X", tag_num);
    string_copy(ret, tmp, abs(len));
    if (len < 0) {
      memset(ret + strlen(ret), ' ', -len - strlen(ret) - 1);
      ret[-len - 1] = '\0';
    }
    return ret;
  }
  return "";
}

#define MAX_IFD_NESTING_LEVEL 100

#ifndef WORD
#define WORD unsigned short
#endif
#ifndef DWORD
#define DWORD unsigned int
#endif

struct signed_rational {
  int num;
  int den;
};

struct unsigned_rational {
  unsigned int num;
  unsigned int den;
};

union image_info_value {
  char *s;
  unsigned u;
  int i;
  float f;
  double d;
  signed_rational sr;
  unsigned_rational ur;
  union image_info_value *list;
};

struct image_info_data {
  WORD tag;
  WORD format;
  DWORD length;
  DWORD dummy;  /* value ptr of tiff directory entry */
  char *name;
  image_info_value value;
};

struct image_info_list {
  int count;
  image_info_data *list;
};

#define SECTION_FILE        0
#define SECTION_COMPUTED    1
#define SECTION_ANY_TAG     2
#define SECTION_IFD0        3
#define SECTION_THUMBNAIL   4
#define SECTION_COMMENT     5
#define SECTION_APP0        6
#define SECTION_EXIF        7
#define SECTION_FPIX        8
#define SECTION_GPS         9
#define SECTION_INTEROP     10
#define SECTION_APP12       11
#define SECTION_WINXP       12
#define SECTION_MAKERNOTE   13
#define SECTION_COUNT       14

#define FOUND_FILE          (1<<SECTION_FILE)
#define FOUND_COMPUTED      (1<<SECTION_COMPUTED)
#define FOUND_ANY_TAG       (1<<SECTION_ANY_TAG)
#define FOUND_IFD0          (1<<SECTION_IFD0)
#define FOUND_THUMBNAIL     (1<<SECTION_THUMBNAIL)
#define FOUND_COMMENT       (1<<SECTION_COMMENT)
#define FOUND_APP0          (1<<SECTION_APP0)
#define FOUND_EXIF          (1<<SECTION_EXIF)
#define FOUND_FPIX          (1<<SECTION_FPIX)
#define FOUND_GPS           (1<<SECTION_GPS)
#define FOUND_INTEROP       (1<<SECTION_INTEROP)
#define FOUND_APP12         (1<<SECTION_APP12)
#define FOUND_WINXP         (1<<SECTION_WINXP)
#define FOUND_MAKERNOTE     (1<<SECTION_MAKERNOTE)

const StaticString
  s_FILE("FILE"),
  s_COMPUTED("COMPUTED"),
  s_ANY_TAG("ANY_TAG"),
  s_IFD0("IFD0"),
  s_THUMBNAIL("THUMBNAIL"),
  s_COMMENT("COMMENT"),
  s_APP0("APP0"),
  s_EXIF("EXIF"),
  s_FPIX("FPIX"),
  s_GPS("GPS"),
  s_INTEROP("INTEROP"),
  s_APP12("APP12"),
  s_WINXP("WINXP"),
  s_MAKERNOTE("MAKERNOTE");

static OptString exif_get_sectionname(int section) {
  switch(section) {
  case SECTION_FILE:      return s_FILE;
  case SECTION_COMPUTED:  return s_COMPUTED;
  case SECTION_ANY_TAG:   return s_ANY_TAG;
  case SECTION_IFD0:      return s_IFD0;
  case SECTION_THUMBNAIL: return s_THUMBNAIL;
  case SECTION_COMMENT:   return s_COMMENT;
  case SECTION_APP0:      return s_APP0;
  case SECTION_EXIF:      return s_EXIF;
  case SECTION_FPIX:      return s_FPIX;
  case SECTION_GPS:       return s_GPS;
  case SECTION_INTEROP:   return s_INTEROP;
  case SECTION_APP12:     return s_APP12;
  case SECTION_WINXP:     return s_WINXP;
  case SECTION_MAKERNOTE: return s_MAKERNOTE;
  }
  return String::Empty();
}

static tag_table_type exif_get_tag_table(int section) {
  switch(section) {
  case SECTION_FILE:      return &tag_table_IFD[0];
  case SECTION_COMPUTED:  return &tag_table_IFD[0];
  case SECTION_ANY_TAG:   return &tag_table_IFD[0];
  case SECTION_IFD0:      return &tag_table_IFD[0];
  case SECTION_THUMBNAIL: return &tag_table_IFD[0];
  case SECTION_COMMENT:   return &tag_table_IFD[0];
  case SECTION_APP0:      return &tag_table_IFD[0];
  case SECTION_EXIF:      return &tag_table_IFD[0];
  case SECTION_FPIX:      return &tag_table_IFD[0];
  case SECTION_GPS:       return &tag_table_GPS[0];
  case SECTION_INTEROP:   return &tag_table_IOP[0];
  case SECTION_APP12:     return &tag_table_IFD[0];
  case SECTION_WINXP:     return &tag_table_IFD[0];
  }
  return &tag_table_IFD[0];
}

/* Return list of sectionnames specified by sectionlist.
   Return value must be freed */
static char *exif_get_sectionlist(int sectionlist) {
  int i, len, ml = 0;
  char *sections;

  for(i=0; i<SECTION_COUNT; i++) {
    ml += exif_get_sectionname(i).size() + 2;
  }
  sections = (char *)IM_MALLOC(ml + 1);
  CHECK_ALLOC_R(sections, ml + 1, nullptr);
  sections[0] = '\0';
  len = 0;
  for(i=0; i<SECTION_COUNT; i++) {
    if (sectionlist&(1<<i)) {
      snprintf(sections+len, ml-len, "%s, ", exif_get_sectionname(i).c_str());
      len = strlen(sections);
    }
  }
  if (len>2) {
    sections[len-2] = '\0';
  }
  return sections;
}

/*
   This structure stores Exif header image elements in a simple manner
   Used to store camera data as extracted from the various ways that
   it can be stored in a nexif header
*/
struct file_section {
  int type;
  size_t size;
  unsigned char *data;
};

struct file_section_list {
  int count;
  file_section *list;
};

struct thumbnail_data {
  image_filetype filetype;
  size_t width, height;
  size_t size;
  size_t offset;
  char *data;
};

struct xp_field_type {
  char *value;
  size_t size;
  int tag;
};

struct xp_field_list {
  int count;
  xp_field_type *list;
};

/* This structure is used to store a section of a Jpeg file. */
struct image_info_type {
  req::ptr<File> infile;
  OptString FileName;
  time_t FileDateTime;
  size_t FileSize;
  image_filetype  FileType;
  int Height, Width;
  int IsColor;
  char *make;
  char *model;

  float ApertureFNumber;
  float ExposureTime;
  double FocalplaneUnits;
  float CCDWidth;
  double FocalplaneXRes;
  size_t ExifImageWidth;
  float FocalLength;
  float Distance;

  int motorola_intel; /* 1 Motorola; 0 Intel */

  char *UserComment;
  int UserCommentLength;
  char *UserCommentEncoding;
  char *encode_unicode;
  char *decode_unicode_be;
  char *decode_unicode_le;
  char *encode_jis;
  char *decode_jis_be;
  char *decode_jis_le;
  /* EXIF standard defines Copyright as
     "<Photographer> [ '\0' <Editor> ] ['\0']" */
  char *Copyright;
  char *CopyrightPhotographer;
  char *CopyrightEditor;

  xp_field_list xp_fields;

  thumbnail_data Thumbnail;
  /* other */
  int sections_found; /* FOUND_<marker> */
  image_info_list info_list[SECTION_COUNT];
  /* for parsing */
  bool read_thumbnail;
  bool read_all;
  int ifd_nesting_level;
  /* internal */
  file_section_list file;
};

struct jpeg_sof_info {
  int     bits_per_sample;
  size_t  width;
  size_t  height;
  int     num_components;
};

/* forward declarations */
static int exif_process_IFD_in_JPEG(image_info_type *ImageInfo,
                                    char *dir_start, char *offset_base,
                                    char *end,
                                    size_t IFDlength, size_t displacement,
                                    int section_index);
static int exif_process_IFD_TAG(image_info_type *ImageInfo, char *dir_entry,
                                char *offset_base, char *end, size_t IFDlength,
                                size_t displacement, int section_index,
                                int ReadNextIFD, tag_table_type tag_table);

/*
 Add a file_section to image_info
 returns the used block or -1. if size>0 and data == nullptr buffer of
 size is allocated
*/
static int exif_file_sections_add(image_info_type *ImageInfo, int type,
                                  size_t size, unsigned char *data) {
  file_section *tmp;
  int count = ImageInfo->file.count;
  size_t realloc_size = (count+1) * sizeof(file_section);
  tmp = (file_section *)IM_REALLOC(ImageInfo->file.list, realloc_size);
  if (!tmp) {
    // IM_REALLOC freed the old buffer on failure; null out the stale pointer
    // to prevent use-after-free in exif_file_sections_free (CVE-2026-23867).
    ImageInfo->file.list = nullptr;
    ImageInfo->file.count = 0;
  }
  CHECK_ALLOC_R(tmp, realloc_size, -1);
  ImageInfo->file.list = tmp;
  ImageInfo->file.list[count].type = 0xFFFF;
  ImageInfo->file.list[count].data = nullptr;
  ImageInfo->file.list[count].size = 0;
  ImageInfo->file.count = count+1;
  if (!size) {
    data = nullptr;
  } else if (data == nullptr) {
    data = (unsigned char *)IM_MALLOC(size);
    if (data == nullptr) {
      IM_FREE(tmp);
      // Null out the stale pointer to prevent use-after-free during cleanup.
      ImageInfo->file.list = nullptr;
      ImageInfo->file.count = 0;
    }
    CHECK_ALLOC_R(data, size, -1);
  }
  ImageInfo->file.list[count].type = type;
  ImageInfo->file.list[count].data = data;
  ImageInfo->file.list[count].size = size;
  return count;
}

/* get length of string if buffer if less than buffer size or buffer size */
static size_t php_strnlen(char* str, size_t maxlen) {
  size_t len = 0;

  if (str && maxlen && *str) {
    do {
      len++;
    } while (--maxlen && *(++str));
  }
  return len;
}

/* Add a value to image_info */
static void exif_iif_add_value(image_info_type *image_info, int section_index,
                               const char *name, int tag, int format, int length,
                               void* value, int motorola_intel) {
  size_t idex;
  void *vptr;
  image_info_value *info_value;
  image_info_data  *info_data;
  image_info_data  *list;

  if (length < 0) {
    return;
  }

  size_t realloc_size = (image_info->info_list[section_index].count+1) *
                        sizeof(image_info_data);
  list = (image_info_data*)
    IM_REALLOC(image_info->info_list[section_index].list, realloc_size);
  if (!list) {
    // IM_REALLOC frees the old buffer on failure; null the stale pointer and
    // reset the count so subsequent exif_iif_add_* calls and exif_iif_free
    // cleanup do not use-after-free / double-free it (CVE-2026-23867 sibling).
    image_info->info_list[section_index].list = nullptr;
    image_info->info_list[section_index].count = 0;
  }
  CHECK_ALLOC(list, realloc_size);
  image_info->info_list[section_index].list = list;

  info_data  = &image_info->info_list[section_index].
                list[image_info->info_list[section_index].count];
  memset(info_data, 0, sizeof(image_info_data));
  info_data->tag = tag;
  info_data->format = format;
  info_data->length = length;
  PHP_STRDUP(info_data->name, name);
  info_value = &info_data->value;

  switch (format) {
  case TAG_FMT_STRING:
    if (value) {
      length = php_strnlen((char*)value, length);
      // TODO
      // if (PG(magic_quotes_runtime)) {
      //   info_value->s = php_addslashes(value, length, &length, 0);
      // } else {
      PHP_STRNDUP(info_value->s, (const char *)value, length);
      // }
      info_data->length = (info_value->s ? length : 0);
    } else {
      info_data->length = 0;
      PHP_STRDUP(info_value->s, "");
    }
    break;

  default:
    /* Standard says more types possible but skip them...
     * but allow users to handle data if they know how to
     * So not return but use type UNDEFINED
     * return;
     */
    info_data->tag = TAG_FMT_UNDEFINED;/* otherwise not freed from memory */
    [[fallthrough]];
  case TAG_FMT_SBYTE:
  case TAG_FMT_BYTE:
    /* in contrast to strings bytes do not need to allocate buffer for
       nullptr if length==0 */
    if (!length)
      break;
    [[fallthrough]];
  case TAG_FMT_UNDEFINED:
    if (value) {
      /* do not recompute length here */
      // TODO
      // if (PG(magic_quotes_runtime)) {
      //   info_value->s = php_addslashes(value, length, &length, 0);
      // } else {
      PHP_STRNDUP(info_value->s, (const char *)value, length);
      // }
      info_data->length = (info_value->s ? length : 0);
    } else {
      info_data->length = 0;
      PHP_STRDUP(info_value->s, "");
    }
    break;

  case TAG_FMT_USHORT:
  case TAG_FMT_ULONG:
  case TAG_FMT_URATIONAL:
  case TAG_FMT_SSHORT:
  case TAG_FMT_SLONG:
  case TAG_FMT_SRATIONAL:
  case TAG_FMT_SINGLE:
  case TAG_FMT_DOUBLE:
    if (length==0) {
      break;
    } else if (length>1) {
      info_value->list =
        (image_info_value*)IM_CALLOC(length, sizeof(image_info_value));
      CHECK_ALLOC(info_value->list, sizeof(image_info_value));
    } else {
      info_value = &info_data->value;
    }
    for (idex=0,vptr=value; idex<(size_t)length;
         idex++,vptr=(char *) vptr + get_php_tiff_bytes_per_format(format)) {
      if (length>1) {
        info_value = &info_data->value.list[idex];
      }
      switch (format) {
      case TAG_FMT_USHORT:
        info_value->u = php_ifd_get16u(vptr, motorola_intel);
        break;

      case TAG_FMT_ULONG:
        info_value->u = php_ifd_get32u(vptr, motorola_intel);
        break;

      case TAG_FMT_URATIONAL:
        info_value->ur.num = php_ifd_get32u(vptr, motorola_intel);
        info_value->ur.den = php_ifd_get32u(4+(char *)vptr, motorola_intel);
        break;

      case TAG_FMT_SSHORT:
        info_value->i = php_ifd_get16s(vptr, motorola_intel);
        break;

      case TAG_FMT_SLONG:
        info_value->i = php_ifd_get32s(vptr, motorola_intel);
        break;

      case TAG_FMT_SRATIONAL:
        info_value->sr.num = php_ifd_get32u(vptr, motorola_intel);
        info_value->sr.den = php_ifd_get32u(4+(char *)vptr, motorola_intel);
        break;

      case TAG_FMT_SINGLE:
        info_value->f = *(float *)value;
        break;

      case TAG_FMT_DOUBLE:
        info_value->d = *(double *)value;
        break;
      }
    }
  }
  image_info->sections_found |= 1<<section_index;
  image_info->info_list[section_index].count++;
}

/* Add a tag from IFD to image_info */
static void exif_iif_add_tag(image_info_type *image_info, int section_index,
                             const char *name, int tag, int format,
                             size_t length, void* value) {
  exif_iif_add_value(image_info, section_index, name, tag, format,
                     (int)length, value, image_info->motorola_intel);
}

/* Evaluate number, be it int, rational, or float from directory. */
static double exif_convert_any_format(void *value, int format,
                                      int motorola_intel) {
  int s_den;
  unsigned u_den;

  switch(format) {
    case TAG_FMT_SBYTE:
      return *(signed char *)value;
    case TAG_FMT_BYTE:
      return *(unsigned char *)value;

    case TAG_FMT_USHORT:
      return php_ifd_get16u(value, motorola_intel);
    case TAG_FMT_ULONG:
      return php_ifd_get32u(value, motorola_intel);

    case TAG_FMT_URATIONAL:
      u_den = php_ifd_get32u(4+(char *)value, motorola_intel);
      if (u_den == 0) {
        return 0;
      } else {
        return (double)php_ifd_get32u(value, motorola_intel) / u_den;
      }

    case TAG_FMT_SRATIONAL:
      s_den = php_ifd_get32s(4+(char *)value, motorola_intel);
      if (s_den == 0) {
        return 0;
      } else {
        return (double)php_ifd_get32s(value, motorola_intel) / s_den;
      }

    case TAG_FMT_SSHORT:
      return (signed short)php_ifd_get16u(value, motorola_intel);
    case TAG_FMT_SLONG:
      return php_ifd_get32s(value, motorola_intel);

    /* Not sure if this is correct (never seen float used in Exif format) */
    case TAG_FMT_SINGLE:
      return (double)*(float *)value;
    case TAG_FMT_DOUBLE:
      return *(double *)value;
  }
  return 0;
}

/* Evaluate number, be it int, rational, or float from directory. */
static size_t exif_convert_any_to_int(void *value, int format,
                                      int motorola_intel) {
  int s_den;
  unsigned u_den;

  switch(format) {
    case TAG_FMT_SBYTE:
      return *(signed char *)value;
    case TAG_FMT_BYTE:
      return *(unsigned char *)value;

    case TAG_FMT_USHORT:
      return php_ifd_get16u(value, motorola_intel);
    case TAG_FMT_ULONG:
      return php_ifd_get32u(value, motorola_intel);

    case TAG_FMT_URATIONAL:
      u_den = php_ifd_get32u(4+(char *)value, motorola_intel);
      if (u_den == 0) {
        return 0;
      } else {
        return php_ifd_get32u(value, motorola_intel) / u_den;
      }

    case TAG_FMT_SRATIONAL:
      s_den = php_ifd_get32s(4+(char *)value, motorola_intel);
      if (s_den == 0) {
        return 0;
      } else {
        return (size_t)((double)php_ifd_get32s(value, motorola_intel) / s_den);
      }

    case TAG_FMT_SSHORT:
      return php_ifd_get16u(value, motorola_intel);
    case TAG_FMT_SLONG:
      return php_ifd_get32s(value, motorola_intel);

    /* Not sure if this is correct (never seen float used in Exif format) */
    case TAG_FMT_SINGLE:
      return (size_t)*(float *)value;
    case TAG_FMT_DOUBLE:
      return (size_t)*(double *)value;
  }
  return 0;
}

/* Get 16 bits motorola order (always) for jpeg header stuff. */
static int php_jpg_get16(void *value) {
  return (((unsigned char *)value)[0] << 8) | ((unsigned char *)value)[1];
}

/* Write 16 bit unsigned value to data */
static
void php_ifd_set16u(char *data, unsigned int value, int motorola_intel) {
  if (motorola_intel) {
    data[0] = (value & 0xFF00) >> 8;
    data[1] = (value & 0x00FF);
  } else {
    data[1] = (value & 0xFF00) >> 8;
    data[0] = (value & 0x00FF);
  }
}

/* Convert a 32 bit unsigned value from file's native byte order */
static void php_ifd_set32u(char *data, size_t value, int motorola_intel) {
  if (motorola_intel) {
    data[0] = (value & 0xFF000000) >> 24;
    data[1] = (value & 0x00FF0000) >> 16;
    data[2] = (value & 0x0000FF00) >>  8;
    data[3] = (value & 0x000000FF);
  } else {
    data[3] = (value & 0xFF000000) >> 24;
    data[2] = (value & 0x00FF0000) >> 16;
    data[1] = (value & 0x0000FF00) >>  8;
    data[0] = (value & 0x000000FF);
  }
}

/* Create a value for an ifd from an info_data pointer */
static void* exif_ifd_make_value(image_info_data *info_data,
                                 int motorola_intel) {
  size_t  byte_count;
  char *value_ptr, *data_ptr;
  size_t  i;

  image_info_value  *info_value;

  byte_count =
    get_php_tiff_bytes_per_format(info_data->format) * info_data->length;
  size_t malloc_size = byte_count > 4 ? byte_count : 4;
  value_ptr = (char *)IM_MALLOC(malloc_size);
  CHECK_ALLOC_R(value_ptr, malloc_size, nullptr);
  memset(value_ptr, 0, 4);
  if (!info_data->length) {
    return value_ptr;
  }
  if (info_data->format == TAG_FMT_UNDEFINED ||
      info_data->format == TAG_FMT_STRING ||
      (byte_count>1 && (info_data->format == TAG_FMT_BYTE ||
                        info_data->format == TAG_FMT_SBYTE))) {
    memmove(value_ptr, info_data->value.s, byte_count);
    return value_ptr;
  } else if (info_data->format == TAG_FMT_BYTE) {
    *value_ptr = info_data->value.u;
    return value_ptr;
  } else if (info_data->format == TAG_FMT_SBYTE) {
    *value_ptr = info_data->value.i;
    return value_ptr;
  } else {
    data_ptr = value_ptr;
    for(i=0; i<info_data->length; i++) {
      if (info_data->length==1) {
        info_value = &info_data->value;
      } else {
        info_value = &info_data->value.list[i];
      }
      switch(info_data->format) {
      case TAG_FMT_USHORT:
        php_ifd_set16u(data_ptr, info_value->u, motorola_intel);
        data_ptr += 2;
        break;
      case TAG_FMT_ULONG:
        php_ifd_set32u(data_ptr, info_value->u, motorola_intel);
        data_ptr += 4;
        break;
      case TAG_FMT_SSHORT:
        php_ifd_set16u(data_ptr, info_value->i, motorola_intel);
        data_ptr += 2;
        break;
      case TAG_FMT_SLONG:
        php_ifd_set32u(data_ptr, info_value->i, motorola_intel);
        data_ptr += 4;
        break;
      case TAG_FMT_URATIONAL:
        php_ifd_set32u(data_ptr,   info_value->sr.num, motorola_intel);
        php_ifd_set32u(data_ptr+4, info_value->sr.den, motorola_intel);
        data_ptr += 8;
        break;
      case TAG_FMT_SRATIONAL:
        php_ifd_set32u(data_ptr,   info_value->ur.num, motorola_intel);
        php_ifd_set32u(data_ptr+4, info_value->ur.den, motorola_intel);
        data_ptr += 8;
        break;
      case TAG_FMT_SINGLE:
        memmove(data_ptr, &info_value->f, 4);
        data_ptr += 4;
        break;
      case TAG_FMT_DOUBLE:
        memmove(data_ptr, &info_value->d, 8);
        data_ptr += 8;
        break;
      }
    }
  }
  return value_ptr;
}

/*
   Process a COM marker.
   We want to print out the marker contents as legible text;
   we must guard against random junk and varying newline representations.
*/
static void exif_process_COM(image_info_type *image_info, char *value,
                             size_t length) {
  exif_iif_add_tag(image_info, SECTION_COMMENT, "Comment",
                   TAG_COMPUTED_VALUE, TAG_FMT_STRING,
                   length-2, value+2);
}

/* Check and build thumbnail */
static void exif_thumbnail_build(image_info_type *ImageInfo) {
  size_t new_size, new_move, new_value;
  char *new_data;
  void *value_ptr;
  int i, byte_count;
  image_info_list *info_list;
  image_info_data *info_data;

  if (!ImageInfo->read_thumbnail || !ImageInfo->Thumbnail.offset ||
      !ImageInfo->Thumbnail.size) {
    return; /* ignore this call */
  }
  switch(ImageInfo->Thumbnail.filetype) {
  default:
  case IMAGE_FILETYPE_JPEG:
    /* done */
    break;
  case IMAGE_FILETYPE_TIFF_II:
  case IMAGE_FILETYPE_TIFF_MM:
    info_list = &ImageInfo->info_list[SECTION_THUMBNAIL];
    new_size  = 8 + 2 + info_list->count * 12 + 4;
    new_value= new_size; /* offset for ifd values outside ifd directory */
    for (i=0; i<info_list->count; i++) {
      info_data  = &info_list->list[i];
      byte_count =
        get_php_tiff_bytes_per_format(info_data->format) * info_data->length;
      if (byte_count > 4) {
        new_size += byte_count;
      }
    }
    new_move = new_size;
    new_data = (char *)IM_REALLOC(ImageInfo->Thumbnail.data,
                                  ImageInfo->Thumbnail.size + new_size);
    if (!new_data) {
      // IM_REALLOC frees the old buffer on failure; null the stale pointer and
      // reset the size so exif_discard_imageinfo cleanup does not double-free it
      // (CVE-2026-23867 sibling).
      ImageInfo->Thumbnail.data = nullptr;
      ImageInfo->Thumbnail.size = 0;
    }
    CHECK_ALLOC(new_data, ImageInfo->Thumbnail.size + new_size);
    ImageInfo->Thumbnail.data = new_data;
    memmove(ImageInfo->Thumbnail.data + new_move,
            ImageInfo->Thumbnail.data, ImageInfo->Thumbnail.size);
    ImageInfo->Thumbnail.size += new_size;
    /* fill in data */
    if (ImageInfo->motorola_intel) {
      memmove(new_data, "MM\x00\x2a\x00\x00\x00\x08", 8);
    } else {
      memmove(new_data, "II\x2a\x00\x08\x00\x00\x00", 8);
    }
    new_data += 8;
    php_ifd_set16u(new_data, info_list->count, ImageInfo->motorola_intel);
    new_data += 2;
    for (i=0; i<info_list->count; i++) {
      info_data  = &info_list->list[i];
      byte_count =
        get_php_tiff_bytes_per_format(info_data->format) * info_data->length;
      if (info_data->tag==TAG_STRIP_OFFSETS ||
          info_data->tag==TAG_JPEG_INTERCHANGE_FORMAT) {
        php_ifd_set16u(new_data + 0, info_data->tag,
                       ImageInfo->motorola_intel);
        php_ifd_set16u(new_data + 2, TAG_FMT_ULONG,
                       ImageInfo->motorola_intel);
        php_ifd_set32u(new_data + 4, 1, ImageInfo->motorola_intel);
        php_ifd_set32u(new_data + 8, new_move, ImageInfo->motorola_intel);
      } else {
        php_ifd_set16u(new_data + 0, info_data->tag,
                       ImageInfo->motorola_intel);
        php_ifd_set16u(new_data + 2, info_data->format,
                       ImageInfo->motorola_intel);
        php_ifd_set32u(new_data + 4, info_data->length,
                       ImageInfo->motorola_intel);
        value_ptr = exif_ifd_make_value(info_data, ImageInfo->motorola_intel);
        if (byte_count <= 4) {
          memmove(new_data+8, value_ptr, 4);
        } else {
          php_ifd_set32u(new_data+8, new_value, ImageInfo->motorola_intel);
          memmove(ImageInfo->Thumbnail.data+new_value, value_ptr, byte_count);
          new_value += byte_count;
        }
        if (value_ptr) IM_FREE(value_ptr);
      }
      new_data += 12;
    }
    memset(new_data, 0, 4); /* next ifd pointer */
    break;
  }
}

/* Grab the thumbnail, corrected */
static void exif_thumbnail_extract(image_info_type *ImageInfo,
                                   char *offset, size_t length) {
  if (ImageInfo->Thumbnail.data) {
    raise_warning("Multiple possible thumbnails");
    return; /* Should not happen */
  }
  if (!ImageInfo->read_thumbnail) {
    return; /* ignore this call */
  }
  /* according to exif2.1, the thumbnail is not supposed to be greater
     than 64K */
  if (ImageInfo->Thumbnail.size >= 65536 ||
      ImageInfo->Thumbnail.size <= 0 ||
      ImageInfo->Thumbnail.offset <= 0) {
    raise_warning("Illegal thumbnail size/offset");
    return;
  }
  /* Check to make sure we are not going to go past the ExifLength */
  if (((ImageInfo->Thumbnail.offset + ImageInfo->Thumbnail.size) <
       ImageInfo->Thumbnail.offset) ||
      ((ImageInfo->Thumbnail.offset + ImageInfo->Thumbnail.size) > length)) {
    raise_warning("Thumbnail goes IFD boundary or end of file reached");
    return;
  }
  PHP_STRNDUP(ImageInfo->Thumbnail.data, offset + ImageInfo->Thumbnail.offset,
              ImageInfo->Thumbnail.size);
  exif_thumbnail_build(ImageInfo);
}

/* Copy a string/buffer in Exif header to a character string and return
   length of allocated buffer if any. */
static int exif_process_undefined(char **result, char *value,
                                  size_t byte_count) {
  /* we cannot use strlcpy - here the problem is that we have to copy NUL
   * chars up to byte_count, we also have to add a single NUL character to
   * force end of string.
   */
  if (byte_count) {
    PHP_STRNDUP((*result), value, byte_count); /* NULL @ byte_count!!! */
    if (*result) return byte_count+1;
  }
  return 0;
}

/* Copy a string in Exif header to a character string returns length of
   allocated buffer if any. */
static int exif_process_string_raw(char **result, char *value,
                                   size_t byte_count) {
  /* we cannot use strlcpy - here the problem is that we have to copy NUL
   * chars up to byte_count, we also have to add a single NUL character to
   * force end of string.
   */
  *result = 0;
  if (byte_count) {
    (*result) = (char*)IM_MALLOC(byte_count + 1);
    CHECK_ALLOC_R((*result), byte_count + 1, 0);
    memcpy(*result, value, byte_count);
    (*result)[byte_count] = '\0';
    return byte_count+1;
  }
  return 0;
}

/*
 * Copy a string in Exif header to a character string and return length of
   allocated buffer if any. In contrast to exif_process_string this function
   does always return a string buffer */
static int exif_process_string(char **result, char *value,
                               size_t byte_count) {
  /* we cannot use strlcpy - here the problem is that we cannot use strlen to
   * determin length of string and we cannot use strlcpy with len=byte_count+1
   * because then we might get into an EXCEPTION if we exceed an allocated
   * memory page...so we use php_strnlen in conjunction with memcpy and add
   * the NUL char.
   */
    if ((byte_count=php_strnlen(value, byte_count)) > 0) {
      return exif_process_undefined(result, value, byte_count);
    }
    PHP_STRNDUP((*result), "", 1); /* force empty string */
    if (*result) return byte_count+1;
    return 0;
}

/* Process UserComment in IFD. */
static int
exif_process_user_comment(image_info_type* /*ImageInfo*/, char** pszInfoPtr,
                          char** pszEncoding, char* szValuePtr, int ByteCount) {
  int   a;
  *pszEncoding = nullptr;
  /* Copy the comment */
  if (ByteCount>=8) {
    if (!memcmp(szValuePtr, "UNICODE\0", 8)) {
      PHP_STRDUP(*pszEncoding, (const char*)szValuePtr);
      szValuePtr = szValuePtr+8;
      ByteCount -= 8;
      return exif_process_string_raw(pszInfoPtr, szValuePtr, ByteCount);
    } else
    if (!memcmp(szValuePtr, "ASCII\0\0\0", 8)) {
      PHP_STRDUP(*pszEncoding, (const char*)szValuePtr);
      szValuePtr = szValuePtr+8;
      ByteCount -= 8;
    } else
    if (!memcmp(szValuePtr, "JIS\0\0\0\0\0", 8)) {
      /* JIS should be tanslated to MB or we leave it to the user */
      PHP_STRDUP(*pszEncoding, (const char*)szValuePtr);
      szValuePtr = szValuePtr+8;
      ByteCount -= 8;
      return exif_process_string_raw(pszInfoPtr, szValuePtr, ByteCount);
    } else
    if (!memcmp(szValuePtr, "\0\0\0\0\0\0\0\0", 8)) {
      /* 8 NULL means undefined and should be ASCII... */
      PHP_STRDUP(*pszEncoding, "UNDEFINED");
      szValuePtr = szValuePtr+8;
      ByteCount -= 8;
    }
  }

  /* Olympus has this padded with trailing spaces.  Remove these first. */
  if (ByteCount>0) {
    for (a=ByteCount-1;a && szValuePtr[a]==' ';a--) {
      (szValuePtr)[a] = '\0';
    }
  }

  /* normal text without encoding */
  exif_process_string(pszInfoPtr, szValuePtr, ByteCount);
  return strlen(*pszInfoPtr);
}

/* Process unicode field in IFD. */
static int
exif_process_unicode(image_info_type* /*ImageInfo*/, xp_field_type* xp_field,
                     int tag, char* szValuePtr, int ByteCount) {
  xp_field->tag = tag;
  xp_field->value = nullptr;

  /* Copy the comment */
  xp_field->size =
    exif_process_string_raw(&xp_field->value, szValuePtr, ByteCount);
  return xp_field->size;
}

/* Process nested IFDs directories in Maker Note. */
static int exif_process_IFD_in_MAKERNOTE(image_info_type *ImageInfo,
                                         char * value_ptr, int value_len,
                                         char *offset_base, size_t IFDlength,
                                         size_t displacement) {
  int de, section_index = SECTION_MAKERNOTE;
  int NumDirEntries, old_motorola_intel, offset_diff;
  const maker_note_type *maker_note;
  char *dir_start;
  char *value_end = value_ptr + value_len;

  for (unsigned int i=0;
       i<=sizeof(maker_note_array)/sizeof(maker_note_type); i++) {
    if (i==sizeof(maker_note_array)/sizeof(maker_note_type))
      return 0;
    maker_note = maker_note_array+i;

    if (maker_note->make &&
        (!ImageInfo->make || strcmp(maker_note->make, ImageInfo->make))) {
      continue;
    }
    if (maker_note->model &&
        (!ImageInfo->model || strcmp(maker_note->model, ImageInfo->model))) {
      continue;
    }
    if (maker_note->id_string &&
        strncmp(maker_note->id_string, value_ptr,
                (maker_note->id_string_len < value_len ?
                 maker_note->id_string_len : value_len))) {
      continue;
    }
    break;
  }

  if (value_len < 2 || maker_note->offset >= value_len - 1) {
    raise_warning("IFD data too short: 0x%04X offset 0x%04X", value_len, maker_note->offset);
    return 0;
  }

  dir_start = value_ptr + maker_note->offset;
  ImageInfo->sections_found |= FOUND_MAKERNOTE;

  old_motorola_intel = ImageInfo->motorola_intel;
  switch (maker_note->byte_order) {
    case MN_ORDER_INTEL:
      ImageInfo->motorola_intel = 0;
      break;
    case MN_ORDER_MOTOROLA:
      ImageInfo->motorola_intel = 1;
      break;
    default:
    case MN_ORDER_NORMAL:
      break;
  }
  if (value_end - dir_start < 2) return 0;
  NumDirEntries = php_ifd_get16u(dir_start, ImageInfo->motorola_intel);

  switch (maker_note->offset_mode) {
    case MN_OFFSET_MAKER:
      offset_base = value_ptr;
      break;
    case MN_OFFSET_GUESS:
      if (value_end - (dir_start+10) < 4) return 0;
      offset_diff = 2 + NumDirEntries*12 + 4 -
                    php_ifd_get32u(dir_start+10, ImageInfo->motorola_intel);
      if (offset_diff < 0 || offset_diff >= value_len) return 0;
      offset_base = value_ptr + offset_diff;
      break;
    default:
    case MN_OFFSET_NORMAL:
      break;
  }

  if ((2+NumDirEntries*12) > value_len) {
    raise_warning("Illegal IFD size: 2 + x%04X*12 = x%04X > x%04X",
                    NumDirEntries, 2+NumDirEntries*12, value_len);
    return 0;
  }
  if ((dir_start - value_ptr) > value_len - (2+NumDirEntries*12)) {
    raise_warning("Illegal IFD size: 0x%04lX > 0x%04X",
                  (dir_start - value_ptr) + (2+NumDirEntries*12),
                  value_len);
    return 0;
  }

  for (de=0;de<NumDirEntries;de++) {
    if (!exif_process_IFD_TAG(ImageInfo, dir_start + 2 + 12 * de,
                              offset_base, value_end, IFDlength, displacement,
                              section_index, 0, maker_note->tag_table)) {
      return 0;
    }
  }
  ImageInfo->motorola_intel = old_motorola_intel;
  return 0;
}

#define REQUIRE_NON_EMPTY() do { \
  if (byte_count == 0) { \
    raise_warning("Process tag(x%04X=%s): Cannot be empty", tag, exif_get_tagname(tag, tagname, -12, tag_table)); \
    return 0; \
  } \
} while (0)

/* Process one of the nested IFDs directories. */
static int exif_process_IFD_TAG(image_info_type *ImageInfo, char *dir_entry,
                                char *offset_base, char *end, size_t IFDlength,
                                size_t displacement, int section_index,
                                int ReadNextIFD, tag_table_type tag_table) {
  size_t length;
  int tag, format, components;
  char *value_ptr, *value_ptr_end, tagname[64], cbuf[32], *outside=nullptr;
  size_t byte_count, offset_val, fpos, fgot;
  int64_t byte_count_signed;
  xp_field_type *tmp_xp;

  /* Protect against corrupt headers */
  if (ImageInfo->ifd_nesting_level > MAX_IFD_NESTING_LEVEL) {
    raise_warning("corrupt EXIF header: maximum directory "
                    "nesting level reached");
    return 0;
  }
  ImageInfo->ifd_nesting_level++;

  CHECK_BUFFER_R(dir_entry+4, end, 4, 0);
  tag = php_ifd_get16u(dir_entry, ImageInfo->motorola_intel);
  format = php_ifd_get16u(dir_entry+2, ImageInfo->motorola_intel);
  components = php_ifd_get32u(dir_entry+4, ImageInfo->motorola_intel);

  if (!format || format > NUM_FORMATS) {
    /* (-1) catches illegal zero case as unsigned underflows to
       positive large. */
    raise_warning("Process tag(x%04X=%s): Illegal format code 0x%04X, "
                    "suppose BYTE", tag,
                    exif_get_tagname(tag, tagname, -12, tag_table), format);
    format = TAG_FMT_BYTE;
    /*return TRUE;*/
  }

  if (components < 0) {
    raise_warning("Process tag(x%04X=%s): Illegal components(%d)",
                    tag, exif_get_tagname(tag, tagname, -12, tag_table),
                    components);
    return 1;
  }

  byte_count_signed = (int64_t)components *
                      get_php_tiff_bytes_per_format(format);

  if (byte_count_signed < 0 || (byte_count_signed > 2147483648)) {
    raise_warning("Process tag(x%04X=%s): Illegal byte_count(%ld)",
                    tag, exif_get_tagname(tag, tagname, -12, tag_table),
                    byte_count_signed);
    return 1; // ignore that field, but don't abort parsing
  }
  byte_count = (size_t)byte_count_signed;

  if (byte_count > 4) {
    CHECK_BUFFER_R(dir_entry+8, end, 4, 0);
    offset_val = php_ifd_get32u(dir_entry+8, ImageInfo->motorola_intel);
    /* If its bigger than 4 bytes, the dir entry contains an offset. */
    value_ptr = offset_base+offset_val;
    value_ptr_end = end;
    if (byte_count > IFDlength ||
        offset_val > IFDlength-byte_count ||
        value_ptr < dir_entry ||
        offset_val < (size_t)(dir_entry-offset_base)) {
      /*
      // It is important to check for IMAGE_FILETYPE_TIFF
      // JPEG does not use absolute pointers instead
      // its pointers are relative to the start
      // of the TIFF header in APP1 section.
      */
      if (byte_count > ImageInfo->FileSize ||
          offset_val>ImageInfo->FileSize-byte_count ||
          (ImageInfo->FileType!=IMAGE_FILETYPE_TIFF_II &&
           ImageInfo->FileType!=IMAGE_FILETYPE_TIFF_MM &&
           ImageInfo->FileType!=IMAGE_FILETYPE_JPEG)) {
        if (value_ptr < dir_entry) {
          /* we can read this if offset_val > 0 */
          /* some files have their values in other parts of the file */
          raise_warning("Process tag(x%04X=%s): Illegal pointer offset"
                          "(x%04lX < %04lX)", tag,
                          exif_get_tagname(tag, tagname, -12, tag_table),
                          offset_val, dir_entry-offset_base);
        } else {
          /* this is for sure not allowed */
          /* exception are IFD pointers */
          raise_warning("Process tag(x%04X=%s): Illegal pointer offset"
                          "(x%04lX + x%04lX = x%04lX > x%04lX)", tag,
                          exif_get_tagname(tag, tagname, -12, tag_table),
                          offset_val, byte_count, offset_val+byte_count,
                          IFDlength);
        }
        return 0;
      }
      if (byte_count>sizeof(cbuf)) {
        /* mark as outside range and get buffer */
        value_ptr = (char *)IM_MALLOC(byte_count);
        CHECK_ALLOC_R(value_ptr, byte_count, 0);
        value_ptr_end = value_ptr + byte_count;
        outside = value_ptr;
      } else {
        /*
        // in most cases we only access a small range so
        // it is faster to use a static buffer there
        // BUT it offers also the possibility to have
        // pointers read without the need to free them
        // explicitley before returning.
        */
        memset(&cbuf, 0, sizeof(cbuf));
        value_ptr = cbuf;
        value_ptr_end = value_ptr + sizeof(cbuf);
      }

      fpos = ImageInfo->infile->tell();
      ImageInfo->infile->seek(displacement+offset_val, SEEK_SET);
      fgot = ImageInfo->infile->tell();
      if (fgot!=displacement+offset_val) {
        if (outside) IM_FREE(outside);
        raise_warning("Wrong file pointer: 0x%08lX != 0x%08lX",
                        fgot, displacement+offset_val);
        return 0;
      }
      OptString str = ImageInfo->infile->read(byte_count);
      fgot = str.length();
      memcpy(value_ptr, str.c_str(), fgot);
      ImageInfo->infile->seek(fpos, SEEK_SET);
      if (fgot<byte_count) {
        if (outside) IM_FREE(outside);
        raise_warning("Unexpected end of file reached");
        return 0;
      }
    }
  } else {
    /* 4 bytes or less and value is in the dir entry itself */
    value_ptr = dir_entry+8;
    value_ptr_end = end;
    offset_val= value_ptr-offset_base;
  }

  ImageInfo->sections_found |= FOUND_ANY_TAG;
  if (section_index==SECTION_THUMBNAIL) {
    if (!ImageInfo->Thumbnail.data) {
      REQUIRE_NON_EMPTY();
      switch(tag) {
        case TAG_IMAGEWIDTH:
        case TAG_COMP_IMAGE_WIDTH:
          ImageInfo->Thumbnail.width =
            exif_convert_any_to_int(value_ptr, format,
                                    ImageInfo->motorola_intel);
          break;

        case TAG_IMAGEHEIGHT:
        case TAG_COMP_IMAGE_HEIGHT:
          ImageInfo->Thumbnail.height =
            exif_convert_any_to_int(value_ptr, format,
                                    ImageInfo->motorola_intel);
          break;

        case TAG_STRIP_OFFSETS:
        case TAG_JPEG_INTERCHANGE_FORMAT:
          /* accept both formats */
          ImageInfo->Thumbnail.offset =
            exif_convert_any_to_int(value_ptr, format,
                                    ImageInfo->motorola_intel);
          break;

        case TAG_STRIP_BYTE_COUNTS:
          if (ImageInfo->FileType == IMAGE_FILETYPE_TIFF_II ||
              ImageInfo->FileType == IMAGE_FILETYPE_TIFF_MM) {
            ImageInfo->Thumbnail.filetype = ImageInfo->FileType;
          } else {
            /* motorola is easier to read */
            ImageInfo->Thumbnail.filetype = IMAGE_FILETYPE_TIFF_MM;
          }
          ImageInfo->Thumbnail.size =
            exif_convert_any_to_int(value_ptr, format,
                                    ImageInfo->motorola_intel);
          break;

        case TAG_JPEG_INTERCHANGE_FORMAT_LEN:
          if (ImageInfo->Thumbnail.filetype == IMAGE_FILETYPE_UNKNOWN) {
            ImageInfo->Thumbnail.filetype = IMAGE_FILETYPE_JPEG;
            ImageInfo->Thumbnail.size =
              exif_convert_any_to_int(value_ptr, format,
                                      ImageInfo->motorola_intel);
          }
          break;
      }
    }
  } else {
    if (section_index==SECTION_IFD0 || section_index==SECTION_EXIF)
    switch(tag) {
      case TAG_COPYRIGHT:
        /* check for "<photographer> NUL <editor> NUL" */
        if (byte_count>1 && (length=php_strnlen(value_ptr, byte_count)) > 0) {
          if (length<byte_count-1) {
            /* When there are any characters after the first NUL */
            PHP_STRDUP(ImageInfo->CopyrightPhotographer, value_ptr);
            PHP_STRNDUP(
              ImageInfo->CopyrightEditor,
              value_ptr + length + 1,
              byte_count - length - 1
            );
            if (ImageInfo->Copyright) IM_FREE(ImageInfo->Copyright);
            php_vspprintf(&ImageInfo->Copyright, 0, "%s, %s",
                          value_ptr, ImageInfo->CopyrightEditor);
            /* format = TAG_FMT_UNDEFINED; this musn't be ASCII         */
            /* but we are not supposed to change this                   */
            /* keep in mind that image_info does not store editor value */
          } else {
            PHP_STRNDUP(ImageInfo->Copyright, value_ptr, byte_count);
          }
        }
        break;

      case TAG_USERCOMMENT:
        ImageInfo->UserCommentLength =
          exif_process_user_comment(ImageInfo, &(ImageInfo->UserComment),
                                    &(ImageInfo->UserCommentEncoding),
                                    value_ptr, byte_count);
        break;

      case TAG_XP_TITLE:
      case TAG_XP_COMMENTS:
      case TAG_XP_AUTHOR:
      case TAG_XP_KEYWORDS:
      case TAG_XP_SUBJECT: {
        size_t realloc_size =
          (ImageInfo->xp_fields.count+1) * sizeof(xp_field_type);
        tmp_xp = (xp_field_type*)
          IM_REALLOC(ImageInfo->xp_fields.list, realloc_size);
        if (!tmp_xp) {
          if (outside) IM_FREE(outside);
          // IM_REALLOC frees the old buffer on failure; null the stale pointer
          // and reset the count so exif_discard_imageinfo cleanup does not
          // use-after-free / double-free it (CVE-2026-23867 sibling).
          ImageInfo->xp_fields.list = nullptr;
          ImageInfo->xp_fields.count = 0;
        }
        CHECK_ALLOC_R(tmp_xp, realloc_size, 0);
        ImageInfo->sections_found |= FOUND_WINXP;
        ImageInfo->xp_fields.list = tmp_xp;
        ImageInfo->xp_fields.count++;
        exif_process_unicode(ImageInfo,
          &(ImageInfo->xp_fields.list[ImageInfo->xp_fields.count-1]),
          tag, value_ptr, byte_count);
        break;
      }
      case TAG_FNUMBER:
        /* Simplest way of expressing aperture, so I trust it the most.
           (overwrite previously computed value if there is one) */
        REQUIRE_NON_EMPTY();
        ImageInfo->ApertureFNumber =
          (float)exif_convert_any_format(value_ptr, format,
                                         ImageInfo->motorola_intel);
        break;

      case TAG_APERTURE:
      case TAG_MAX_APERTURE:
        /* More relevant info always comes earlier, so only use this
           field if we don't have appropriate aperture information yet. */
        if (ImageInfo->ApertureFNumber == 0) {
          REQUIRE_NON_EMPTY();
          ImageInfo->ApertureFNumber
            = (float)exp(exif_convert_any_format(value_ptr, format,
                           ImageInfo->motorola_intel)*log(2)*0.5);
        }
        break;

      case TAG_SHUTTERSPEED:
        /* More complicated way of expressing exposure time, so only use
           this value if we don't already have it from somewhere else.
           SHUTTERSPEED comes after EXPOSURE TIME
          */
        if (ImageInfo->ExposureTime == 0) {
          REQUIRE_NON_EMPTY();
          ImageInfo->ExposureTime
            = (float)(1/exp(exif_convert_any_format(value_ptr, format,
                              ImageInfo->motorola_intel)*log(2)));
        }
        break;
      case TAG_EXPOSURETIME:
        ImageInfo->ExposureTime = -1;
        break;

      case TAG_COMP_IMAGE_WIDTH:
        REQUIRE_NON_EMPTY();
        ImageInfo->ExifImageWidth =
          exif_convert_any_to_int(value_ptr, format,
                                  ImageInfo->motorola_intel);
        break;

      case TAG_FOCALPLANE_X_RES:
        REQUIRE_NON_EMPTY();
        ImageInfo->FocalplaneXRes =
          exif_convert_any_format(value_ptr, format,
                                  ImageInfo->motorola_intel);
        break;

      case TAG_SUBJECT_DISTANCE:
        /* Inidcates the distacne the autofocus camera is focused to.
           Tends to be less accurate as distance increases. */
        REQUIRE_NON_EMPTY();
        ImageInfo->Distance =
          (float)exif_convert_any_format(value_ptr, format,
                                         ImageInfo->motorola_intel);
        break;

      case TAG_FOCALPLANE_RESOLUTION_UNIT:
        REQUIRE_NON_EMPTY();
        switch((int)exif_convert_any_format(value_ptr, format,
                                            ImageInfo->motorola_intel)) {
          case 1: ImageInfo->FocalplaneUnits = 25.4; break; /* inch */
          case 2:
            /* According to the information I was using, 2 measn meters.
               But looking at the Cannon powershot's files, inches is the only
               sensible value. */
            ImageInfo->FocalplaneUnits = 25.4;
            break;

          case 3: ImageInfo->FocalplaneUnits = 10;   break;  /* centimeter */
          case 4: ImageInfo->FocalplaneUnits = 1;    break;  /* milimeter  */
          case 5: ImageInfo->FocalplaneUnits = .001; break;  /* micrometer */
        }
        break;

      case TAG_SUB_IFD:
        if (format==TAG_FMT_IFD) {
          /* If this is called we are either in a TIFFs thumbnail or
             a JPEG where we cannot handle it */
          /* TIFF thumbnail: our data structure cannot store a thumbnail
             of a thumbnail */
          /* JPEG do we have the data area and what to do with it */
          raise_notice("Skip SUB IFD");
        }
        break;

      case TAG_MAKE:
        PHP_STRNDUP(ImageInfo->make, value_ptr, byte_count);
        break;
      case TAG_MODEL:
        PHP_STRNDUP(ImageInfo->model, value_ptr, byte_count);
        break;

      case TAG_MAKER_NOTE:
        exif_process_IFD_in_MAKERNOTE(ImageInfo, value_ptr, byte_count,
                                      offset_base, IFDlength, displacement);
        break;

      case TAG_EXIF_IFD_POINTER:
      case TAG_GPS_IFD_POINTER:
      case TAG_INTEROP_IFD_POINTER:
        if (ReadNextIFD) {
          REQUIRE_NON_EMPTY();
          char *Subdir_start;
          int sub_section_index = 0;
          switch(tag) {
            case TAG_EXIF_IFD_POINTER:
              ImageInfo->sections_found |= FOUND_EXIF;
              sub_section_index = SECTION_EXIF;
              break;
            case TAG_GPS_IFD_POINTER:
              ImageInfo->sections_found |= FOUND_GPS;
              sub_section_index = SECTION_GPS;
              break;
            case TAG_INTEROP_IFD_POINTER:
              ImageInfo->sections_found |= FOUND_INTEROP;
              sub_section_index = SECTION_INTEROP;
              break;
          }
          CHECK_BUFFER_R(value_ptr, value_ptr_end, 4, 0);
          Subdir_start = offset_base +
                         php_ifd_get32u(value_ptr, ImageInfo->motorola_intel);
          if (Subdir_start < offset_base ||
              Subdir_start > offset_base+IFDlength) {
            raise_warning("Illegal IFD Pointer");
            return 0;
          }
          if (!exif_process_IFD_in_JPEG(ImageInfo, Subdir_start,
                                        offset_base, end, IFDlength,
                                        displacement, sub_section_index)) {
            return 0;
          }
        }
    }
  }
  exif_iif_add_tag(ImageInfo, section_index,
                   exif_get_tagname(tag, tagname, sizeof(tagname), tag_table),
                   tag, format, components, value_ptr);
  if (outside) IM_FREE(outside);
  return 1;
}

/* Process one of the nested IFDs directories. */
static int exif_process_IFD_in_JPEG(image_info_type *ImageInfo,
                                    char *dir_start, char *offset_base,
                                    char *end,
                                    size_t IFDlength, size_t displacement,
                                    int section_index) {
  int de;
  int NumDirEntries;
  int NextDirOffset;

  ImageInfo->sections_found |= FOUND_IFD0;

  CHECK_BUFFER_R(dir_start, end, 2, 0);
  NumDirEntries = php_ifd_get16u(dir_start, ImageInfo->motorola_intel);

  if ((dir_start+2+NumDirEntries*12) > (offset_base+IFDlength)) {
    raise_warning("Illegal IFD size: x%04X + 2 + x%04X*12 = x%04X > x%04lX",
                    (int)((size_t)dir_start+2-(size_t)offset_base),
                    NumDirEntries,
                   (int)((size_t)dir_start+2+
                   NumDirEntries*12-(size_t)offset_base), IFDlength);
    return 0;
  }

  for (de=0;de<NumDirEntries;de++) {
    if (!exif_process_IFD_TAG(ImageInfo, dir_start + 2 + 12 * de,
                              offset_base, end, IFDlength, displacement,
                              section_index, 1,
                              exif_get_tag_table(section_index))) {
      return 0;
    }
  }
  /*
   * Ignore IFD2 if it purportedly exists
   */
  if (section_index == SECTION_THUMBNAIL) {
    return true;
  }
  /*
   * Hack to make it process IDF1 I hope
   * There are 2 IDFs, the second one holds the keys (0x0201 and 0x0202)
   * to the thumbnail
   */
  CHECK_BUFFER_R(dir_start+2+12*de, end, 4, 0);
  NextDirOffset =
    php_ifd_get32u(dir_start+2+12*de, ImageInfo->motorola_intel);
  if (NextDirOffset) {
    /* the next line seems false but here IFDlength means
       length of all IFDs */
    if (offset_base + NextDirOffset < offset_base ||
        offset_base + NextDirOffset > offset_base+IFDlength) {
      raise_warning("Illegal IFD offset");
      return 0;
    }
    /* That is the IFD for the first thumbnail */
    if (exif_process_IFD_in_JPEG(ImageInfo, offset_base + NextDirOffset,
                                 offset_base, end, IFDlength, displacement,
                                 SECTION_THUMBNAIL)) {
      if (ImageInfo->Thumbnail.filetype != IMAGE_FILETYPE_UNKNOWN &&
          ImageInfo->Thumbnail.size &&
          ImageInfo->Thumbnail.offset &&
          ImageInfo->read_thumbnail) {
        exif_thumbnail_extract(ImageInfo, offset_base, IFDlength);
      }
      return 1;
    } else {
      return 0;
    }
  }
  return 1;
}

/* Process a TIFF header in a JPEG file */
static void exif_process_TIFF_in_JPEG(image_info_type *ImageInfo,
                                      char *CharBuf, size_t length,
                                      size_t displacement) {
  char *end = CharBuf + length;
  unsigned exif_value_2a, offset_of_ifd;

  /* set the thumbnail stuff to nothing so we can test to see if
     they get set up */
  CHECK_BUFFER(CharBuf, end, 2);
  if (memcmp(CharBuf, "II", 2) == 0) {
    ImageInfo->motorola_intel = 0;
  } else if (memcmp(CharBuf, "MM", 2) == 0) {
    ImageInfo->motorola_intel = 1;
  } else {
    raise_warning("Invalid TIFF a lignment marker");
    return;
  }

  /* Check the next two values for correctness. */
  CHECK_BUFFER(CharBuf+4, end, 4);
  exif_value_2a = php_ifd_get16u(CharBuf+2, ImageInfo->motorola_intel);
  offset_of_ifd = php_ifd_get32u(CharBuf+4, ImageInfo->motorola_intel);
  if ( exif_value_2a != 0x2a || offset_of_ifd < 0x08) {
    raise_warning("Invalid TIFF start (1)");
    return;
  }

  if (offset_of_ifd > length) {
    raise_warning("Invalid IFD start");
    return;
  }

  ImageInfo->sections_found |= FOUND_IFD0;
  /* First directory starts at offset 8. Offsets starts at 0. */
  exif_process_IFD_in_JPEG(ImageInfo, CharBuf+offset_of_ifd,
                           CharBuf, end, length/* -14*/, displacement,
                           SECTION_IFD0);

  /* Compute the CCD width, in milimeters. */
  if (ImageInfo->FocalplaneXRes != 0) {
    ImageInfo->CCDWidth = (float)(ImageInfo->ExifImageWidth *
      ImageInfo->FocalplaneUnits / ImageInfo->FocalplaneXRes);
  }
}

/* Process an JPEG APP1 block marker
   Describes all the drivel that most digital cameras include...
*/
static void exif_process_APP1(image_info_type *ImageInfo, char *CharBuf,
                              size_t length, size_t displacement) {
  /* Check the APP1 for Exif Identifier Code */
  char *end = CharBuf + length;
  static const
  unsigned char ExifHeader[] = {0x45, 0x78, 0x69, 0x66, 0x00, 0x00};
  CHECK_BUFFER(CharBuf+2, end, 6);
  if (length <= 8 || memcmp(CharBuf+2, ExifHeader, 6)) {
    raise_warning("Incorrect APP1 Exif Identifier Code");
    return;
  }
  exif_process_TIFF_in_JPEG(ImageInfo, CharBuf + 8, length - 8,
                            displacement+8);
}

/* Process an JPEG APP12 block marker used by OLYMPUS */
static void exif_process_APP12(image_info_type *ImageInfo,
                               char *buffer, size_t length) {
  size_t l1, l2=0;
  if ((l1 = php_strnlen(buffer+2, length-2)) > 0) {
    exif_iif_add_tag(ImageInfo, SECTION_APP12, "Company",
                     TAG_NONE, TAG_FMT_STRING, l1, buffer+2);
    if (length > 2+l1+1) {
      l2 = php_strnlen(buffer+2+l1+1, length-2-l1-1);
      exif_iif_add_tag(ImageInfo, SECTION_APP12, "Info",
                       TAG_NONE, TAG_FMT_STRING, l2, buffer+2+l1+1);
    }
  }
}

/* Process a SOFn marker.  This is useful for the image dimensions */
static void
exif_process_SOFn(unsigned char* Data, int /*marker*/, jpeg_sof_info* result) {
  result->bits_per_sample = Data[2];
  result->height          = php_jpg_get16(Data+3);
  result->width           = php_jpg_get16(Data+5);
  result->num_components  = Data[7];
}

/* Parse the marker stream until SOS or EOI is seen; */
static int exif_scan_JPEG_header(image_info_type *ImageInfo) {
  int sn;
  int marker = 0, last_marker = M_PSEUDO, comment_correction=1;
  int ll, lh;
  unsigned char *Data;
  size_t fpos, size, got, itemlen;
  jpeg_sof_info  sof_info;

  while(true) {
    // get marker byte, swallowing possible padding
    // some software does not count the length bytes of COM section
    // one company doing so is very much envolved in JPEG...
    // so we accept too
    if (last_marker==M_COM && comment_correction) {
      comment_correction = 2;
    }
    do {
      if ((marker = ImageInfo->infile->getc()) == EOF) {
        raise_warning("File structure corrupted");
        return 0;
      }
      if (last_marker==M_COM && comment_correction>0) {
        if (marker!=0xFF) {
          marker = 0xff;
          comment_correction--;
        } else  {
          last_marker = M_PSEUDO; /* stop skipping 0 for M_COM */
        }
      }
    } while (marker == 0xff);
    if (last_marker==M_COM && !comment_correction) {
      raise_notice("Image has corrupt COM section: some software set "
                   "wrong length information");
    }
    if (last_marker==M_COM && comment_correction)
      return M_EOI; /* ah illegal: char after COM section not 0xFF */

    fpos = ImageInfo->infile->tell();

    if (marker == 0xff) {
      // 0xff is legal padding, but if we get that many, something's wrong.
      raise_warning("To many padding bytes");
      return 0;
    }

    /* Read the length of the section. */

    if ((lh = ImageInfo->infile->getc()) == EOF) {
      raise_warning("File structure corrupted");
      return 0;
    }

    if ((ll = ImageInfo->infile->getc()) == EOF) {
      raise_warning("File structure corrupted");
      return 0;
    }

    itemlen = (lh << 8) | ll;

    if (itemlen < 2) {
      raise_warning("File structure corrupted");
      return 0;
    }

    sn = exif_file_sections_add(ImageInfo, marker, itemlen+1, nullptr);
    if (sn == -1) return 0;
    Data = ImageInfo->file.list[sn].data;

    /* Store first two pre-read bytes. */
    Data[0] = (unsigned char)lh;
    Data[1] = (unsigned char)ll;

    OptString str = ImageInfo->infile->read(itemlen-2);
    got = str.length();
    if (got != itemlen-2) {
      raise_warning("Error reading from file: "
                      "got=x%04lX(=%lu) != itemlen-2=x%04lX(=%lu)",
                      got, got, itemlen-2, itemlen-2);
      return 0;
    }
    memcpy(Data+2, str.c_str(), got);
    switch(marker) {
      case M_SOS:   /* stop before hitting compressed data  */
        // If reading entire image is requested, read the rest of the data.
        if (ImageInfo->read_all) {
          /* Determine how much file is left. */
          fpos = ImageInfo->infile->tell();
          size = ImageInfo->FileSize - fpos;
          sn = exif_file_sections_add(ImageInfo, M_PSEUDO, size, nullptr);
          if (sn == -1) return 0;
          Data = ImageInfo->file.list[sn].data;
          str = ImageInfo->infile->read(size);
          got = str.length();
          if (got != size) {
            raise_warning("Unexpected end of file reached");
            return 0;
          }
          memcpy(Data, str.c_str(), got);
        }
        return 1;

      case M_EOI:   /* in case it's a tables-only JPEG stream */
        raise_warning("No image in jpeg!");
        return (ImageInfo->sections_found&(~FOUND_COMPUTED)) ? 1 : 0;

      case M_COM: /* Comment section */
        exif_process_COM(ImageInfo, (char *)Data, itemlen);
        break;

      case M_EXIF:
        if (!(ImageInfo->sections_found&FOUND_IFD0)) {
          /*ImageInfo->sections_found |= FOUND_EXIF;*/
          /* Seen files from some 'U-lead' software with Vivitar scanner
             that uses marker 31 later in the file (no clue what for!) */
          exif_process_APP1(ImageInfo, (char *)Data, itemlen, fpos);
        }
        break;

      case M_APP12:
        exif_process_APP12(ImageInfo, (char *)Data, itemlen);
        break;


      case M_SOF0:
      case M_SOF1:
      case M_SOF2:
      case M_SOF3:
      case M_SOF5:
      case M_SOF6:
      case M_SOF7:
      case M_SOF9:
      case M_SOF10:
      case M_SOF11:
      case M_SOF13:
      case M_SOF14:
      case M_SOF15:
        if ((itemlen - 2) < 6) {
          return 0;
        }

        exif_process_SOFn(Data, marker, &sof_info);
        ImageInfo->Width  = sof_info.width;
        ImageInfo->Height = sof_info.height;
        if (sof_info.num_components == 3) {
          ImageInfo->IsColor = 1;
        } else {
          ImageInfo->IsColor = 0;
        }
        break;
      default:
        /* skip any other marker silently. */
        break;
    }

    /* keep track of last marker */
    last_marker = marker;
  }
  return 1;
}

/* Reallocate a file section returns 0 on success and -1 on failure */
static int exif_file_sections_realloc(image_info_type *ImageInfo,
                                      int section_index, size_t size) {
  void *tmp;

  /* This is not a malloc/realloc check. It is a plausibility check for the
   * function parameters (requirements engineering).
   */
  if (section_index >= ImageInfo->file.count) {
    raise_warning("Illegal reallocating of undefined file section");
    return -1;
  }
  tmp = IM_REALLOC(ImageInfo->file.list[section_index].data, size);
  if (!tmp) {
    // IM_REALLOC frees the old buffer on failure; null the stale pointer and
    // reset the size so exif_file_sections_free cleanup does not double-free it
    // (CVE-2026-23867 sibling).
    ImageInfo->file.list[section_index].data = nullptr;
    ImageInfo->file.list[section_index].size = 0;
  }
  CHECK_ALLOC_R(tmp, size, -1);
  ImageInfo->file.list[section_index].data = (unsigned char *)tmp;
  ImageInfo->file.list[section_index].size = size;
  return 0;
}

/* Parse the TIFF header; */
static int exif_process_IFD_in_TIFF(image_info_type *ImageInfo,
                                    size_t dir_offset, int section_index) {
  int i, sn, num_entries, sub_section_index = 0;
  unsigned char *dir_entry;
  char tagname[64];
  size_t ifd_size, dir_size, entry_offset, next_offset,
         entry_length, entry_value=0, fgot;
  int entry_tag , entry_type;
  tag_table_type tag_table = exif_get_tag_table(section_index);

  if (ImageInfo->ifd_nesting_level > MAX_IFD_NESTING_LEVEL) {
    return 0;
  }

  if (ImageInfo->FileSize >= dir_offset+2) {
    sn = exif_file_sections_add(ImageInfo, M_PSEUDO, 2, nullptr);
    if (sn == -1) return 0;
    /* we do not know the order of sections */
    ImageInfo->infile->seek(dir_offset, SEEK_SET);
    OptString snData = ImageInfo->infile->read(2);
    memcpy(ImageInfo->file.list[sn].data, snData.c_str(), 2);
    num_entries = php_ifd_get16u(ImageInfo->file.list[sn].data,
                                 ImageInfo->motorola_intel);
    dir_size = 2/*num dir entries*/ +
               12/*length of entry*/*num_entries +
               4/* offset to next ifd (points to thumbnail or NULL)*/;
    if (ImageInfo->FileSize >= dir_offset+dir_size) {
      if (exif_file_sections_realloc(ImageInfo, sn, dir_size)) {
        return 0;
      }
      snData = ImageInfo->infile->read(dir_size-2);
      memcpy(ImageInfo->file.list[sn].data+2, snData.c_str(), dir_size-2);
      next_offset =
        php_ifd_get32u(ImageInfo->file.list[sn].data + dir_size - 4,
                       ImageInfo->motorola_intel);
      /* now we have the directory we can look how long it should be */
      ifd_size = dir_size;
      char *end = (char*)ImageInfo->file.list[sn].data + dir_size;
      for(i=0;i<num_entries;i++) {
        dir_entry = ImageInfo->file.list[sn].data+2+i*12;
        CHECK_BUFFER_R(dir_entry+4, end, 4, 0);
        entry_tag = php_ifd_get16u(dir_entry+0, ImageInfo->motorola_intel);
        entry_type = php_ifd_get16u(dir_entry+2, ImageInfo->motorola_intel);
        if (entry_type > NUM_FORMATS) {
          raise_notice("Read from TIFF: tag(0x%04X,%12s): "
                       "Illegal format code 0x%04X, switching to BYTE",
                       entry_tag,
                       exif_get_tagname(entry_tag, tagname, -12, tag_table),
                       entry_type);
          /* Since this is repeated in exif_process_IFD_TAG make it a
             notice here and make it a warning in the exif_process_IFD_TAG
             which is called elsewhere. */
          entry_type = TAG_FMT_BYTE;
        }
        entry_length =
          php_ifd_get32u(dir_entry+4, ImageInfo->motorola_intel) *
          get_php_tiff_bytes_per_format(entry_type);
        if (entry_length <= 4) {
          switch(entry_type) {
          case TAG_FMT_USHORT:
            CHECK_BUFFER_R(dir_entry+8, end, 2, 0);
            entry_value  = php_ifd_get16u(dir_entry+8,
                                          ImageInfo->motorola_intel);
            break;
          case TAG_FMT_SSHORT:
            CHECK_BUFFER_R(dir_entry+8, end, 2, 0);
            entry_value  = php_ifd_get16s(dir_entry+8,
                                          ImageInfo->motorola_intel);
            break;
          case TAG_FMT_ULONG:
            CHECK_BUFFER_R(dir_entry+8, end, 4, 0);
            entry_value  = php_ifd_get32u(dir_entry+8,
                                          ImageInfo->motorola_intel);
            break;
          case TAG_FMT_SLONG:
            CHECK_BUFFER_R(dir_entry+8, end, 4, 0);
            entry_value  = php_ifd_get32s(dir_entry+8,
                                          ImageInfo->motorola_intel);
            break;
          }
          switch(entry_tag) {
          case TAG_IMAGEWIDTH:
          case TAG_COMP_IMAGE_WIDTH:
            ImageInfo->Width  = entry_value;
            break;
          case TAG_IMAGEHEIGHT:
          case TAG_COMP_IMAGE_HEIGHT:
            ImageInfo->Height = entry_value;
            break;
          case TAG_PHOTOMETRIC_INTERPRETATION:
            switch (entry_value) {
            case PMI_BLACK_IS_ZERO:
            case PMI_WHITE_IS_ZERO:
            case PMI_TRANSPARENCY_MASK:
              ImageInfo->IsColor = 0;
              break;
            case PMI_RGB:
            case PMI_PALETTE_COLOR:
            case PMI_SEPARATED:
            case PMI_YCBCR:
            case PMI_CIELAB:
              ImageInfo->IsColor = 1;
              break;
            }
            break;
          }
        } else {
          CHECK_BUFFER_R(dir_entry+8, end, 4, 0);
          entry_offset =
            php_ifd_get32u(dir_entry+8, ImageInfo->motorola_intel);
          /* if entry needs expading ifd cache and entry is at end of
             current ifd cache. */
          /* otherwise there may be huge holes between two entries */
          if (entry_offset + entry_length > dir_offset + ifd_size &&
              entry_offset == dir_offset + ifd_size) {
            ifd_size = entry_offset + entry_length - dir_offset;
          }
        }
      }
      if (ImageInfo->FileSize >=
          dir_offset + ImageInfo->file.list[sn].size) {
        if (ifd_size > dir_size) {
          if (dir_offset + ifd_size > ImageInfo->FileSize) {
            raise_warning("Error in TIFF: filesize(x%04lX) less than "
                            "size of IFD(x%04lX + x%04lX)",
                            ImageInfo->FileSize, dir_offset, ifd_size);
            return 0;
          }
          if (exif_file_sections_realloc(ImageInfo, sn, ifd_size)) {
            return 0;
          } else {
            end = (char*)ImageInfo->file.list[sn].data + dir_size;
          }
          /* read values not stored in directory itself */
          snData = ImageInfo->infile->read(ifd_size-dir_size);
          memcpy(ImageInfo->file.list[sn].data+dir_size, snData.c_str(),
                 ifd_size-dir_size);
        }
        /* now process the tags */
        for(i=0;i<num_entries;i++) {
          dir_entry = ImageInfo->file.list[sn].data+2+i*12;
          CHECK_BUFFER_R(dir_entry+2, end, 2, 0);
          entry_tag = php_ifd_get16u(dir_entry+0, ImageInfo->motorola_intel);
          entry_type = php_ifd_get16u(dir_entry+2, ImageInfo->motorola_intel);
          if (entry_tag == TAG_EXIF_IFD_POINTER ||
              entry_tag == TAG_INTEROP_IFD_POINTER ||
              entry_tag == TAG_GPS_IFD_POINTER ||
              entry_tag == TAG_SUB_IFD) {
            switch(entry_tag) {
            case TAG_EXIF_IFD_POINTER:
              ImageInfo->sections_found |= FOUND_EXIF;
              sub_section_index = SECTION_EXIF;
              break;
            case TAG_GPS_IFD_POINTER:
              ImageInfo->sections_found |= FOUND_GPS;
              sub_section_index = SECTION_GPS;
              break;
            case TAG_INTEROP_IFD_POINTER:
              ImageInfo->sections_found |= FOUND_INTEROP;
              sub_section_index = SECTION_INTEROP;
              break;
            case TAG_SUB_IFD:
              ImageInfo->sections_found |= FOUND_THUMBNAIL;
              sub_section_index = SECTION_THUMBNAIL;
              break;
            }
            CHECK_BUFFER_R(dir_entry+8, end, 4, 0);
            entry_offset =
              php_ifd_get32u(dir_entry+8, ImageInfo->motorola_intel);
            ImageInfo->ifd_nesting_level++;
            exif_process_IFD_in_TIFF(ImageInfo, entry_offset,
                                     sub_section_index);
            if (section_index!=SECTION_THUMBNAIL && entry_tag==TAG_SUB_IFD) {
              if (ImageInfo->Thumbnail.filetype != IMAGE_FILETYPE_UNKNOWN &&
                  ImageInfo->Thumbnail.size &&
                  ImageInfo->Thumbnail.offset &&
                  ImageInfo->read_thumbnail) {
                if (!ImageInfo->Thumbnail.data) {
                  ImageInfo->Thumbnail.data =
                    (char *)IM_MALLOC(ImageInfo->Thumbnail.size);
                  CHECK_ALLOC_R(ImageInfo->Thumbnail.data,
                                ImageInfo->Thumbnail.size, 0);
                  ImageInfo->infile->seek(ImageInfo->Thumbnail.offset,
                                          SEEK_SET);
                  OptString str =
                    ImageInfo->infile->read(ImageInfo->Thumbnail.size);
                  fgot = str.length();
                  if (fgot < ImageInfo->Thumbnail.size) {
                    raise_warning("Thumbnail goes IFD boundary or "
                                    "end of file reached");
                    IM_FREE(ImageInfo->Thumbnail.data);
                    ImageInfo->Thumbnail.data = nullptr;
                  } else {
                    memcpy(ImageInfo->Thumbnail.data, str.c_str(), fgot);
                    exif_thumbnail_build(ImageInfo);
                  }
                }
              }
            }
          } else {
            if (!exif_process_IFD_TAG(ImageInfo, (char*)dir_entry,
                          (char*)(ImageInfo->file.list[sn].data-dir_offset),
                          (char*)(ImageInfo->file.list[sn].data + ifd_size),
                          ifd_size, 0, section_index, 0, tag_table)) {
              return 0;
            }
          }
        }
        /* If we had a thumbnail in a SUB_IFD we have ANOTHER image in
           NEXT IFD */
        if (next_offset && section_index != SECTION_THUMBNAIL) {
          /* this should be a thumbnail IFD */
          /* the thumbnail itself is stored at Tag=StripOffsets */
          ImageInfo->ifd_nesting_level++;
          exif_process_IFD_in_TIFF(ImageInfo, next_offset,
                                   SECTION_THUMBNAIL);
          if (!ImageInfo->Thumbnail.data && ImageInfo->Thumbnail.offset &&
              ImageInfo->Thumbnail.size && ImageInfo->read_thumbnail) {
            ImageInfo->Thumbnail.data =
              (char *)IM_MALLOC(ImageInfo->Thumbnail.size);
            CHECK_ALLOC_R(ImageInfo->Thumbnail.data,
                          ImageInfo->Thumbnail.size, 0);
            ImageInfo->infile->seek(ImageInfo->Thumbnail.offset, SEEK_SET);
            OptString str = ImageInfo->infile->read(ImageInfo->Thumbnail.size);
            fgot = str.length();
            if (fgot < ImageInfo->Thumbnail.size) {
              raise_warning("Thumbnail goes IFD boundary or "
                              "end of file reached");
              IM_FREE(ImageInfo->Thumbnail.data);
              ImageInfo->Thumbnail.data = nullptr;
            } else {
              memcpy(ImageInfo->Thumbnail.data, str.c_str(), fgot);
              exif_thumbnail_build(ImageInfo);
            }
          }
        }
        return 1;
      } else {
        raise_warning("Error in TIFF: filesize(x%04lX) less than "
                        "size of IFD(x%04lX)",
                        ImageInfo->FileSize,
                        dir_offset+ImageInfo->file.list[sn].size);
        return 0;
      }
    } else {
      raise_warning("Error in TIFF: filesize(x%04lX) less than size "
                      "of IFD dir(x%04lX)",
                      ImageInfo->FileSize, dir_offset+dir_size);
      return 0;
    }
  } else {
    raise_warning("Error in TIFF: filesize(x%04lX) less than "
                    "start of IFD dir(x%04lX)",
                    ImageInfo->FileSize, dir_offset+2);
    return 0;
  }
}

/* Parse the marker stream until SOS or EOI is seen; */
static int exif_scan_FILE_header(image_info_type *ImageInfo) {
  unsigned char *file_header;
  int ret = 0;

  ImageInfo->FileType = IMAGE_FILETYPE_UNKNOWN;

  if (ImageInfo->FileSize >= 2) {
    ImageInfo->infile->seek(0, SEEK_SET);
    OptString fileHeader = ImageInfo->infile->read(2);
    if (fileHeader.length() != 2) {
      return 0;
    }
    file_header = (unsigned char *)fileHeader.c_str();
    if ((file_header[0]==0xff) && (file_header[1]==M_SOI)) {
      ImageInfo->FileType = IMAGE_FILETYPE_JPEG;
      if (exif_scan_JPEG_header(ImageInfo)) {
        ret = 1;
      } else {
        raise_warning("Invalid JPEG file");
      }
    } else if (ImageInfo->FileSize >= 8) {
      OptString str = ImageInfo->infile->read(6);
      if (str.length() != 6) {
        return 0;
      }
      fileHeader += str;
      file_header = (unsigned char *)fileHeader.c_str();
      if (!memcmp(file_header, "II\x2A\x00", 4)) {
        ImageInfo->FileType = IMAGE_FILETYPE_TIFF_II;
        ImageInfo->motorola_intel = 0;
        ImageInfo->sections_found |= FOUND_IFD0;
        if (exif_process_IFD_in_TIFF(ImageInfo,
            php_ifd_get32u(file_header + 4, ImageInfo->motorola_intel),
                           SECTION_IFD0)) {
          ret = 1;
        } else {
          raise_warning("Invalid TIFF file");
        }
      } else if (!memcmp(file_header, "MM\x00\x2a", 4)) {
        ImageInfo->FileType = IMAGE_FILETYPE_TIFF_MM;
        ImageInfo->motorola_intel = 1;
        ImageInfo->sections_found |= FOUND_IFD0;
        if (exif_process_IFD_in_TIFF(ImageInfo,
            php_ifd_get32u(file_header + 4, ImageInfo->motorola_intel),
                           SECTION_IFD0)) {
          ret = 1;
        } else {
          raise_warning("Invalid TIFF file");
        }
      } else {
        raise_warning("File not supported");
        return 0;
      }
    }
  } else {
    raise_warning("File too small (%lu)", ImageInfo->FileSize);
  }
  return ret;
}

static int exif_read_file(image_info_type *ImageInfo, OptString FileName,
                          bool read_thumbnail, bool read_all) {
  struct stat st;

  /* Start with an empty image information structure. */
  memset(ImageInfo, 0, sizeof(*ImageInfo));

  ImageInfo->motorola_intel = -1; /* flag as unknown */

  ImageInfo->infile = File::Open(FileName, "rb");
  if (!ImageInfo->infile) {
    raise_warning("Unable to open file %s", FileName.c_str());
    return 0;
  }
  auto plain_file = dyn_cast<PlainFile>(ImageInfo->infile);
  if (plain_file) {
    if (stat(FileName.c_str(), &st) >= 0) {
      if ((st.st_mode & S_IFMT) != S_IFREG) {
        raise_warning("Not a file");
        return 0;
      }
    }

    /* Store file date/time. */
    ImageInfo->FileDateTime = st.st_mtime;
    ImageInfo->FileSize = st.st_size;
  } else {
    if (!ImageInfo->FileSize) {
      ImageInfo->infile->seek(0, SEEK_END);
      ImageInfo->FileSize = ImageInfo->infile->tell();
      ImageInfo->infile->seek(0, SEEK_SET);
    }
  }

  ImageInfo->FileName = HHVM_FN(basename)(FileName);
  ImageInfo->read_thumbnail = read_thumbnail;
  ImageInfo->read_all = read_all;
  ImageInfo->Thumbnail.filetype = IMAGE_FILETYPE_UNKNOWN;

  PHP_STRDUP(ImageInfo->encode_unicode,    "ISO-8859-15");
  PHP_STRDUP(ImageInfo->decode_unicode_be, "UCS-2BE");
  PHP_STRDUP(ImageInfo->decode_unicode_le, "UCS-2LE");
  PHP_STRDUP(ImageInfo->encode_jis,        "");
  PHP_STRDUP(ImageInfo->decode_jis_be,     "JIS");
  PHP_STRDUP(ImageInfo->decode_jis_le,     "JIS");

  ImageInfo->ifd_nesting_level = 0;

  /* Scan the JPEG headers. */
  auto ret = exif_scan_FILE_header(ImageInfo);

  ImageInfo->infile->close();
  return ret;
}

/* Free memory allocated for image_info */
static void exif_iif_free(image_info_type *image_info, int section_index) {
  int  i;
  void *f; /* faster */

  if (image_info->info_list[section_index].count) {
    for (i=0; i < image_info->info_list[section_index].count; i++) {
      if ((f=image_info->info_list[section_index].list[i].name) != nullptr) {
        IM_FREE(f);
      }
      switch(image_info->info_list[section_index].list[i].format) {
      case TAG_FMT_SBYTE:
      case TAG_FMT_BYTE:
        /* in contrast to strings bytes do not need to allocate
           buffer for nullptr if length==0 */
        if (image_info->info_list[section_index].list[i].length<1)
          break;
        [[fallthrough]];
      default:
      case TAG_FMT_UNDEFINED:
      case TAG_FMT_STRING:
        if ((f=image_info->info_list[section_index].list[i].value.s)
             != nullptr) {
          IM_FREE(f);
        }
        break;

      case TAG_FMT_USHORT:
      case TAG_FMT_ULONG:
      case TAG_FMT_URATIONAL:
      case TAG_FMT_SSHORT:
      case TAG_FMT_SLONG:
      case TAG_FMT_SRATIONAL:
      case TAG_FMT_SINGLE:
      case TAG_FMT_DOUBLE:
        /* nothing to do here */
        if (image_info->info_list[section_index].list[i].length > 1) {
          if ((f=image_info->info_list[section_index].list[i].value.list)
              != nullptr) {
            IM_FREE(f);
          }
        }
        break;
      }
    }
  }
  if (image_info->info_list[section_index].list) {
    IM_FREE(image_info->info_list[section_index].list);
  }
}

/* Discard all file_sections in ImageInfo */
static int exif_file_sections_free(image_info_type *ImageInfo) {
  int i;

  if (ImageInfo->file.count) {
    for (i=0; i<ImageInfo->file.count; i++) {
       if (ImageInfo->file.list[i].data) {
         IM_FREE(ImageInfo->file.list[i].data);
       }
    }
  }
  if (ImageInfo->file.list) IM_FREE(ImageInfo->file.list);
  ImageInfo->file.count = 0;
  return 1;
}

/* Discard data scanned by exif_read_file. */
static int exif_discard_imageinfo(image_info_type *ImageInfo) {
  int i;

  if (ImageInfo->UserComment) IM_FREE(ImageInfo->UserComment);
  if (ImageInfo->UserCommentEncoding) {
    IM_FREE(ImageInfo->UserCommentEncoding);
  }
  if (ImageInfo->Copyright) IM_FREE(ImageInfo->Copyright);
  if (ImageInfo->CopyrightPhotographer) {
    IM_FREE(ImageInfo->CopyrightPhotographer);
  }
  if (ImageInfo->CopyrightEditor) IM_FREE(ImageInfo->CopyrightEditor);
  if (ImageInfo->Thumbnail.data) IM_FREE(ImageInfo->Thumbnail.data);
  if (ImageInfo->encode_unicode) IM_FREE(ImageInfo->encode_unicode);
  if (ImageInfo->decode_unicode_be) {
    IM_FREE(ImageInfo->decode_unicode_be);
  }
  if (ImageInfo->decode_unicode_le) {
    IM_FREE(ImageInfo->decode_unicode_le);
  }
  if (ImageInfo->encode_jis) IM_FREE(ImageInfo->encode_jis);
  if (ImageInfo->decode_jis_be) IM_FREE(ImageInfo->decode_jis_be);
  if (ImageInfo->decode_jis_le) IM_FREE(ImageInfo->decode_jis_le);
  if (ImageInfo->make) IM_FREE(ImageInfo->make);
  if (ImageInfo->model) IM_FREE(ImageInfo->model);
  for (i=0; i<ImageInfo->xp_fields.count; i++) {
    if (ImageInfo->xp_fields.list[i].value) {
      IM_FREE(ImageInfo->xp_fields.list[i].value);
    }
  }
  if (ImageInfo->xp_fields.list) IM_FREE(ImageInfo->xp_fields.list);
  for (i=0; i<SECTION_COUNT; i++) {
    exif_iif_free(ImageInfo, i);
  }
  exif_file_sections_free(ImageInfo);
  memset(ImageInfo, 0, sizeof(*ImageInfo));
  return 1;
}

/* Add an int value to image_info */
static void exif_iif_add_int(image_info_type *image_info, int section_index,
                             char *name, int value) {
  image_info_data *info_data;
  image_info_data *list;
  size_t realloc_size = (image_info->info_list[section_index].count+1) *
                        sizeof(image_info_data);
  list = (image_info_data *)
    IM_REALLOC(image_info->info_list[section_index].list, realloc_size);
  if (!list) {
    // IM_REALLOC frees the old buffer on failure; null the stale pointer and
    // reset the count so subsequent exif_iif_add_* calls and exif_iif_free
    // cleanup do not use-after-free / double-free it (CVE-2026-23867 sibling).
    image_info->info_list[section_index].list = nullptr;
    image_info->info_list[section_index].count = 0;
  }
  CHECK_ALLOC(list, realloc_size);
  image_info->info_list[section_index].list = list;

  info_data = &image_info->info_list[section_index].
              list[image_info->info_list[section_index].count];
  memset(info_data, 0, sizeof(image_info_data));
  info_data->tag = (unsigned short)TAG_NONE;
  info_data->format = TAG_FMT_SLONG;
  info_data->length = 1;
  PHP_STRDUP(info_data->name, name);
  info_data->value.i = value;
  image_info->sections_found |= 1<<section_index;
  image_info->info_list[section_index].count++;
}

/* Add a string value to image_info MUST BE NUL TERMINATED */
static void exif_iif_add_str(image_info_type *image_info,
                             int section_index, const char *name, char *value) {
  image_info_data  *info_data;
  image_info_data  *list;

  if (value) {
    size_t realloc_size = (image_info->info_list[section_index].count+1) *
                          sizeof(image_info_data);
    list = (image_info_data *)
      IM_REALLOC(image_info->info_list[section_index].list, realloc_size);
    if (!list) {
      // IM_REALLOC frees the old buffer on failure; null the stale pointer and
      // reset the count so subsequent exif_iif_add_* calls and exif_iif_free
      // cleanup do not use-after-free / double-free it (CVE-2026-23867 sibling).
      image_info->info_list[section_index].list = nullptr;
      image_info->info_list[section_index].count = 0;
    }
    CHECK_ALLOC(list, realloc_size);
    image_info->info_list[section_index].list = list;
    info_data = &image_info->info_list[section_index].
                list[image_info->info_list[section_index].count];
    memset(info_data, 0, sizeof(image_info_data));
    info_data->tag = (unsigned short)TAG_NONE;
    info_data->format = TAG_FMT_STRING;
    info_data->length = 1;
    PHP_STRDUP(info_data->name, name);
    // TODO
    // if (PG(magic_quotes_runtime)) {
    //   info_data->value.s = php_addslashes(value, strlen(value), nullptr, 0);
    // } else {
    PHP_STRDUP(info_data->value.s, value);
    image_info->sections_found |= 1<<section_index;
    image_info->info_list[section_index].count++;
  }
}

/* Add a format string value to image_info MUST BE NUL TERMINATED */
static void exif_iif_add_fmt(image_info_type *image_info, int section_index,
                             char *name, char *value, ...) {
  va_list arglist;

  va_start(arglist, value);
  if (value) {
    char *tmp = 0;
    php_vspprintf_ap(&tmp, 0, value, arglist);
    exif_iif_add_str(image_info, section_index, name, tmp);
    if (tmp) IM_FREE(tmp);
  }
  va_end(arglist);
}

/* Add a string value to image_info MUST BE NUL TERMINATED */
static void exif_iif_add_buffer(image_info_type *image_info,
                                int section_index, char *name,
                                int length, char *value) {
  image_info_data *info_data;
  image_info_data *list;

  if (value) {
    size_t realloc_size = (image_info->info_list[section_index].count+1) *
                          sizeof(image_info_data);
    list = (image_info_data *)
      IM_REALLOC(image_info->info_list[section_index].list, realloc_size);
    if (!list) {
      // IM_REALLOC frees the old buffer on failure; null the stale pointer and
      // reset the count so subsequent exif_iif_add_* calls and exif_iif_free
      // cleanup do not use-after-free / double-free it (CVE-2026-23867 sibling).
      image_info->info_list[section_index].list = nullptr;
      image_info->info_list[section_index].count = 0;
    }
    CHECK_ALLOC(list, realloc_size);
    image_info->info_list[section_index].list = list;
    info_data = &image_info->info_list[section_index].
                list[image_info->info_list[section_index].count];
    memset(info_data, 0, sizeof(image_info_data));
    info_data->tag = (unsigned short)TAG_NONE;
    info_data->format = TAG_FMT_UNDEFINED;
    info_data->length = length;
    PHP_STRDUP(info_data->name, name);
    // if (PG(magic_quotes_runtime)) {
    //   info_data->value.s = php_addslashes(value, length, &length, 0);
    //   info_data->length = length;
    // } else {
    info_data->value.s = (char *)IM_MALLOC(length + 1);
    if (!info_data->value.s) info_data->length = 0;
    CHECK_ALLOC(info_data->value.s, length + 1);
    memcpy(info_data->value.s, value, length);
    info_data->value.s[length] = 0;
    image_info->sections_found |= 1<<section_index;
    image_info->info_list[section_index].count++;
  }
}

/* scan JPEG in thumbnail (memory) */
static int exif_scan_thumbnail(image_info_type *ImageInfo) {
  unsigned char c, *data = (unsigned char*)ImageInfo->Thumbnail.data;
  int n, marker;
  size_t length=2, pos=0;
  jpeg_sof_info sof_info;

  if (!data || ImageInfo->Thumbnail.size < 4) {
    return 0; /* nothing to do here */
  }
  if (memcmp(data, "\xFF\xD8\xFF", 3)) {
    if (!ImageInfo->Thumbnail.width && !ImageInfo->Thumbnail.height) {
      raise_warning("Thumbnail is not a JPEG image");
    }
    return 0;
  }
  for (;;) {
    pos += length;
    if (pos>=ImageInfo->Thumbnail.size)
      return 0;
    c = data[pos++];
    if (pos>=ImageInfo->Thumbnail.size)
      return 0;
    if (c != 0xFF) {
      return 0;
    }
    n = 8;
    while ((c = data[pos++]) == 0xFF && n--) {
      if (pos+3>=ImageInfo->Thumbnail.size)
        return 0;
      /* +3 = pos++ of next check when reaching marker + 2 bytes for length */
    }
    if (c == 0xFF)
      return 0;
    marker = c;
    if (ImageInfo->Thumbnail.size - 2 < pos) return 0;
    length = php_jpg_get16(data+pos);
    if (length > ImageInfo->Thumbnail.size || pos >= ImageInfo->Thumbnail.size - length) {
      return 0;
    }
    switch (marker) {
      case M_SOF0:
      case M_SOF1:
      case M_SOF2:
      case M_SOF3:
      case M_SOF5:
      case M_SOF6:
      case M_SOF7:
      case M_SOF9:
      case M_SOF10:
      case M_SOF11:
      case M_SOF13:
      case M_SOF14:
      case M_SOF15:
        /* handle SOFn block */
        if (length < 8 || ImageInfo->Thumbnail.size - 8 < pos) {
          /* exif_process_SOFn needs 8 bytes */
          return 0;
        }
        exif_process_SOFn(data+pos, marker, &sof_info);
        ImageInfo->Thumbnail.height   = sof_info.height;
        ImageInfo->Thumbnail.width    = sof_info.width;
        return 1;

      case M_SOS:
      case M_EOI:
        raise_warning("Could not compute size of thumbnail");
        return 0;

      default:
        /* just skip */
        break;
    }
  }

  raise_warning("Could not compute size of thumbnail");
  return 0;
}

/* Add image_info to associative array value. */
static void add_assoc_image_info(Array &value, bool sub_array,
                                 image_info_type *image_info,
                                 int section_index) {
  char buffer[64], *val, *name, uname[64];
  int i, ap, l, b, idx=0, unknown=0;
  image_info_value *info_value;
  image_info_data  *info_data;
  Array tmp;
  Array *tmpi = &tmp;
  Array array;

  if (image_info->info_list[section_index].count) {
    if (!sub_array) {
      tmpi = &value;
    }

    for(i=0; i<image_info->info_list[section_index].count; i++) {
      info_data  = &image_info->info_list[section_index].list[i];
      info_value = &info_data->value;
      if (!(name = info_data->name)) {
        snprintf(uname, sizeof(uname), "%d", unknown++);
        name = uname;
      }
      if (info_data->length==0) {
        tmpi->set(OptString(name, CopyString), uninit_null());
      } else {
        switch (info_data->format) {
        default:
          /* Standard says more types possible but skip them...
           * but allow users to handle data if they know how to
           * So not return but use type UNDEFINED
           * return;
           */
        case TAG_FMT_BYTE:
        case TAG_FMT_SBYTE:
        case TAG_FMT_UNDEFINED:
          if (!info_value->s) {
            tmpi->set(OptString(name, CopyString), "");
          } else {
            tmpi->set(OptString(name, CopyString),
                      OptString(info_value->s, info_data->length, CopyString));
          }
          break;

        case TAG_FMT_STRING:
          if (!(val = info_value->s)) {
            val = "";
          }
          if (section_index==SECTION_COMMENT) {
            tmpi->set(idx++, OptString(val, CopyString));
          } else {
            tmpi->set(OptString(name, CopyString), OptString(val, CopyString));
          }
          break;

        case TAG_FMT_URATIONAL:
        case TAG_FMT_SRATIONAL:
        /*case TAG_FMT_BYTE:
        case TAG_FMT_SBYTE:*/
        case TAG_FMT_USHORT:
        case TAG_FMT_SSHORT:
        case TAG_FMT_SINGLE:
        case TAG_FMT_DOUBLE:
        case TAG_FMT_ULONG:
        case TAG_FMT_SLONG:
          /* now the rest, first see if it becomes an array */
          if ((l = info_data->length) > 1) {
            array.clear();
          }
          for(ap=0; ap<l; ap++) {
            if (l>1) {
              info_value = &info_data->value.list[ap];
            }
            switch (info_data->format) {
            case TAG_FMT_BYTE:
              if (l>1) {
                info_value = &info_data->value;
                for (b=0;b<l;b++) {
                  array.set(b, (int)(info_value->s[b]));
                }
                break;
              }
              [[fallthrough]];
            case TAG_FMT_USHORT:
            case TAG_FMT_ULONG:
              if (l==1) {
                tmpi->set(OptString(name, CopyString), (int)info_value->u);
              } else {
                array.set(ap, (int)info_value->u);
              }
              break;

            case TAG_FMT_URATIONAL:
              snprintf(buffer, sizeof(buffer), "%u/%u",
                       info_value->ur.num, info_value->ur.den);
              if (l==1) {
                tmpi->set(OptString(name, CopyString),
                          OptString(buffer, CopyString));
              } else {
                array.set(ap, OptString(buffer, CopyString));
              }
              break;

            case TAG_FMT_SBYTE:
              if (l>1) {
                info_value = &info_data->value;
                for (b=0;b<l;b++) {
                  array.set(ap, (int)info_value->s[b]);
                }
                break;
              }
              [[fallthrough]];
            case TAG_FMT_SSHORT:
            case TAG_FMT_SLONG:
              if (l==1) {
                tmpi->set(OptString(name, CopyString), info_value->i);
              } else {
                array.set(ap, info_value->i);
              }
              break;

            case TAG_FMT_SRATIONAL:
              snprintf(buffer, sizeof(buffer), "%i/%i",
                       info_value->sr.num, info_value->sr.den);
              if (l==1) {
                tmpi->set(OptString(name, CopyString),
                          OptString(buffer, CopyString));
              } else {
                array.set(ap, OptString(buffer, CopyString));
              }
              break;

            case TAG_FMT_SINGLE:
              if (l==1) {
                tmpi->set(OptString(name, CopyString), info_value->f);
              } else {
                array.set(ap, info_value->f);
              }
              break;

            case TAG_FMT_DOUBLE:
              if (l==1) {
                tmpi->set(OptString(name, CopyString), info_value->d);
              } else {
                array.set(ap, info_value->d);
              }
              break;
            }
            info_value = &info_data->value.list[ap];
          }
          if (l>1) {
            tmpi->set(OptString(name, CopyString), array);
          }
          break;
        }
      }
    }
    if (sub_array) {
      value.set(exif_get_sectionname(section_index), tmp);
    }
  }
}

Variant HHVM_FUNCTION(exif_tagname, int64_t index) {
  auto szTemp = exif_get_tagname(index, nullptr, 0, tag_table_IFD);
  if (index <0 || !szTemp || !szTemp[0]) {
    return false;
  } else {
    return OptString(szTemp, CopyString);
  }
}

Variant HHVM_FUNCTION(exif_read_data,
    const OptString& filename, const OptString& sections /*="" */,
    bool arrays /*=false */, bool thumbnail /*=false */) {
  int i, ret, sections_needed=0;
  image_info_type ImageInfo;
  char tmp[64], *sections_str, *s;

  memset(&ImageInfo, 0, sizeof(ImageInfo));
  if (!sections.empty()) {
    php_vspprintf(&sections_str, 0, ",%s,", sections.c_str());

    /* sections_str DOES start with , and SPACES are NOT allowed in names */
    s = sections_str;
    while(*++s) {
      if (*s==' ') {
        *s = ',';
      }
    }
    for (i=0; i<SECTION_COUNT; i++) {
      snprintf(tmp, sizeof(tmp), ",%s,", exif_get_sectionname(i).c_str());
      if (strstr(sections_str, tmp)) {
        sections_needed |= 1<<i;
      }
    }
    if (sections_str) IM_FREE(sections_str);
  }
  ret = exif_read_file(&ImageInfo, filename, thumbnail, 0);
  sections_str = exif_get_sectionlist(ImageInfo.sections_found);
  /* do not inform about in debug*/
  ImageInfo.sections_found |= FOUND_COMPUTED|FOUND_FILE;
  if (ret==0|| (sections_needed &&
                !(sections_needed&ImageInfo.sections_found))) {
    exif_discard_imageinfo(&ImageInfo);
    if (sections_str) IM_FREE(sections_str);
    return false;
  }
  /* now we can add our information */
  exif_iif_add_str(&ImageInfo, SECTION_FILE, "FileName",
                   (char *)ImageInfo.FileName.c_str());
  exif_iif_add_int(&ImageInfo, SECTION_FILE, "FileDateTime",
                   ImageInfo.FileDateTime);
  exif_iif_add_int(&ImageInfo, SECTION_FILE, "FileSize",
                   ImageInfo.FileSize);
  exif_iif_add_int(&ImageInfo, SECTION_FILE, "FileType",
                   ImageInfo.FileType);
  exif_iif_add_str(&ImageInfo, SECTION_FILE, "MimeType",
                   (char*)php_image_type_to_mime_type(ImageInfo.FileType));
  exif_iif_add_str(&ImageInfo, SECTION_FILE, "SectionsFound",
                   sections_str ? sections_str : (char *)"NONE");

  if (ImageInfo.Width>0 &&  ImageInfo.Height>0) {
    exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "html",
                     "width=\"%d\" height=\"%d\"",
                     ImageInfo.Width, ImageInfo.Height);
    exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "Height",
                     ImageInfo.Height);
    exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "Width",
                     ImageInfo.Width);
  }
  exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "IsColor",
                   ImageInfo.IsColor);
  if (ImageInfo.motorola_intel != -1) {
    exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "ByteOrderMotorola",
                     ImageInfo.motorola_intel);
  }
  if (ImageInfo.FocalLength) {
    exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "FocalLength",
                     "%4.1Fmm", ImageInfo.FocalLength);
    if (ImageInfo.CCDWidth) {
      exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "35mmFocalLength",
                       "%dmm",
                       (int)(ImageInfo.FocalLength/ImageInfo.CCDWidth*35+0.5));
    }
  }
  if (ImageInfo.CCDWidth) {
    exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "CCDWidth",
                     "%dmm", (int)ImageInfo.CCDWidth);
  }
  if (ImageInfo.ExposureTime>0) {
    float recip_exposure_time = 0.5f + 1.0f/ImageInfo.ExposureTime;
    if (ImageInfo.ExposureTime <= 0.5 && recip_exposure_time < INT_MAX) {
      exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "ExposureTime", "%0.3F s (1/%d)",
                       ImageInfo.ExposureTime, (int) recip_exposure_time);
    } else {
      exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "ExposureTime",
                       "%0.3F s", ImageInfo.ExposureTime);
    }
  }
  if (ImageInfo.ApertureFNumber) {
    exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "ApertureFNumber",
                     "f/%.1F", ImageInfo.ApertureFNumber);
  }
  if (ImageInfo.Distance) {
    if (ImageInfo.Distance<0) {
      exif_iif_add_str(&ImageInfo, SECTION_COMPUTED, "FocusDistance",
                       "Infinite");
    } else {
      exif_iif_add_fmt(&ImageInfo, SECTION_COMPUTED, "FocusDistance",
                       "%0.2Fm", ImageInfo.Distance);
    }
  }
  if (ImageInfo.UserComment) {
    exif_iif_add_buffer(&ImageInfo, SECTION_COMPUTED, "UserComment",
                        ImageInfo.UserCommentLength, ImageInfo.UserComment);
    if (ImageInfo.UserCommentEncoding &&
        strlen(ImageInfo.UserCommentEncoding)) {
      exif_iif_add_str(&ImageInfo, SECTION_COMPUTED, "UserCommentEncoding",
                       ImageInfo.UserCommentEncoding);
    }
  }

  exif_iif_add_str(&ImageInfo, SECTION_COMPUTED, "Copyright",
                   ImageInfo.Copyright);
  exif_iif_add_str(&ImageInfo, SECTION_COMPUTED, "Copyright.Photographer",
                   ImageInfo.CopyrightPhotographer);
  exif_iif_add_str(&ImageInfo, SECTION_COMPUTED, "Copyright.Editor",
                   ImageInfo.CopyrightEditor);

  for (i=0; i<ImageInfo.xp_fields.count; i++) {
    exif_iif_add_str(&ImageInfo, SECTION_WINXP,
                     exif_get_tagname(ImageInfo.xp_fields.list[i].tag,
                     nullptr, 0, exif_get_tag_table(SECTION_WINXP)),
                     ImageInfo.xp_fields.list[i].value);
  }
  if (ImageInfo.Thumbnail.size) {
    if (thumbnail) {
      /* not exif_iif_add_str : this is a buffer */
      exif_iif_add_tag(&ImageInfo, SECTION_THUMBNAIL, "THUMBNAIL",
                       TAG_NONE, TAG_FMT_UNDEFINED, ImageInfo.Thumbnail.size,
                       ImageInfo.Thumbnail.data);
    }
    if (!ImageInfo.Thumbnail.width || !ImageInfo.Thumbnail.height) {
      /* try to evaluate if thumbnail data is present */
      exif_scan_thumbnail(&ImageInfo);
    }
    exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "Thumbnail.FileType",
                     ImageInfo.Thumbnail.filetype);
    exif_iif_add_str(&ImageInfo, SECTION_COMPUTED, "Thumbnail.MimeType",
      (char*)php_image_type_to_mime_type(ImageInfo.Thumbnail.filetype));
  }
  if (ImageInfo.Thumbnail.width && ImageInfo.Thumbnail.height) {
    exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "Thumbnail.Height",
                     ImageInfo.Thumbnail.height);
    exif_iif_add_int(&ImageInfo, SECTION_COMPUTED, "Thumbnail.Width",
                     ImageInfo.Thumbnail.width);
  }
  if (sections_str) IM_FREE(sections_str);

  Array retarr;
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_FILE);
  add_assoc_image_info(retarr, true, &ImageInfo,
                       SECTION_COMPUTED);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_ANY_TAG);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_IFD0);
  add_assoc_image_info(retarr, true, &ImageInfo,
                       SECTION_THUMBNAIL);
  add_assoc_image_info(retarr, true, &ImageInfo,
                       SECTION_COMMENT);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_EXIF);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_GPS);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_INTEROP);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_FPIX);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_APP12);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_WINXP);
  add_assoc_image_info(retarr, arrays, &ImageInfo,
                       SECTION_MAKERNOTE);

  exif_discard_imageinfo(&ImageInfo);
  return retarr;
}

Variant HHVM_FUNCTION(exif_thumbnail, const OptString& filename,
                         int64_t& width,
                         int64_t& height,
                         int64_t& imagetype) {
  image_info_type ImageInfo;

  memset(&ImageInfo, 0, sizeof(ImageInfo));

  int ret = exif_read_file(&ImageInfo, filename.c_str(), 1, 0);
  if (ret==0) {
    exif_discard_imageinfo(&ImageInfo);
    return false;
  }

  if (!ImageInfo.Thumbnail.data || !ImageInfo.Thumbnail.size) {
    exif_discard_imageinfo(&ImageInfo);
    return false;
  }

  if (!ImageInfo.Thumbnail.width || !ImageInfo.Thumbnail.height) {
    if (!exif_scan_thumbnail(&ImageInfo)) {
      ImageInfo.Thumbnail.width = ImageInfo.Thumbnail.height = 0;
    }
  }
  width = ImageInfo.Thumbnail.width;
  height = ImageInfo.Thumbnail.height;
  imagetype = ImageInfo.Thumbnail.filetype;
  OptString str(ImageInfo.Thumbnail.data, ImageInfo.Thumbnail.size, CopyString);
  exif_discard_imageinfo(&ImageInfo);
  return str;
}

Variant HHVM_FUNCTION(exif_imagetype, const OptString& filename) {
  auto stream = File::Open(filename, "rb");
  if (!stream) {
    return false;
  }
  int itype = php_getimagetype(stream);
  stream->close();
  if (itype == IMAGE_FILETYPE_UNKNOWN) return false;
  return itype;
}

///////////////////////////////////////////////////////////////////////////////

struct ExifExtension final : Extension {
  ExifExtension() : Extension("exif", "1.0", "images_infra_backend") {}

  void moduleRegisterNative() override {
    HHVM_FE(exif_imagetype);
    HHVM_FE(exif_read_data);
    HHVM_FE(exif_tagname);
    HHVM_FE(exif_thumbnail);
  }
} s_exif_extension;

struct GdExtension final : Extension {
  GdExtension() : Extension("gd", "1.0", "images_infra_backend") {}

  void moduleRegisterNative() override {
    HHVM_FE(gd_info);
    HHVM_FE(getimagesize);
    HHVM_FE(getimagesizefromstring);
    HHVM_FE(image_type_to_extension);
    HHVM_FE(image_type_to_mime_type);
    HHVM_FE(image2wbmp);
    HHVM_FE(imageaffine);
    HHVM_FE(imageaffinematrixconcat);
    HHVM_FE(imageaffinematrixget);
    HHVM_FE(imagealphablending);
    HHVM_FE(imageantialias);
    HHVM_FE(imagearc);
    HHVM_FE(imagechar);
    HHVM_FE(imagecharup);
    HHVM_FE(imagecolorallocate);
    HHVM_FE(imagecolorallocatealpha);
    HHVM_FE(imagecolorat);
    HHVM_FE(imagecolorclosest);
    HHVM_FE(imagecolorclosestalpha);
    HHVM_FE(imagecolorclosesthwb);
    HHVM_FE(imagecolordeallocate);
    HHVM_FE(imagecolorexact);
    HHVM_FE(imagecolorexactalpha);
    HHVM_FE(imagecolormatch);
    HHVM_FE(imagecolorresolve);
    HHVM_FE(imagecolorresolvealpha);
    HHVM_FE(imagecolorset);
    HHVM_FE(imagecolorsforindex);
    HHVM_FE(imagecolorstotal);
    HHVM_FE(imagecolortransparent);
    HHVM_FE(imageconvolution);
    HHVM_FE(imagecopy);
    HHVM_FE(imagecopymerge);
    HHVM_FE(imagecopymergegray);
    HHVM_FE(imagecopyresampled);
    HHVM_FE(imagecopyresized);
    HHVM_FE(imagecreate);
    HHVM_FE(imagecreatefromgd2part);
    HHVM_FE(imagecreatefromgd);
    HHVM_FE(imagecreatefromgd2);
    HHVM_FE(imagecreatefromgif);
    HHVM_FE(imagecreatefromjpeg);
    HHVM_FE(imagecreatefrompng);
    HHVM_FE(imagecreatefromstring);
    HHVM_FE(imagecreatefromwbmp);
    HHVM_FE(imagecreatefromxbm);
    HHVM_FE(imagecreatetruecolor);
    HHVM_FE(imagecrop);
    HHVM_FE(imagecropauto);
    HHVM_FE(imagedashedline);
    HHVM_FE(imagedestroy);
    HHVM_FE(imageellipse);
    HHVM_FE(imagefill);
    HHVM_FE(imagefilledarc);
    HHVM_FE(imagefilledellipse);
    HHVM_FE(imagefilledpolygon);
    HHVM_FE(imagefilledrectangle);
    HHVM_FE(imagefilltoborder);
    HHVM_FE(imagefilledellipse);
    HHVM_FE(imagefilledpolygon);
    HHVM_FE(imagefilledrectangle);
    HHVM_FE(imagefilltoborder);
    HHVM_FE(imagefilter);
    HHVM_FE(imageflip);
    HHVM_FE(imagefontheight);
    HHVM_FE(imagefontwidth);
    HHVM_FE(imageftbbox);
    HHVM_FE(imagefttext);
    HHVM_FE(imagegammacorrect);
    HHVM_FE(imagegd2);
    HHVM_FE(imagegd);
    HHVM_FE(imagegif);
    HHVM_FE(imageinterlace);
    HHVM_FE(imageistruecolor);
    HHVM_FE(imagejpeg);
    HHVM_FE(imagelayereffect);
    HHVM_FE(imageline);
    HHVM_FE(imageloadfont);
    HHVM_FE(imagepng);
    HHVM_FE(imagepolygon);
    HHVM_FE(imagerectangle);
    HHVM_FE(imagerotate);
    HHVM_FE(imagesavealpha);
    HHVM_FE(imagescale);
    HHVM_FE(imagesetbrush);
    HHVM_FE(imagesetinterpolation);
    HHVM_FE(imagesetpixel);
    HHVM_FE(imagesetstyle);
    HHVM_FE(imagesetthickness);
    HHVM_FE(imagesettile);
    HHVM_FE(imagestring);
    HHVM_FE(imagestringup);
    HHVM_FE(imagesx);
    HHVM_FE(imagesy);
    HHVM_FE(imagetruecolortopalette);
    HHVM_FE(imagettfbbox);
    HHVM_FE(imagettftext);
    HHVM_FE(imagetypes);
    HHVM_FE(imagewbmp);

    HHVM_FE(iptcembed);
    HHVM_FE(iptcparse);

    HHVM_FE(jpeg2wbmp);
    HHVM_FE(png2wbmp);

    HHVM_FE(imagepalettecopy);

    HHVM_RC_INT(IMG_GIF, IMAGE_TYPE_GIF);
    HHVM_RC_INT(IMG_JPG, IMAGE_TYPE_JPEG);
    HHVM_RC_INT(IMG_JPEG, IMAGE_TYPE_JPEG);
    HHVM_RC_INT(IMG_PNG, IMAGE_TYPE_PNG);
    HHVM_RC_INT(IMG_WBMP, IMAGE_TYPE_WBMP);

    /* special colours for gd */
    HHVM_RC_INT(IMG_COLOR_TILED, gdTiled);
    HHVM_RC_INT(IMG_COLOR_STYLED, gdStyled);
    HHVM_RC_INT(IMG_COLOR_BRUSHED, gdBrushed);
    HHVM_RC_INT(IMG_COLOR_STYLEDBRUSHED, gdStyledBrushed);
    HHVM_RC_INT(IMG_COLOR_TRANSPARENT, gdTransparent);

    /* for imagefilledarc */
    HHVM_RC_INT(IMG_ARC_ROUNDED, gdArc);
    HHVM_RC_INT(IMG_ARC_PIE, gdPie);
    HHVM_RC_INT(IMG_ARC_CHORD, gdChord);
    HHVM_RC_INT(IMG_ARC_NOFILL, gdNoFill);
    HHVM_RC_INT(IMG_ARC_EDGED, gdEdged);

    /* GD2 image format types */
    HHVM_RC_INT(IMG_GD2_RAW, GD2_FMT_RAW);
    HHVM_RC_INT(IMG_GD2_COMPRESSED, GD2_FMT_COMPRESSED);
    HHVM_RC_INT(IMG_FLIP_HORIZONTAL, GD_FLIP_HORINZONTAL);
    HHVM_RC_INT(IMG_FLIP_VERTICAL, GD_FLIP_VERTICAL);
    HHVM_RC_INT(IMG_FLIP_BOTH, GD_FLIP_BOTH);
    HHVM_RC_INT(IMG_EFFECT_REPLACE, gdEffectReplace);
    HHVM_RC_INT(IMG_EFFECT_ALPHABLEND, gdEffectAlphaBlend);
    HHVM_RC_INT(IMG_EFFECT_NORMAL, gdEffectNormal);
    HHVM_RC_INT(IMG_EFFECT_OVERLAY, gdEffectOverlay);

    HHVM_RC_INT(IMG_CROP_DEFAULT, GD_CROP_DEFAULT);
    HHVM_RC_INT(IMG_CROP_TRANSPARENT, GD_CROP_TRANSPARENT);
    HHVM_RC_INT(IMG_CROP_BLACK, GD_CROP_BLACK);
    HHVM_RC_INT(IMG_CROP_WHITE, GD_CROP_WHITE);
    HHVM_RC_INT(IMG_CROP_SIDES, GD_CROP_SIDES);
    HHVM_RC_INT(IMG_CROP_THRESHOLD, GD_CROP_THRESHOLD);

    HHVM_RC_INT(IMG_BELL, GD_BELL);
    HHVM_RC_INT(IMG_BESSEL, GD_BESSEL);
    HHVM_RC_INT(IMG_BILINEAR_FIXED, GD_BILINEAR_FIXED);
    HHVM_RC_INT(IMG_BICUBIC, GD_BICUBIC);
    HHVM_RC_INT(IMG_BICUBIC_FIXED, GD_BICUBIC_FIXED);
    HHVM_RC_INT(IMG_BLACKMAN, GD_BLACKMAN);
    HHVM_RC_INT(IMG_BOX, GD_BOX);
    HHVM_RC_INT(IMG_BSPLINE, GD_BSPLINE);
    HHVM_RC_INT(IMG_CATMULLROM, GD_CATMULLROM);
    HHVM_RC_INT(IMG_GAUSSIAN, GD_GAUSSIAN);
    HHVM_RC_INT(IMG_GENERALIZED_CUBIC, GD_GENERALIZED_CUBIC);
    HHVM_RC_INT(IMG_HERMITE, GD_HERMITE);
    HHVM_RC_INT(IMG_HAMMING, GD_HAMMING);
    HHVM_RC_INT(IMG_HANNING, GD_HANNING);
    HHVM_RC_INT(IMG_MITCHELL, GD_MITCHELL);
    HHVM_RC_INT(IMG_POWER, GD_POWER);
    HHVM_RC_INT(IMG_QUADRATIC, GD_QUADRATIC);
    HHVM_RC_INT(IMG_SINC, GD_SINC);
    HHVM_RC_INT(IMG_NEAREST_NEIGHBOUR, GD_NEAREST_NEIGHBOUR);
    HHVM_RC_INT(IMG_WEIGHTED4, GD_WEIGHTED4);
    HHVM_RC_INT(IMG_TRIANGLE, GD_TRIANGLE);

    HHVM_RC_INT(IMG_AFFINE_TRANSLATE, GD_AFFINE_TRANSLATE);
    HHVM_RC_INT(IMG_AFFINE_SCALE, GD_AFFINE_SCALE);
    HHVM_RC_INT(IMG_AFFINE_ROTATE, GD_AFFINE_ROTATE);
    HHVM_RC_INT(IMG_AFFINE_SHEAR_HORIZONTAL, GD_AFFINE_SHEAR_HORIZONTAL);
    HHVM_RC_INT(IMG_AFFINE_SHEAR_VERTICAL, GD_AFFINE_SHEAR_VERTICAL);

    HHVM_RC_INT(IMG_FILTER_BRIGHTNESS, IMAGE_FILTER_BRIGHTNESS);
    HHVM_RC_INT(IMG_FILTER_COLORIZE, IMAGE_FILTER_COLORIZE);
    HHVM_RC_INT(IMG_FILTER_CONTRAST, IMAGE_FILTER_CONTRAST);
    HHVM_RC_INT(IMG_FILTER_EDGEDETECT, IMAGE_FILTER_EDGEDETECT);
    HHVM_RC_INT(IMG_FILTER_EMBOSS, IMAGE_FILTER_EMBOSS);
    HHVM_RC_INT(IMG_FILTER_GAUSSIAN_BLUR, IMAGE_FILTER_GAUSSIAN_BLUR);
    HHVM_RC_INT(IMG_FILTER_GRAYSCALE, IMAGE_FILTER_GRAYSCALE);
    HHVM_RC_INT(IMG_FILTER_MEAN_REMOVAL, IMAGE_FILTER_MEAN_REMOVAL);
    HHVM_RC_INT(IMG_FILTER_NEGATE, IMAGE_FILTER_NEGATE);
    HHVM_RC_INT(IMG_FILTER_SELECTIVE_BLUR, IMAGE_FILTER_SELECTIVE_BLUR);
    HHVM_RC_INT(IMG_FILTER_SMOOTH, IMAGE_FILTER_SMOOTH);
    HHVM_RC_INT(IMG_FILTER_PIXELATE, IMAGE_FILTER_PIXELATE);

    HHVM_RC_INT(IMAGETYPE_GIF, IMAGE_FILETYPE_GIF);
    HHVM_RC_INT(IMAGETYPE_JPEG, IMAGE_FILETYPE_JPEG);
    HHVM_RC_INT(IMAGETYPE_PNG, IMAGE_FILETYPE_PNG);
    HHVM_RC_INT(IMAGETYPE_SWF, IMAGE_FILETYPE_SWF);
    HHVM_RC_INT(IMAGETYPE_PSD, IMAGE_FILETYPE_PSD);
    HHVM_RC_INT(IMAGETYPE_BMP, IMAGE_FILETYPE_BMP);
    HHVM_RC_INT(IMAGETYPE_TIFF_II, IMAGE_FILETYPE_TIFF_II);
    HHVM_RC_INT(IMAGETYPE_TIFF_MM, IMAGE_FILETYPE_TIFF_MM);
    HHVM_RC_INT(IMAGETYPE_JPC, IMAGE_FILETYPE_JPC);
    HHVM_RC_INT(IMAGETYPE_JP2, IMAGE_FILETYPE_JP2);
    HHVM_RC_INT(IMAGETYPE_JPX, IMAGE_FILETYPE_JPX);
    HHVM_RC_INT(IMAGETYPE_JB2, IMAGE_FILETYPE_JB2);
    HHVM_RC_INT(IMAGETYPE_IFF, IMAGE_FILETYPE_IFF);
    HHVM_RC_INT(IMAGETYPE_WBMP, IMAGE_FILETYPE_WBMP);
    HHVM_RC_INT(IMAGETYPE_XBM, IMAGE_FILETYPE_XBM);
    HHVM_RC_INT(IMAGETYPE_ICO, IMAGE_FILETYPE_ICO);
    HHVM_RC_INT(IMAGETYPE_WEBP, IMAGE_FILETYPE_WEBP);
    HHVM_RC_INT(IMAGETYPE_AVIF, IMAGE_FILETYPE_AVIF);
    HHVM_RC_INT(IMAGETYPE_HEIC, IMAGE_FILETYPE_HEIC);
    HHVM_RC_INT(IMAGETYPE_UNKNOWN, IMAGE_FILETYPE_UNKNOWN);
    HHVM_RC_INT(IMAGETYPE_COUNT, IMAGE_FILETYPE_COUNT);
    HHVM_RC_INT(IMAGETYPE_SWC, IMAGE_FILETYPE_SWC);
    HHVM_RC_INT(IMAGETYPE_JPEG2000, IMAGE_FILETYPE_JPC);

    HHVM_RC_STR(GD_VERSION, GD_VERSION_STRING);

    HHVM_RC_INT_SAME(GD_MAJOR_VERSION);
    HHVM_RC_INT_SAME(GD_MINOR_VERSION);
    HHVM_RC_INT_SAME(GD_RELEASE_VERSION);
    HHVM_RC_STR_SAME(GD_EXTRA_VERSION);

    HHVM_RC_INT(PNG_NO_FILTER, 0x00);
    HHVM_RC_INT(PNG_FILTER_NONE, 0x08);
    HHVM_RC_INT(PNG_FILTER_SUB, 0x10);
    HHVM_RC_INT(PNG_FILTER_UP, 0x20);
    HHVM_RC_INT(PNG_FILTER_AVG, 0x40);
    HHVM_RC_INT(PNG_FILTER_PAETH, 0x80);
    HHVM_RC_INT(PNG_ALL_FILTERS, 0x08 | 0x10 | 0x20 | 0x40 | 0x80);
  }
} s_gd_extension;

///////////////////////////////////////////////////////////////////////////////
}
