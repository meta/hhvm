<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

namespace HH\Lib\_Private\_File;

use namespace HH\Lib\Str;

// Resolve a path relative to some directory.
// This function is expected to be used internally in HSL only, instead of as a
// general utility.
// We need a sophiticated path library based on std::filesystem::canonical or
// something in folly in the future.
function relative_path(string $path, ?string $relative_to = null): string {
  if (
    Str\starts_with($path, '/') ||
    $relative_to === null ||
    $relative_to === ''
  ) {
    return $path;
  } else {
    return $relative_to.'/'.$path;
  }
}
