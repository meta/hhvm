<?hh /* -*- php -*- */
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 */

const int ZLIB_ENCODING_RAW;
const int ZLIB_ENCODING_GZIP;
const int ZLIB_ENCODING_DEFLATE;
const int ZLIB_ENCODING_ANY;

const int FORCE_GZIP;
const int FORCE_DEFLATE;

<<__PHPStdLib>>
function gzclose(resource $zp): bool;
<<__PHPStdLib>>
function gzcompress(string $data, int $level = -1)[]: mixed;
<<__PHPStdLib>>
function gzdecode(string $data, int $length = PHP_INT_MAX)[]: mixed;
<<__PHPStdLib>>
function gzdeflate(string $data, int $level = -1)[]: mixed;
<<__PHPStdLib>>
function gzencode(string $data, int $level = -1)[]: mixed;
<<__PHPStdLib>>
function gzeof(resource $zp): bool;
<<__PHPStdLib>>
function gzfile(
  string $filename,
  int $use_include_path = 0,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzgetc(resource $zp): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzgets(resource $zp, int $length = 1024): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzgetss(
  resource $zp,
  int $length = 0,
  string $allowable_tags = "",
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzinflate(string $data, int $length = 0)[]: mixed;
<<__PHPStdLib>>
function gzopen(
  string $filename,
  string $mode,
  int $use_include_path = 0,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzpassthru(resource $zp): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzputs(
  resource $zp,
  string $str,
  int $length = 0,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzread(resource $zp, int $length = 0): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzrewind(resource $zp): bool;
<<__PHPStdLib>>
function gzseek(
  resource $zp,
  int $offset,
  int $whence = SEEK_SET,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gztell(resource $zp): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function gzuncompress(string $data, int $length = 0)[]: mixed;
<<__PHPStdLib>>
function gzwrite(
  resource $zp,
  string $str,
  int $length = 0,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function nzcompress(string $uncompressed)[]: HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function nzuncompress(string $compressed)[]: HH\FIXME\MISSING_RETURN_TYPE;
<<
  __PHPStdLib,
  __Deprecated(
    'QuickLZ writes are deprecated. Migrate writers and readers together; '.
    'see T295342194.',
  ),
>>
function qlzcompress(
  string $data,
  int $level = 1,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function qlzuncompress(string $data, int $level = 1): mixed;
<<__PHPStdLib>>
function readgzfile(
  string $filename,
  int $use_include_path = 0,
): HH\FIXME\MISSING_RETURN_TYPE;
<<__PHPStdLib>>
function zlib_decode(
  string $data,
  int $max_len = 0,
)[]: mixed; // string or false
<<__PHPStdLib>>
function zlib_encode(string $data, int $encoding, int $level = -1)[]: mixed;
