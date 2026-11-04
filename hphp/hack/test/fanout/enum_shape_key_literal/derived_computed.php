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

//// derived.php
<?hh

type DS = shape(Derived::A => int, Derived::D => int);

function get_derived(DS $s): void {
  $_ = $s[Derived::D];
}

//////////////////////

//// enum.php
<?hh

enum Base: int as int {
  A = 1;
}

enum Derived: int as int {
  use Base;
  D = 4 + 1;
}

// Derived::D becomes computed: only derived.php must error. The
// inherited Base::A stays literal and must not error.

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

// Derived::D is a literal again: its error must clear.
