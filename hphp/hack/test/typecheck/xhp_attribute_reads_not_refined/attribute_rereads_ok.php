<?hh

class :xhp {
  attribute string attr, int count = 0, string req @required;

  public function getAttribute(string $_, mixed $_ = null): mixed {
    return null;
  }
}

function takes_string(string $_): void {}

function local_copy(:xhp $xhp): void {
  $attr = $xhp->:attr;
  if ($attr is nonnull) {
    takes_string($attr);
  }
}

function coalesce(:xhp $xhp): void {
  takes_string($xhp->:attr ?? '');
}

function required_and_default(:xhp $xhp): int {
  takes_string($xhp->:req);
  return $xhp->:count + 1;
}

function as_expression(:xhp $xhp): void {
  takes_string($xhp->:attr as nonnull);
}
