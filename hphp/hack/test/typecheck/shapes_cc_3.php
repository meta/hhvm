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
  const string X2 = 'field2';
}

// Should fail for mixing class const and string literal
type myshape = shape(
  X::X1 => int,
  'field2' => bool,
);
