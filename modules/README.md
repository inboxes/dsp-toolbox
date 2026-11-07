# modules/
First-party module packages, one folder each, built with `dsptb_add_module()`. `common/` holds the shared DSP libraries, which aren't part of the plugin ABI. A module marked `DISTRIBUTION BUNDLED` ships in the app zip; the rest are released on their own.
