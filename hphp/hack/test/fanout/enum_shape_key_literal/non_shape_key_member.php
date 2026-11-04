//// enum.php
<?hh

enum Base: int as int {
  A = 1;
  B = 2;
}

// Baseline: all members are literals, so no shape-key errors.

//// plain.php
<?hh

function plain(): int {
  return Base::B;
}

//////////////////////

//// enum.php
<?hh

enum Base: int as int {
  A = 1;
  B = 2 + 1;
}

// Base::B becomes computed, but B is never a shape key: no shape-key
// error, and only plain.php (its plain use) is rechecked.

//////////////////////

//// enum.php
<?hh

enum Base: int as int {
  A = 1;
  B = 2;
}

// Base::B is a literal again: still no shape-key effect.
