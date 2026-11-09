<?hh

class :xhp {
  attribute string attr, int count = 0, string req @required;

  public function getAttribute(string $_, mixed $_ = null): mixed {
    return null;
  }
}

function takes_string(string $_): void {}

function is_nonnull(:xhp $xhp): void {
  if ($xhp->:attr is nonnull) {
    takes_string($xhp->:attr);
  }
}

function not_null(:xhp $xhp): void {
  if ($xhp->:attr !== null) {
    takes_string($xhp->:attr);
  }
}

function as_nonnull(:xhp $xhp): void {
  $xhp->:attr as nonnull;
  takes_string($xhp->:attr);
}

class :pure {
  attribute string attr;

  final public function getAttribute(string $_, mixed $_ = null)[]: mixed {
    return null;
  }

  public function on_this(): void {
    if ($this->:attr is nonnull) {
      takes_string($this->:attr);
    }
  }
}
