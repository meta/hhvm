<?hh

<<__EntryPoint>>
function repo_mode_entrypoint(): void {
  echo ini_get('hhvm.repo.authoritative') ? 'true' : 'false';
}
