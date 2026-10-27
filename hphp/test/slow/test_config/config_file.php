<?hh

<<__EntryPoint>>
function main(): void {
  // Both .inc files are pulled into the repo by the `hphp_opts` block in
  // config_file.php.config, so these succeed in repo-authoritative mode too.
  require_once('config_file_a.inc');
  require_once('config_file_b.inc');
  echo ConfigFileA::NAME.' '.ConfigFileB::NAME."\n";
}
