# Contributing

Issues and pull requests are welcome. Keep changes to the plugin's public headers documented (they are the API
reference), and add or update an automation test with every behaviour change.

## Running the automation tests

There is no hosted CI for this plugin: building it needs the Unreal Engine. Run the tests locally before you send a
change, in a host project (any C++ or Blueprint project on Unreal Engine 5.3 or newer) with this repository in its
`Plugins/SplatterVault` folder.

1. **Start the API mock** (Node 18 or newer; no dependencies). It is a local stand-in for the public studio API with
   fake test credentials only; it never calls SplatterVault. From this repository's root:

   ```sh
   node tools/sdk-mock/server.mjs --port 8787
   ```

   Use your own instance (not one shared with other clients): the tests reset it, inject faults and expire tokens.
   `node tools/sdk-mock/selftest.mjs` checks the mock itself against the API description it ships with.

2. **Point the tests at it** with the `SV_MOCK_URL` environment variable, set before the editor starts:

   ```bat
   set SV_MOCK_URL=http://127.0.0.1:8787/rest
   ```

   ```sh
   export SV_MOCK_URL=http://127.0.0.1:8787/rest
   ```

   Without `SV_MOCK_URL` the `SplatterVault.Integration.*` tests log a warning and pass without doing anything; the
   `SplatterVault.Unit.*` tests never use the network.

   Optional: `SV_REDIRECT_URL` names a local server that answers every request with a redirect (for example a 307) to
   the same path on the mock. It enables the "a redirect the HTTP stack followed is refused" check (UE 5.5+);
   without it that one check is skipped with a warning.

3. **Run them**, in the editor (Tools > Session Frontend > Automation, filter "SplatterVault", Start Tests) or
   headless:

   ```bat
   UnrealEditor-Cmd.exe C:\Path\Host.uproject -ExecCmds="Automation RunTests SplatterVault" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -log -ReportExportPath=C:\Path\Report
   ```

   ```sh
   UnrealEditor-Cmd /path/to/Host.uproject -ExecCmds="Automation RunTests SplatterVault" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -log -ReportExportPath=/path/to/Report
   ```

   Every test must pass with no errors. Stop the mock when you are done.

The integration tests also compile and run the quick start sample in `Docs/Samples/SvQuickStartSample.inl`, so keep
it building.

## Generated files

Do not commit `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/` or the generated API docs in `Docs/api/`
(`doxygen Docs/Doxyfile`); `.gitignore` already leaves them out.
