//// enum.php
<?hh

enum Base: int as int {
  A = 1;
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

type DS = shape(Derived::A => int);

function get_derived(DS $s): void {
  $_ = $s[Derived::A];
}

//////////////////////

//// enum.php
<?hh

enum Base: int as int {
  A = 1 + 1;
}

enum Derived: int as int {
  use Base;
  D = 4;
}

// Base::A becomes computed: shape-key uses in use.php and
// derived.php (via Derived) must error.

//////////////////////

//// deleted-use.php

//////////////////////

//// enum.php
<?hh

enum Base: int as int {
  A = 1;
}

enum Derived: int as int {
  use Base;
  D = 4;
}

// Base::A is a literal again, with use.php deleted: only
// derived.php is rechecked and its error clears.
