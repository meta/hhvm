<?hh
// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

<<file:__EnableUnstableFeatures('require_constraint')>>

interface IDefinesConcreteTypeConst {
  const type TThere = int;
}

class DefinesConcreteTypeConst implements IDefinesConcreteTypeConst {
  use TUsesConcreteTypeConst;
}

trait TUsesConcreteTypeConst {
  require class DefinesConcreteTypeConst;

  const this::TThere I_AM_A_NUMBER = 42;

  const type THere = string;
  const this::THere I_AM_A_STRING = "hello";

  public function behold(): void {
    $a = self::I_AM_A_NUMBER;
    // reports an error - the type of the constant is this::TThere = int, not Banana
    // wasn't reported before because the type was Tany
    give_me_a_banana($a);
    $b = $this::I_AM_A_NUMBER;
    // reports an error - the type of the constant is this::TThere = int, not Banana
    give_me_a_banana($b);
    $c = self::I_AM_A_STRING;
    // type constants defined inside a trait still work
    give_me_a_banana($c);
    $d = $this::I_AM_A_STRING;
    // type constants defined inside a trait still work
    give_me_a_banana($d);

  }
}

final class Banana {}
function give_me_a_banana(Banana $_): void {}
