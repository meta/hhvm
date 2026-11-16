<?hh

function concrete(TypeStructure<(function(int): arraykey)> $ts): void {
  hh_show($ts['kind']);
  hh_show($ts['alias'] ?? null);
  hh_show($ts['name'] ?? null);
  hh_show($ts['param_types']);
  hh_show($ts['return_type']);

  // any other field will fail
  hh_show($ts['classname']);
  hh_show($ts['nullable']);
  hh_show($ts['elem_types']);
  hh_show($ts['fields']);
  hh_show($ts['generic_types']);

  // make sure kind still works
  hh_show($ts['kind']);
}

function generic<T as (function(): arraykey)>(TypeStructure<T> $ts): void {
  hh_show($ts['kind']);
  hh_show($ts['alias'] ?? null);
  hh_show($ts['name'] ?? null);
  hh_show($ts['param_types']);
  hh_show($ts['return_type']);

  // any other field will fail
  hh_show($ts['classname']);
  hh_show($ts['nullable']);
  hh_show($ts['elem_types']);
  hh_show($ts['fields']);
  hh_show($ts['generic_types']);

  // make sure kind still works
  hh_show($ts['kind']);
}
