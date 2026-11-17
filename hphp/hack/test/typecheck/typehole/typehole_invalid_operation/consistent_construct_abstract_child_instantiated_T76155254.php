<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__ConsistentConstruct>>
class Hgoldstein {}

abstract class HgoldsteinChild extends Hgoldstein {}

final class HgoldsteinGrandChild extends HgoldsteinChild {}

function make_hgoldstein(class<Hgoldstein> $cls): void {
  $_ = new $cls();
}

<<__EntryPoint>>
function hgoldstein_main(): void {
  make_hgoldstein(HgoldsteinChild::class);
}
