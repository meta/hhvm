//// enum.php
<?hh

class K {
  const FWD = 9;
}

enum Base: int as int {
  A = 1;
  B = 2;
}

enum Derived: int as int {
  use Base;
  D = 4;
}

// Baseline: all members are literals, so no shape-key errors.

//// use.php
<?hh

type S = shape(Base::A => int);

function get(S $s): void {
  $_ = $s[Base::A];
}

//// derived.php
<?hh

type DS = shape(Derived::A => int, Derived::D => int);

function get_derived(DS $s): void {
  $_ = $s[Derived::A];
}

//////////////////////

//// enum.php
<?hh

class K {
  const FWD = 9;
}

enum Base: int as int {
  A = K::FWD;
  B = 2;
}

enum Derived: int as int {
  use Base;
  D = 4;
}

// Base::A becomes forwarded (non-literal): shape-key uses in use.php
// and derived.php (via Derived) must error.

//////////////////////

//// enum.php
<?hh

class K {
  const FWD = 9;
}

enum Base: int as int {
  A = 1;
  B = 2;
}

enum Derived: int as int {
  use Base;
  D = 4;
}

// Base::A is a literal again: the errors above must clear.
