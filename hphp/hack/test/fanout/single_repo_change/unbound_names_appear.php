//// base-a.php
<?hh

class AA {
  public static function foo(): void {}
  public function bar(): void {}
}

//// base-b.php
<?hh

function f(): void {
  AA::class;
}

//// base-c.php
<?hh

type X = vec<AA>;

//// base-d.php
<?hh

function g(): void {
  AA::foo();
}

//// base-e.php
<?hh

function h(AA $a): void {}

//// base-f.php
<?hh

function i(AA $a): void {
  $a->bar();
}

//// base-dict.php
<?hh

function f_dict(): void {
  dict[
    AA::class => 0,
  ];
}

//// base-shape.php
<?hh

function f_shp(): void {
  shape(
    nameof AA => 0,
  );
}

//// changed-a.php
<?hh

class A {
  public static function foo(): void {}
  public function bar(): void {}
}

//// changed-b.php
<?hh

function f(): void {
  AA::class;
}

//// changed-c.php
<?hh

type X = vec<AA>;

//// changed-d.php
<?hh

function g(): void {
  AA::foo();
}

//// changed-e.php
<?hh

function h(AA $a): void {}

//// changed-f.php
<?hh

function i(AA $a): void {
  $a->bar();
}

//// changed-dict.php
<?hh

function f_dict(): void {
  dict[
    AA::class => 0,
  ];
}

//// changed-shape.php
<?hh

function f_shp(): void {
  shape(
    nameof AA => 0,
  );
}
