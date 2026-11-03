<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

namespace HH\Lib\IO;

use namespace HH\Lib\_Private\_IO;

/** The outcome of one request-only body read. */
enum RequestBodyReadStatus: int {
  DATA = 0;
  EOF = 1;
  TIMEOUT = 2;
  BODY_TOO_LARGE = 3;
  TRANSPORT_ERROR = 4;
  CANCELED = 5;
  ABANDONED = 6;
}

type RequestBodyProfile = shape(
  'name' => string,
  'route' => string,
  'segments' => vec<string>,
  'capacity' => int,
  'queue_high_water_mark' => int,
  'max_read_bytes' => int,
  'failure' => ?RequestBodyReadStatus,
);

type RequestBodyReadResult = shape(
  'status' => RequestBodyReadStatus,
  'data' => string,
);

/** Return the server-selected profile and current failure, or null when unselected.
 *
 * Selection is not authorization. route is the configured template; segments
 * contains alternative raw/decoded values for its sole capture, or is empty
 * for a capture-free route. Authorize the application's resolved parameter.
 * failure is null while healthy and never DATA or EOF; EOF is a read result.
 * Capacity bounds admitted requests, not pre-admission queues. The queue
 * high-water mark is a pause threshold, not a physical allocation ceiling.
 */
function request_body_profile(): ?RequestBodyProfile {
  $profile = _IO\request_body_profile();
  if ($profile is null) {
    return null;
  }
  $failure = $profile['failure'];
  $profile['failure'] = $failure is null
    ? null
    : RequestBodyReadStatus::assert($failure);
  return $profile;
}

/** Try to acquire this profile's process-wide slot without waiting.
 *
 * Call only after application authorization and admission checks. Admission is
 * idempotent: repeated successful calls consume no additional capacity. Each
 * reader holds at most one slot; capacity counts concurrently admitted readers.
 * A slot is retained through request-heap cleanup and outstanding native body-read IO.
 * Requests without a selected profile throw InvalidOperationException.
 */
function request_body_try_admit(): bool {
  return _IO\request_body_try_admit();
}

/** Read available bytes without committing a response or waiting to fill a chunk.
 *
 * Size must be positive and at most the profile's max_read_bytes. Timeout is a
 * positive relative nanosecond budget, including command transit to the IO
 * thread. Only one read may be outstanding. Failure results contain no data.
 * Canceling a read prevents subsequent successful reads across the lost chunk.
 * Do not hold a read across work requiring QUEUE_NO_PENDING_IO.
 */
async function request_body_read_async(
  int $max_bytes,
  int $timeout_ns,
): Awaitable<RequestBodyReadResult> {
  list($status, $data) = await _IO\request_body_read($max_bytes, $timeout_ns);
  return shape(
    'status' => RequestBodyReadStatus::assert($status),
    'data' => $data,
  );
}

/** Stop reading this request's input. This does not release its slot early. */
function request_body_abandon(): void {
  _IO\request_body_abandon();
}

/** Return monotonic nanoseconds for an application-owned body/operation budget. */
function request_body_monotonic_ns(): int {
  return _IO\request_body_monotonic_ns();
}
