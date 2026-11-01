<?hh

class :xhp {
  attribute string attr;

  public function __construct(
    public darray<string,mixed> $attributes,
    varray<mixed> $_,
    string $_,
    int $_,
  ): void {}

  public function getAttribute(string $attr, mixed $default = null): mixed {
    $this->attributes['attr'] = 42;

    return $this->attributes[$attr];
  }
}

function takes_string(string $_): void {}

<<__EntryPoint>>
function oops(): void {
  $xhp = <xhp attr="hi" />;
  // Type refinement causes attribute to become an int
  if ($xhp->:attr is nonnull) {
    // The typechecker thinks the type of $xhp->attr is string
    takes_string($xhp->:attr); // Fatals
  }
}
