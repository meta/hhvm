let use_test_stubbing = true

module Exclusions = struct
  let is_excluded (env_name : string) : bool =
    match Sys.getenv_opt env_name with
    | Some "0"
    | None ->
      false
    | Some _ -> true

  let exclude_watchman () = is_excluded "HH_MOCKING_EXCLUDE_WATCHMAN"

  let exclude_disk () = is_excluded "HH_MOCKING_EXCLUDE_DISK"

  let exclude_hg () = is_excluded "HH_MOCKING_EXCLUDE_HG"

  let exclude_gk () = is_excluded "HH_MOCKING_EXCLUDE_GK"

  let exclude_client_provider () =
    is_excluded "HH_MOCKING_EXCLUDE_CLIENT_PROVIDER"

  let exclude_edenfs_watcher () =
    is_excluded "HH_MOCKING_EXCLUDE_EDENFS_WATCHER"

  let exclude_everstore_package () =
    is_excluded "HH_MOCKING_EXCLUDE_EVERSTORE_PACKAGE"

  let exclude_typing_check_service () =
    is_excluded "HH_MOCKING_EXCLUDE_TYPING_CHECK_SERVICE"
end
