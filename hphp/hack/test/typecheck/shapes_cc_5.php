<?hh
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *
 */

class X {
  <<__ShapeKey>>
  const string X1 = 'field1';
  <<__ShapeKey>>
  const int X2 = 2;
}

// Should fail for mixing int and string class consts
type myshape = shape(
  X::X1 => int,
  X::X2 => bool,
);
