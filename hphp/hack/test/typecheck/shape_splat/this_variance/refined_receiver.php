<?hh
<<file:__EnableUnstableFeatures('shape_splat_concrete')>>

type Row<T> = shape('self' => T);
interface Marker {}

class Base {
  public function plain(shape('self' => this, 'tag' => bool) $p): void {}
  public function splat(
    shape(...shape('self' => this), 'tag' => bool) $p,
  ): void {}
  public function multiple_splats(
    shape(...shape('tag' => bool), ...shape('self' => this)) $p,
  ): void {}
  public function named_splat(shape(...Row<this>, 'tag' => bool) $p): void {}
  public function nested_splat(
    shape('wrap' => shape(...shape('self' => this), 'tag' => bool)) $p,
  ): void {}
  public function tail(shape('tag' => bool, this...) $p): void {}
  public function nested_tail(
    shape('wrap' => shape('tag' => bool, this...)) $p,
  ): void {}

  public function test(this $other): void {
    if ($this is Marker) {
      $this->plain(shape('self' => $other, 'tag' => true));
      $this->splat(shape('self' => $other, 'tag' => true));
      $this->multiple_splats(shape('self' => $other, 'tag' => true));
      $this->named_splat(shape('self' => $other, 'tag' => true));
      $this->nested_splat(shape('wrap' => shape('self' => $other, 'tag' => true)));
      $this->tail(shape('self' => $other, 'tag' => true));
      $this->nested_tail(shape('wrap' => shape('self' => $other, 'tag' => true)));
    }
  }
}
