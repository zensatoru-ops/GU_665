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

7. **No 2x MSAA floor on an Adreno 610** (`device.c` `adreno610_class`, `msaa_floor`): the floor that keeps 2x MSAA on Adreno +
   Vulkan 1.1 below 720 lines is never applied when the GPU's name is "Adreno (TM) 610", whatever `[Performance] AdrenoFix`
   says. Before, only a first-run file got `AdrenoFix = Off`; a phone whose ini already said `Auto` (the default) paid for 2x
   without choosing it, and the only report of this GPU (above) is that the floor made it dark and flickering. The menu's
   Adreno fix row is hidden there (both values would do the same). `MHGU_ADRENO_FLOOR=1`: the old behaviour, for a test. If
   the 3D picture repeats in a grid at a low resolution with this, that is the bug the floor was for: say so, with the log.
8. **The hot threads stay on the big cores** (`android_perf.c` `hot_thread_to_big_cores`, called from `perf_hint_thread` the
   first time a thread gives a role: the game's update thread, the presenting one, the one translating command lists; SD665
   class only). Affinity only, no priority change, and only when the thread may use at least 3 big cores (three threads share them; a log of one phone showed 2 allowed in one run and 4 in another): else `[perf] thread <tid>: only N big core(s) allowed to it: not pinned`. Logged as `[perf] thread <tid>: 4 big cores`. `MHGU_HOT_PIN=0` turns it off.

9. **The HUD at most 720 lines on this class** (`config.c`, Low-end mode On): the Low-end rule keeps the HUD at the screen's size up
   to twice the 3D picture's height; at 40% of a 1080-line screen that is 864 lines, 4 times the 3D's pixels, alpha blended
   and copied to the window. Capped at 720 (what 25 to 36% already got). Toaster keeps its 540. `MHGU_HUD_LINES=<lines>`
   sets it for a test (360 and up), e.g. 540 or 1080.
10. **Light pass barriers, an experiment, off by default** (`exec.c` `pass_begin_barrier`, `MHGU_VK_LITEBARRIER=1`): the barrier
   before every render pass is all stages to all stages, which on a tiler keeps the next pass's geometry work from
   starting until the last pass has finished drawing. The light version is two barriers: transfers, compute and host
   writes visible to every stage as before, and the passes' attachment writes visible to the later fragment stages, tests,
   output, transfers and compute only. It leaves out a vertex shader reading what an earlier pass drew or a fragment shader
   stored. Whether this driver gains from it is not known: A/B it, and watch the picture for flicker or missing parts.
11. **Anisotropic filtering capped at 2x** (`textures.c` `aniso_cap`, SD665 class by default): the game's samplers ask for up to 16
    taps a sample, which Low-end mode switches off altogether; this class is fragment-bound (a phone's own profile: two
    936x432 scene passes were 27 of 40 ms), and the 3D picture is small and stretched. `MHGU_MAX_ANISO=<1..16>` sets it on any
    device (16 is the game's own). `MHGU_MIP_NEAREST=1` (a test, off by default) makes samplers pick the nearest mip level, as
    Low-end mode does. The shadow atlas clear conversion tried before was taken out: it never matched on that phone (its driver
    is 1.1.87, where every clear is a draw anyway) and the shadow pass is 0.6 to 3.6 ms there.
12. **A GPU profile of its own** (`exec.c` `gpuprof_auto`, SD665 class by default): `gpuprof` takes one frame's time per pass, but needs
    `control.txt` and a test build. On this class one frame is profiled 40 s after the first frame and then every 60 s, 12 in all
    (a hitch of one frame each), logged as `[gpuprof] ...` in `log.txt` (each pass labelled with the fragment program of its first draw, `1st fs <hash>`) with a header line (3D size, HUD size, FPS limit,
    MSAA, shadows, bloom, Low-end). `MHGU_GPUPROF_AUTO=<seconds>` sets the period on any device (test builds), 0 never.

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
- A/B: `MHGU_BG_THREADS=0`, `MHGU_SOC_PROFILE=none`, `MHGU_PCACHE_SAVE_S=0`, `MHGU_HOT_PIN=0`, `MHGU_ADRENO_FLOOR=1`, `MHGU_HUD_LINES=<n>`, `MHGU_VK_LITEBARRIER=1`, `MHGU_SHADOW_CLEARDRAW=0` each change
  one change away. Change one at a time, same place in the same hunt, read the on-screen counter, and note the
  `[vk] MSAA` line of `log.txt` (it says whether 2x is on).
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
