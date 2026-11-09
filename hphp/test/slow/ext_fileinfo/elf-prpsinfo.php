<?hh
// Crafted ET_CORE ELF with a "CORE" NT_PRPSINFO note whose program-name field
// is printable to the end of the note buffer with no NUL. donote()'s name scan
// was unbounded, so it read past nbuf[BUFSIZ] (CVE-2019-8905); the fix stops it
// at the buffer end. Plain finfo() (not MIME mode) so file_tryelf runs; under
// ASan the pre-fix over-read faults.

<<__EntryPoint>>
function main(): void {
  $finfo = new finfo();
  $r = $finfo->file(__DIR__ . '/elf-prpsinfo.input');
  // A long printable run confirms the NT_PRPSINFO name path ran; under ASan the
  // pre-fix scan over-reads nbuf[BUFSIZ] before reaching here.
  var_dump(is_string($r) && strpos($r, str_repeat('A', 256)) !== false);
}
