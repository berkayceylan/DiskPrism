
### Install

1. Download the zip for your Mac: **arm64** for Apple silicon, **x86_64** for Intel.
2. Unzip it and move DiskPrism to your Applications folder.
3. Open DiskPrism. macOS blocks it the first time, because the app is signed ad hoc and not notarized by Apple.
4. In System Settings, open Privacy & Security, scroll down, click **Open Anyway** and confirm. macOS remembers this for the app.

If you would rather not override that check, [build it from source](https://github.com/berkayceylan/DiskPrism#build). An app you build yourself opens normally.

Requires macOS 14 or later. The [release workflow](https://github.com/berkayceylan/DiskPrism/actions/workflows/release.yml) built these files from the tagged commit and tested them in virtual machines: macOS 15, 26 and 27 on Apple silicon, and macOS 15 on Intel. macOS 14 and the graphics drivers of physical Intel Macs are untested.
