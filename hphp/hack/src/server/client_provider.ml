include
  (val if
         Injector_config.use_test_stubbing
         && not (Injector_config.Exclusions.exclude_client_provider ())
       then
         (module Test_client_provider : Client_provider_sig.S)
       else
         (module Server_client_provider : Client_provider_sig.S))
