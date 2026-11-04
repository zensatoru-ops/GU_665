# Snapdragon 665 / Adreno 610 (Android 10, arm64)

What this branch changes for phones with that chip, why, and how to check it. **None of it has been measured on a device
yet**: it comes from reading the code and from what CLAUDE.md already recorded about Adreno 610 and weak GPUs. Check the
numbers on the phone before saying anything is faster (see "Testing").

Which phones: the SoCs with an Adreno 610, recognised by `ro.board.platform` / `ro.soc.model` (`plat_weak_soc`,
`runtime/platform/android_perf.c`): Snapdragon 665 (SM6125, `trinket`), 662 (SM6115), 460 (SM4250), 680 (SM6225)
(`bengal`). Every other phone behaves as before. `MHGU_SOC_PROFILE=adreno610` / `none` forces it on / off for a test.

## What was found in the code

| Where | Finding |
| --- | --- |
| `nvn/vulkan/device.c` | The pipeline cache was written only by `exit_handler`. Android 10 phones with 3-4 GB end the game from outside (lowmemorykiller, recent apps), so a session's pipelines were lost and compiled again next time: the stutter of a first run, every time. |
| `nvn/vulkan/exec.c` `worker` | The shader prewarm (3 compile threads + 1 translate thread) ran at normal priority on any core. With no ADPF below API 33 the system has no hint, and on four big cores they compete with the game and render threads. |
| `config.c` | The first-run defaults are for a powerful phone (3D picture at 100%, 60 fps, bloom, shadows). Nothing looked at the GPU class. |
| `nvn/vulkan/device.c` `msaa_floor` | Keeps 2x MSAA on Adreno + Vulkan 1.1 below 720 lines; the comment there records an Adreno 610 (1.1.128) that looked dark and flickered with it. A small 3D picture (which this chip needs) is exactly that case. |
| already fine | BC and ASTC textures are native on Adreno (no CPU decoding, unlike Mali); `low_memory_textures` already drops the largest mip under 5 GB; `clears_drawn` already covers Adreno drivers below 1.1.128; ADPF `efficiency_follow` is API 35 only. |

## What changed

1. **Pipeline cache saved while playing** (`device.c`, Android): a thread of its own writes `cache/vk_pipelines.bin` when it
   has grown by 512 KB or more (every 120 s, `MHGU_PCACHE_SAVE_S=<s>`, `0` = only at exit), when the app goes to the
   background (`ui.c`), and when the shader prewarm finishes (`exec.c`). Writes are serialised with the exit handler's.
   Logged as `[vk] pipeline cache saved: ...`.
2. **Prewarm on the little cores** (`android_perf.c` `plat_background_work`, called from `exec.c` `worker`, SD665 class
   only): jobs from the prewarm's low list run on the little cluster at nice 10 (cluster found from `cpu_capacity` or the
   top frequency; nothing is pinned if it cannot be told); jobs a draw waits for run as before on every core. The cache
   saver thread does the same. `MHGU_BG_THREADS=0` turns it off. Trade-off: the first-run prewarm may take longer.
3. **First-run settings** (`config.c` `adreno610_profile`, SD665 class only, only when `mhgurecomp.ini` does not exist):
   RenderScale 50, FPS 30, LowEnd On, Shadows Off, Bloom Off, AdrenoFix Off. These are what the menu's Low-end switch sets
   (with 50% instead of 40%) plus AdrenoFix. All are menu settings. A phone that already has an ini keeps it: delete
   `mhgurecomp.ini` to get them, or use the menu's Low-end mode.
4. **Build** (`ci/`, `.github/workflows/android-arm64.yml`): Linux counterparts of the Windows-only scripts, and a workflow
   that builds the arm64 APK without any game code.

5. **A missing header** (`updater/updater.h`): `runtime/net/ldn.c` and `updater/sha256.c` include it, but neither this
   repository nor upstream (khorne-hub/gu-native) has the file, so the Android runtime did not compile. Rebuilt from
   `sha256.c`'s own code (the `Sha256` struct and three functions; the output matches the FIPS 180-4 test vectors). If the
   author's own `updater.h` turns up, keep that one.
6. **`android_perf.c`'s CPU masks** use raw `sched_*affinity` system calls: the build force-includes `volk.h` ahead of
   the file's `_GNU_SOURCE`, which hid `cpu_set_t` (found by building with the NDK).

Not touched on purpose: `tools/gen.py`, `lifter.py`, `neon.py`, `nso.py`, `gamelib.h`, `hle_symbols.txt`, `cpu.h`, `vfp.h`,
`neon_rt.h` (the `GAMELIB_ID` inputs: changing them makes every player run `build_game.py` again; `ci/build_native.sh`
fails if the id moves), `maxwell/translate.c` and `shader.h` (`SHADER_ID`: it invalidates the shader cache), the
settings' table (`Config`'s order matters).

## Testing

Use a test build (workflow input `dev_build`, or `MHGU_DEV=1`) and `Android/data/com.mhgu.recomp/files/env.txt`:

```
MHGU_VK_STATS=1
```

- Frame rate and frame time: the on-screen counter (Settings, Performance, Show FPS and temperature), not `[vk] 2s waits`.
- One frame's time per pass: `gpuprof` in `control.txt`.
- Stutter: run the same hunt twice and compare; the second run should log no `[shader]` builds that the first one made,
  and `[vk] pipeline cache saved` lines should appear in the first. Kill the game from the recent apps list in the first
  run, start again: the file must be there.
- Heat: `/sys/class/power_supply/battery/current_now` and `voltage_now` while not charging (CLAUDE.md), or the counter's
  temperature, 15 minutes at the same place.
- A/B: `MHGU_BG_THREADS=0`, `MHGU_SOC_PROFILE=none`, `MHGU_PCACHE_SAVE_S=0` each take one change away.
- Log lines to look for at start: `[perf] Adreno 610 class SoC`, `[perf] cores: 4 little, 4 big`, `[config] Adreno 610 class
  phone, first run`.

Things to try if it is still slow, in this order (all in the menu): RenderScale 40 or 33, Toaster mode, Reverb off,
`ShaderPrewarm` Off if the first start is too slow.

## Building the APK

GitHub: push, then Actions, "Android arm64 APK", artifact `MHGU-arm64-v8a-apk` (about 1-1.5 hours the first time, a
fraction with the dependency cache). Without the repository secrets `ANDROID_KEYSTORE_BASE64`,
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD` the APK is signed with a key made for the run: it
installs, but not over an APK signed with another key (uninstall first; saves live in `Android/data`, export them first).

Locally on Linux (NDK 27.0.12077973, CMake, Ninja, JDK 17, Android SDK 35):

```
export ANDROID_NDK_HOME=<ndk>/27.0.12077973
bash ci/fetch_deps.sh && bash ci/build_deps.sh && bash ci/build_native.sh
cd android && ./gradlew assembleRelease     # signs only with keystore.properties + mhgu.keystore
```

The game: the person brings their own `exefs` and `romfs` and runs `tools/build_game.py` (README.md). Nothing here
provides, builds or needs game files.
