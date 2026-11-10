<?hh // strict

class Supertype {}
class Subtype extends Supertype {}

class WithCtx {
  const ctx MyCtx = [leak_safe];
}

function dependent_ctx(
  (function(Supertype): void) $checker,
)[ctx $checker]: void {}

function const_ctx(WithCtx $t, Supertype $s)[$t::MyCtx]: void {}

function wildcard_ctx((function(Supertype)[_]: void) $cb): void {}

function test(Supertype $supertype, Subtype $subtype): void {}
