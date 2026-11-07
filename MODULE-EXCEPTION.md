# Module licensing

DSP Toolbox is licensed under the GNU General Public License, version 3 only (`LICENSE`). We encourage module authors to release their modules as open source under a GPL-compatible licence.

## Additional permission under GPLv3 section 7

The copyright holders of DSP Toolbox grant this permission for their own code in the host and in `sdk/`.

A **Module** is a separate plugin library that communicates with the host only through the public plugin ABI (`sdk/include/dsptb/plugin_abi.h`), and that uses no project code other than the headers and tools published in the SDK package.

You may build, run and distribute a Module under licence terms of your choice, including closed-source terms, without those terms having to follow the GPL, if:

1. **Non-commercial use.** If the Module's licence isn't GPL-compatible, the Module isn't sold, licensed for a fee or otherwise used commercially, unless you have the copyright holders' written permission.
2. **No bundling with the ASIO build.** You don't distribute a Module under this permission in one package with a host build that includes the ASIO backend. Users can install the two together themselves.
3. **Notices.** You keep any copyright and licence notices in the SDK files you distribute.

## What this permission doesn't cover

- Modules under a GPL-compatible licence don't need this permission, and condition 1 doesn't apply to them. The GPL lets anyone use them commercially.
- It doesn't cover third-party code, including Steam Audio and the Steinberg ASIO SDK, which keep their own licences.
- It doesn't let you distribute changed copies of the host or the SDK under other terms. Those stay under GPLv3.

## Commercial permission

To ask for permission to use a closed-source Module commercially, open an issue on the project's GitHub repository (https://github.com/inboxes/dsp-toolbox/issues) with the title "Commercial module permission". Leave out anything private: the maintainer will arrange a private channel.
