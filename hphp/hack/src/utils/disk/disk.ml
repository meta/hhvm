include
  (val if
         Injector_config.use_test_stubbing
         && not (Injector_config.Exclusions.exclude_disk ())
       then
         (module Test_disk : Disk_sig.S)
       else
         (module Real_disk : Disk_sig.S))
