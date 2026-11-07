# tools/
- `check-toolchain.ps1`, `dev-env.ps1`: check and load the build environment.
- `fetch-asio-sdk.ps1`: download the ASIO SDK by pinned hash. It's never committed.
- `new-module.ps1`, `module-template/`: start a new module.
- `make-release.ps1`: build the release zips in `dist/`. It publishes nothing.
- `generate-notices.ps1`, `changelog-section.ps1`: used by `make-release.ps1` and the release workflow.
- `check-package.ps1`, `check-sdk.ps1`: the `package_contents` and `sdk_out_of_tree` tests.
- `check-module-install.ps1`: proves a separate module's zip works in the packaged app.
- `gen_icons.py`: regenerates the icons in `assets/`.
- `sofa/`: `dsptb-sofa`, which prepares HRTF files.
