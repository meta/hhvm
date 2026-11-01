<?hh

// A `use` naming a type that doesn't exist reports no naming error; only
// uses of the alias do.
use NoSuchNamespace\NoSuchType;
