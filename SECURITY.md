# Security

## Modules are native code

A module is a DLL that the app loads into its own process. It isn't sandboxed and has the same access to your files, network and devices as you do. Installing a module is like running a program, so only install modules from people you trust.

The app's checks when you use **Install from .zip** protect against unsafe zip files: paths that leave the install folder, symbolic links and oversized contents. They don't tell you whether the module's code is safe.

## Check your download

Each release has a `SHA256SUMS-<version>.txt` file. Compare it with `Get-FileHash <file> -Algorithm SHA256`. Releases aren't code-signed yet, so Windows SmartScreen may warn you.

## Report a problem

Please don't open a public issue for a security problem. Use **Report a vulnerability** on the repository's **Security** tab, which sends a private report to the maintainer.

Include the version, what you did, what happened and, if you can, a small test case. This is a spare-time project, so a reply may take a few days.

## What to report

- Install from .zip writing outside the modules folder.
- Crashes or code execution from a crafted session file, audio file, HRTF file or module manifest.
- A release package containing something it shouldn't.

Only the latest release gets fixes.
