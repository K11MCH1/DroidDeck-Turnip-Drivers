# DroidDeck Turnip Drivers

A source fork of [Mesa](https://gitlab.freedesktop.org/mesa/mesa), with separately editable Banner patching and DroidDeck Linux packaging stages. Turnip is Mesa's Freedreno Vulkan driver and is developed on Mesa `main`.

The entire Mesa `main` history is imported. `upstream-main` records the initial unmodified upstream snapshot; `main` adds the DroidDeck pipeline. Each patch run fetches upstream `main` again, or an explicitly selected upstream commit, so builds do not depend on the age of that snapshot. Mesa's original README is [README.rst](README.rst).

## 1. Patch and review Turnip

Run **Actions → 1 - Patch Turnip → Run workflow** from `main`. Choose `mesa_ref` (`main` or a full upstream commit SHA) and a GPU variant:

| Variant | Banner recipe |
| --- | --- |
| `standard` | A6xx/A7xx baseline; checks that Banner's common KGSL fixes are already upstream. |
| `a8xx` | WinNative mesh/wave32/Adreno fixes, gen8 patch stack, and shared memory adjustment. Experimental. |
| `710-720-test` | UBWC gralloc detection and captured A722 registers. Experimental. |
| `8g2-oneui` | A740 UBWC hint for firmware whose system driver sets the same hint. |

This workflow saves a new branch named `codex/turnip/<variant>/<run-id>-<attempt>`. It checks that the Linux adjustments also apply, but does not compile or package. Its summary gives the branch and exact commit. Review the diff, or add your own Mesa source commits to that branch, before proceeding. Push and pull request runs validate all four variants without publishing branches.

The patch order is in [droiddeck/patches.json](droiddeck/patches.json). Add future `.patch` or Python patch scripts under `droiddeck/patches/`, and list them in `extra` for all variants, a specific `banner` variant, or `linux` for stage 2. Paths are relative to `droiddeck/`; scripts run with the Mesa tree as their working directory. A patch error stops the workflow. Imported patches are pinned and vendored; nothing silently downloads a newer patch set during a build.

The A8xx series is rebased as `droiddeck/patches/a8xx-mesa-main.patch`; the original patches remain in `vendor/banners`. Rebase details are recorded in `droiddeck/NOTICE`.

## 2. Adapt, build, and package for DroidDeck Linux

Run **Actions → 2 - Build DroidDeck Linux ZIP → Run workflow** from `main`. Set `source_ref` to the reviewed stage 1 branch or exact commit, and set a package version such as `0.1.0`.

This stage checks the source provenance, applies the independently editable Linux series, and cross-compiles a glibc AArch64 driver with KGSL + MSM and Wayland + X11 WSI. It uses the Ubuntu 24.04 ARM64 sysroot recipe from [Droid-Deck/Drivers](https://github.com/Droid-Deck/Drivers), with a glibc requirement no newer than 2.39. Linux fixes cover KGSL dma-buf device numbers, safe timestamp capability advertising, PWR_MAX constraints, process heap usage, and the A8xx LRZ flush reset. Adjust the Linux entries or [build script](droiddeck/build_linux.sh) on `main`, then rerun stage 2 against the same reviewed source.

Download the completed run's artifact and extract it. It contains:

- `DroidDeck-Turnip-v<version>-<variant>-Linux-<commit>.zip`
- The exact patched source and build recipe in `*-source.tar.gz`
- SHA-256 checksum files for both archives

The inner driver ZIP contains `libvulkan_freedreno.so`, Linux `meta.json`, an ICD manifest, a build report, notices, and licenses. Import that ZIP through DroidDeck's Linux Vulkan driver importer. It is a Linux-only package; Android display driver pairing remains a separate choice. These workflows do not publish releases or update the existing `Droid-Deck/Drivers-CI` catalog.

Packaging checks the AArch64 ELF ABI, glibc dependencies, required WSI symbols and power patch marker, minimum glibc, source provenance, dependency availability in the sysroot, and ZIP integrity. Successful CI confirms compilation and package checks; GPU behavior still needs testing on a phone.

## Upstreams and licenses

[droiddeck/sources.json](droiddeck/sources.json) records the imported commits from [The412Banner/Banners-Turnip](https://github.com/The412Banner/Banners-Turnip) and [Droid-Deck/Drivers](https://github.com/Droid-Deck/Drivers). Vendor files preserve their original patch authors, comments, and GPL-3.0 licenses. See [droiddeck/NOTICE](droiddeck/NOTICE) for adaptation details. Mesa's licenses remain in [docs/license.rst](docs/license.rst) and `licenses/`. The build and packaging code in `droiddeck/` is distributed under GPL-3.0, in line with the imported recipe; the patched driver is distributed with corresponding source and original notices.

Local control tests: `python3 -m unittest discover -s droiddeck/tests -v`. The actual cross build requires Linux; the two Actions workflows provision it.
