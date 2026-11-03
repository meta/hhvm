<?hh

function unbound(): void { $_; }
function bound(int $_): void { $_; }
function closure(): void {
  $f = function() { $_; };
}
function reads(vec<int> $v): void {
  $_ = 0;
  $v[$_] = 1;
  $_[0] = 1;
  list($v[$_]) = tuple(1);
  $_ += 1;
  $_++;
  $f = function(): void use ($_) {};
}
function accepts_inout_int(inout int $x): void {}
function read_inout(): void {
  $_ = 1;
  accepts_inout_int(inout $_);
}

class Reads {
  public function test(): void { $_; }
}
