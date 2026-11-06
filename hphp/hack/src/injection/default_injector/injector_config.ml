let use_test_stubbing = false

module Exclusions = struct
  let exclude_watchman () = false

  let exclude_disk () = false

  let exclude_hg () = false

  let exclude_gk () = false

  let exclude_client_provider () = false

  let exclude_edenfs_watcher () = false

  let exclude_everstore_package () = false

  let exclude_typing_check_service () = false
end
