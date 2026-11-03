<?hh
/**
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 */

namespace HH\Lib\_Private\_IO;

function response_write(string $data): int;
function response_flush(): void;
function request_read(int $max_bytes): string;

function request_body_profile(): ?shape(
  'name' => string,
  'route' => string,
  'segments' => vec<string>,
  'capacity' => int,
  'queue_high_water_mark' => int,
  'max_read_bytes' => int,
  'failure' => ?int,
);
function request_body_try_admit(): bool;
function request_body_read(
  int $max_bytes,
  int $timeout_ns,
): Awaitable<(int, string)>;
function request_body_abandon(): void;
function request_body_monotonic_ns(): int;
