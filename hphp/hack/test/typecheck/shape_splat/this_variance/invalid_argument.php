<?hh
<<file:__EnableUnstableFeatures('shape_splat_concrete')>>

interface Marker {}

class Base {
  public function plain(shape('self' => this, 'tag' => bool) $p): void {}
  public function splat(
    shape(...shape('self' => this), 'tag' => bool) $p,
  ): void {}
  public function tail(shape('tag' => bool, this...) $p): void {}

  public function test(Base $other): void {
    if ($this is Marker) {
      $this->plain(shape('self' => $other, 'tag' => true));
      $this->splat(shape('self' => $other, 'tag' => true));
      $this->tail(shape('self' => $other, 'tag' => true));
    }
  }
}
