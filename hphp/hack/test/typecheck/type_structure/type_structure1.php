<?hh

class C<T> {
  const type TC1 = this;
}

type MyAlias<T> = C<T>;
type MyAlias2<T> = (T, T);
type MyAlias3<T> = (function(MyAlias2<T>): Vector<T>);

class D extends C<int> {
  const type TC2 = int;
  const type TC3 = Vector<int>;
  const type TC4 = MyAlias<string>;
  const type TC5 = MyAlias3<C<int>>;
  const type TC6 = shape(
    'foo' => this::TC1,
    'bar' => shape('x' => MyAlias2<num>),
  );

  public function test(): void {
    hh_show(type_structure(D::class, 'TC2'));
    hh_show(type_structure(C::class, 'TC1')['classname']);
    hh_show(type_structure(D::class, 'TC1')['classname']);

    $tc3 = type_structure(D::class, 'TC3');
    hh_show($tc3['classname']);
    hh_show($tc3['generic_types'] ?? null);

    $tc4 = type_structure(D::class, 'TC4');
    hh_show($tc4['classname']);
    hh_show($tc4['generic_types'] ?? null);

    $tc5 = type_structure(D::class, 'TC5');
    hh_show($tc5['param_types']);
    hh_show($tc5['param_types'][0]['elem_types'][1]['generic_types'][0] ?? null);
    hh_show($tc5['return_type']);
    hh_show($tc5['return_type'][0]['classname']);
    hh_show($tc5['return_type'][0]['generic_types'][0]['classname'] ?? null);

    $tc6 = type_structure(D::class, 'TC6')['fields'];
    hh_show($tc6);
    hh_show($tc6['foo']);
    hh_show($tc6['bar']['fields']['x']['elem_types'][1]);
  }
}
