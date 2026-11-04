# MHGU for Android: project notes

A native Android port of Monster Hunter Generations Ultimate 1.4.0, built on MHGURecomp: the game's 32-bit ARM code is translated
to C, and a host runtime reimplements the Switch's services and its NVN graphics API on Vulkan.

This file has two uses: helping someone set the game up, and continuing the project.

## Rules

- This project never provides game files. Don't obtain, link to or explain how to get ROMs or keys, and don't explain how to dump a
  console. The person brings their own `exefs` and `romfs`.
- Never commit or publish `exefs`, `romfs`, `generated/`, `libmhgugame.so`, `libmhgugame.id`, saves or signing keys. Never put game
  code in an APK or a release: `libmhgugame.so` is made from the game, so each person makes their own with `tools/build_game.py`.
- Keep `LICENSE` and `THIRD_PARTY_NOTICES`. The MIT license requires them.
- Don't lower someone's graphics or audio settings without asking.
- Any new menu, notice or touch control must match the game's style: bronze, gold and wine-dark, with no default Android look.

## Helping someone set it up

Follow `README.md`. In short:

1. Check that a folder holds `exefs/main` and a `romfs` with 35,531 files, from version 1.4.0.
2. Run `python tools/build_game.py <that folder>`. It needs Python 3 on Windows or Linux, installs `lz4`, and downloads NDK r27 to
   `~/.mhgu-tool` if there is none (`--ndk <path>` uses another). It leaves `libmhgugame.so` and `libmhgugame.id` in the folder.
3. Copy the folder to the device, anywhere except `Android/data`. With adb: `adb push <folder> /sdcard/MHGU`.
4. Install the APK from Releases. For Samsung's Game Booster: `adb install -r -i com.mhgu.recomp <apk>`.
5. The person opens the app, grants file access and chooses the folder.

To diagnose a device: `adb logcat -s MHGU`, or `Android/data/com.mhgu.recomp/files/log.txt`.

## Layout

- `runtime/`: the host runtime. `touch.cpp` touch controls and editor, `settings_panel.cpp` the menu, `nvn/vulkan/` the renderer,
  `hle/` game hooks, `platform/android*.c` Android, `config.c` settings and first-run defaults.
- `tools/`: `gen.py`, `lifter.py`, `neon.py`, `nso.py` translate the game; `build_game.py` is the person's tool.
- `android/`: the app. `GameFolderActivity` is the entry screen, `GameActivity` runs the game, `SaveImportActivity` imports a save
  (it is only applied at the next start, before the game runs, so the game can't save over it).
- `native/`: CMake and `build_native.sh`. `deps/`: scripts for SDL3, shaderc and FFmpeg.

## Building

Windows, in Git Bash, with the Android SDK, NDK 27.0.12077973, CMake 3.22.1 and Android Studio's JBR.

1. Put the sources in `deps/src/`: `SDL3-3.4.18`, `ffmpeg` (9.0.2) and `shaderc` (2026.4, with glslang 16.6.0 and SPIRV-Tools and
   SPIRV-Headers 1.4.363.0 in its `third_party`). Then run `bash deps/build_deps.sh arm64-v8a` and `bash deps/build_ffmpeg.sh arm64-v8a`.
2. `bash native/build_native.sh arm64-v8a` builds the runtime. With `MHGU_DEV=1` in front, a test build.
3. `cd android && ./gradlew assembleRelease`. It signs only if `android/keystore.properties` and `mhgu.keystore` exist.

The public APK leaves `libmhgugame.so` out. `-Pwithgame` keeps it for your own tests: never publish that APK.

`GAMELIB_ID` is a hash of the generator and of the headers listed in `tools/build_game.py`, `runtime/gamelib.h` and
`hle_symbols.txt` included. If any of them changes, every player must run the tool again, and the app says so. `runtime/*.c` can change
without that: new hooks use `fnhook_set`, new declarations go in another header. Check the id before releasing.

## Testing on a device

A test build (`MHGU_DEV=1`) reads two files in `Android/data/com.mhgu.recomp/files`; a public build reads neither:

- `env.txt`: `NAME=value` lines. `MHGU_QUICKHUNT=101` goes straight into a quest, `MHGU_VK_STATS=1` logs statistics,
  `MHGU_TOUCH_LOG=1` logs fingers. `MHGU_HOME=<folder>` moves the settings and the log, but not the saves: those are always
  `Android/data/com.mhgu.recomp/files/MHGURecomp/save`, so copy them first. `MHGU_CAMLOG=1` logs the hunt camera and the monsters,
  `MHGU_LOG_MOVES=1` the hunter's actions, `MHGU_LOG_NET=1` the network, `MHGU_PAD_DEBUG=1` what SDL reads from a controller,
  `MHGU_FORCE_NOLSE=1` the adapted game code on any CPU, `MHGU_BCN_LOG=1` the time big BC textures take.
- `control.txt`: appended commands, read while the game runs. `A 8`, `DOWN 8`, `type <text>`, `textkey enter`,
  `memsave <hex address> <hex size> <file>`, `peek`, `poke`, `gpuprof` for one frame's time per render pass, `hudtrace` for one
  frame's HUD layers, and `crash` for the error screen. Create `control.txt` from the shell with `chmod 666`, or the app can't read it.

What the app writes to `cache/` under `Android/data` can't be deleted over adb, so a test folder under `MHGU_HOME` stays until the app removes it.

Empty `control.txt` before starting: old commands replay. `3D resolution` needs a restart: until then the menu says so and the old
one is still running.

**Read frame rates from the on-screen counter.** `[vk] 2s waits` counts submits, and a frame takes two of them.

Android killing the game shows in `adb logcat` as `lowmemorykiller: Kill 'com.mhgu.recomp'`.

## What real devices taught us

- Mali has no BC textures, so they are decoded on the CPU and take 4 to 8 times the memory. BC4 and BC5 go to R8 and RG8, and under
  5 GB of RAM textures with mipmaps lose their largest level (`mip_skip`, `nvn/vulkan/textures.c`). Without this a 4 GB phone was killed.
- On a Mali, more than half of a hunt's frame is some 30 full-screen passes, about 0.15 ms each whatever their size. Low-end mode skips
  the bloom and depth of field chain (`lowend_skipped`, `nvn/vulkan/exec.c`), and the two draws that add it to the picture must go too.
  A render target that is never drawn gets uploaded from guest memory when sampled: that garbage, added over the picture, is coloured noise.
  So skipped targets are marked with `renderer_mark_gpu_owned`. Below 100 % it also keeps the HUD at the screen's size through
  `fsr.c`'s plain scaling path, without FSR. `MHGU_VK11=1` was tested with it, only by forcing that path on a newer driver.
- FXAA leaves a 2 px green line at the edges on Mali and costs about a fifth of the frame rate, so it is off by default on Android.
  The cause is unknown.
- A Vulkan 1.1 driver (seen: Mali-G57 MC2, 1.1.177) has no timeline semaphores: each submission gets a fence and one thread waits for them
  (`nvn/vulkan/exec.c`). It also gets SPIR-V 1.3 and no FSR. `MHGU_VK11=1` in `env.txt` forces that path on a newer driver.
- That same Mali has no `shaderInt64`: with `maxwell_no_int64` the shaders do their 64-bit math in 32 bits (`nvn/maxwell/translate.c`).
  `MHGU_VK_NO_INT64=1` forces it. A forced path only shows that the path works: a real old driver can still differ.
- A driver can make more window images than it was asked for. Never assume the count: `VK_SC_MAX`, `nvn/vulkan/device.c`.
  `MHGU_VK_IMAGES=<n>` asks for that many.
- Samsung ignores the manifest's game category. Only the one set by the installer counts: see `GameFolderActivity.declareGame`.
- On Android, a `SUBOPTIMAL` result from present must not recreate the swapchain: doing so cost two thirds of the frame rate.
- The hunter has one class per weapon: never compare against one vtable. Virtual 94 tells a hunter of any weapon and whether the
  weapon is out (`runtime/hle/weapon.c`). The `sPlayer` slot can hold another object that points to the hunter.
- Don't trust an offset measured in one session: check it in another one.
- The game code is built for ARMv8.2, and its 2564 `casal` / `swpal` are illegal on an ARMv8.0 CPU (Cortex-A53, A73 alone): SIGILL at the
  first critical section. `runtime/gamelib_lse.c` writes a private copy of the library where each jumps to an exclusive load / store
  sequence in an added segment. `MHGU_FORCE_NOLSE=1` tests it; no device here lacks LSE, so only logs of those phones confirm it.
- A LAN search has to wait: on Wi-Fi a broadcast waits at the router for a sleeping device's next beacon, and 400 ms found nothing. It
  now listens 1.5 s and asks every 0.5 s, on each interface's own broadcast address too, with a Wi-Fi multicast lock held.
- `touch_hud_menu` decides the touch layout from the HUD's layers. Layer 97 is the game's menus, but aiming a heavy bowgun draws there too
  (`weapon_aiming`), and the multiplayer menu and the hub form are on layer 100, as stretched panels, which the HUD's parts there are not.
- The hunter: heading at +0x14a0, in the camera's units, (x, z) = (sin, cos); written, the hunter turns at once. Place at +0xa0, zone at
  +0x1054, which monsters have too. The classes are `uPlayerQuest00..15` in weapon order, no 05: 04 and 06 the bowguns, 10 the bow.
- In towns the game's camera does not turn, with any stick: it isn't a controller problem.
- Heat: `GameActivity.pickRefreshRate` takes the lowest screen mode that fits the FPS limit, never under 60, because forcing the fastest one
  shows nothing more. `efficiency_follow` (`android_perf.c`, API 35) lets the system use efficient cores while frames use under
  half the target, and stops at once when they near it. `MHGU_ADPF_EFFICIENT=0` turns it off. A phone that isn't charging gives real watts:
  `/sys/class/power_supply/battery/current_now` and `voltage_now`. Measured in a hunt, standing: the FPS limit is what counts. Moto g06 at 60 FPS
  3.37 W, at 30 FPS 2.70 W; S25 Ultra at 120 (106 real) 3.48 W, at 60 2.33 to 2.73 W. 60 Hz against a faster screen showed no gain beyond the noise on either.
  A Galaxy denies those files: use `dumpsys battery` (Charge counter, in steps of 4.9 mAh); its `dumpsys thermalservice` repeats a stale value.
- `MHGU_FS_NEW=1` logs each fragment program the first time it draws; `MHGU_FS_WATCH=<hash>` saves the frames where one draws. A hit draws
  `cc207b43683737c8`, a 17-sample radial blur. The device's `shots` folder keeps old files: go by the names in the log, not by what's there.
- Started with the phone locked or upright, the display says portrait: the 3D picture came out 720x576 with black at the sides. `config.c` now
  swaps the two. Start test runs with the screen on and unlocked anyway: `am start` on a locked phone doesn't show the game.
- The game's Bow Controls (`sGameControl` +0x6c): Type 1, X draws and R enters the aiming mode; Type 2 the other way round. Read `weapon_bow_type`, or a
  bow's layout presses the wrong button. Letting go of a button that doesn't draw does nothing, which looks like a broken bow.
- `HN_WeaponControlsMsg_eng.gmd`, `HN_HunterTipsMsg_eng.gmd` and `StartMenuMsg_eng.gmd` in `romfs/nativeNX/eng/table` are the game's own manual: read them
  before guessing what a button does. L + X and L + B change the ammo, and the game takes L only after about 300 ms.
- The aim's pitch: hunter +0x14b8 (a float, -100 down to 100 up) and +0x2714 (16-bit units, 45 degrees up, 30 down). A shot that isn't aimed ignores them, so a
  locked shot from a bowgun goes through quick aim (`touch.cpp`, `assist_*`). With a gun aimed or a bow drawn the hunt camera's handler does not run: per-update
  work goes in the item bar's hook (`hle/itembar.c`).
- A menu can't be told from a window over the hunt: no flag in the hunter or the game's globals changes, and the hunt camera runs under menus too. `game_menu`
  treats a window as a menu when the hunter isn't being played or a button that opens one was pressed just before.
- The renderer's `vk*` are volk's pointers (`third_party/volk`, force-included by `native/CMakeLists.txt`), filled from `runtime/platform/android_gpu.c`: the
  system's `libvulkan.so`, or the player's driver through `third_party/libadrenotools`. `volkLoadDevice` is not used, so calls go through the same loader
  entry points as before. A trial file sends a driver that never drew back to the system's. The hook libraries must stay extracted (`extractNativeLibs`).
- Custom drivers, tested on an Adreno 830 with a marked copy of its own library, not yet with Turnip. A library that can't be opened doesn't stop the game:
  `hook_impl` falls back to the system's, and so does one named `vulkan.adreno.so` (the system's, already loaded); `gpu_driver_check` notices
  both (below). `AdrenoVK: Driver Path` in logcat tells which one runs. A driver that dies before drawing goes through the trial file.
- Adreno 630 on Android 10 (driver 1.1.87): SIGSEGV in `vulkan.sdm845.so+1dd318`, from `vkCmdClearAttachments`. Drivers under 1.1.128 clear with draws
  (`clears_drawn`, `MHGU_CLEAR_FORCE=1` tests it anywhere). Adreno with Vulkan 1.1: below 720 lines and without MSAA the 3D picture repeats in a grid, so
  `msaa_floor` keeps 2x unless `[Performance] AdrenoFix = Off` (an Adreno 610 on 1.1.128 looked dark and flickered with it). Its cause is unknown.
- Snapdragon 665 / Adreno 610 (Android 10): see `docs/SD665_ADRENO610.md`. The pipeline cache is now written while playing, not only at exit
  (`device.c` saver thread: Android ends a 3 to 4 GB phone's game from outside), the prewarm's workers go to the little cores on that SoC class
  (`plat_background_work`), and its first-run settings are the Low-end ones (`adreno610_profile`, `config.c`). None of it is measured on a device yet:
  read the numbers off the on-screen counter before saying it is faster.
- A romfs under 12.3 GB with all 35,531 files is an incomplete copy: a black picture, then `no function at 0`. The log warns.
- `Config` gets its defaults by position: `CONFIG_DEFAULT_VALUES` in `config.c`. A new field in `config.h` needs its value there, in its
  place, or every later field moves (two added once turned most touch buttons off).
- The shadow maps are one 1024x3072 depth atlas (three cascades), the first pass of a hunt's frame: 47 draws, 2.8 ms of a Moto g06's 18.6.
  The game clears it with three full-size draws, not a clear. `[Performance] Shadows = Off` clears it once and skips its pass (`shadow_skipped`).
- The chat's log is a layer 97 window at the bottom left (x 30..605, y 547..708 of the 1280x720 canvas) that stays seconds after a line: it
  must not count as a menu (`touch_hud_menu`), or the touch controls stay in the menu layout and the hunter can't walk.
- A model's skeleton: +0x490 the joint count, +0x494 the array, 0xa0 bytes each, world position at +0x40 (w 1.0), parent at +0x91 (0xff the
  root). The root is the pelvis. Locked shots aim there (`mousecam.c body_center`).
- `camera_fovy` compares the camera's aspect with the screen's with a tolerance: a small 3D picture's rounding made them differ at 33%.
- DLC: the game asks `aoc:u` only from its main menu's Download, which needs its online services and stops with an error without them,
  even with `MHGU_FAKE_NET=1`. `services/dlc.c`, the RomFS images and the importer are in place; the menu row shows with `MHGU_DLC=1`.
- Cheats, mods and DLC are read from the game folder's `cheats/`, `mods/` and `dlc/` too (a file manager reaches it, not `Android/data`).
  `ImportActivity` copies there; imported cheats start off (`cheats/_off.txt` in the app's folder).
- Every minute the log has a `[mem]` line: memory map entries, resident memory and VMA's blocks. Games on PowerVR closed after 5 to 15
  minutes out of map entries; there VMA is told Vulkan 1.0 so images share its blocks (`MHGU_VMA_DEDICATED=1`: the old way).
- A player's driver is checked once the device is made (`gpu_driver_check`): no library from its folder loaded means the system's runs,
  and the choice moves to `failed`. `DriverActivity` renames a library named as the system's (`vulkan.adreno.so`) to `vulkan.player.so`.
- The Khronos validation layer: put `libVkLayer_khronos_validation.so` (Android binaries of Vulkan-ValidationLayers) in `jniLibs` for a
  test build only, and `MHGU_VK_VALIDATION=1`. Never in a public APK.
- `minSdk` is 28 (Android 9). `./gradlew lintRelease` lists calls newer than that (`NewApi`); `native/build_native.sh` builds for API 28.
- Built for API 28, every `__thread` variable is emulated TLS: a call to `__emutls_get_address` and a `pthread_getspecific` per access.
  `rt_dispatch`'s cache was one, on every indirect jump of the game (a tenth of a game thread on a Moto g06): it is now per `Cpu`, through a
  pointer in `Cpu._pad` (`dispatch.c`, bytes the game's code never touches; `cpu.h` itself must not change). Keep `__thread` out of hot paths.
- Profiling: a test APK with `<profileable android:shell="true"/>` (put it in, build, take it out), NDK `simpleperf record --app` on the
  device, `binary_cache_builder.py -lib native/build/arm64-v8a`. `--call-graph fp` stops at the Mali driver; `dwarf` gets through. The
  stripped Mali library shows its own code as `stdlibp_neon_f32x16_to_f16x16`: that is the driver recording draws, not a conversion.
- The audio renderer had 70 effect slots of 1.7 KB, searched for every order number on every mix: over half the audio thread. `fx_order`
  lists them once after an update (`services/audio.c`). `getenv` walks the whole environment: never call it per frame, cache it.
- `gpuprof` shows the window image's pass (the game's picture copied in, the overlay) as its last line. The game's own passes all load
  their attachments; one whose first commands clear an attachment whole begins with `LOAD_OP_CLEAR` and those values instead
  (`noload_clears`, `render_pass`), and the clears it already did are skipped (`pass_skip_clear`). Not `DONT_CARE` plus the clears: an
  Adreno 830 then drew the scene black but for some tiles. Anbernic (Mali-G57) GPU frame -6 %, Adreno 830 -0.7 %. `MHGU_VK_NOLOAD=0`: off.
- Descriptor sets: `stage_set` reuses one written earlier in the same submission when every write is the same (about a quarter of the
  sets), and draw skips binding the same two again (`SS_SETS`; any other graphics bind forgets them). Zeros, the identity rescale table,
  the alpha reference and each stage's driver constants are copied once per submission (`shared_*`, `zero_ubo`), or every set would be
  new; a storage buffer of zeros is shared only by shaders without `uses_gmem_store`. `MHGU_VK_SETREUSE=0`, `MHGU_VK_SHARED=0`: off.
- A Galaxy (Samsung) refuses every simpleperf event for apps (`cpu-clock`, `task-clock`, `cpu-cycles`): profile the CPU on another
  phone. Its frame is CPU-bound: the GPU span in `gpuprof` includes the gaps where the GPU waits for the render thread.
- `[Performance] LowEnd` is Off, On or Toaster (2). Everything that tests `low_end` treats Toaster as On; Toaster adds a HUD at most 540 lines
  tall and smaller BC textures, and the menu sets 25 %, 30 FPS, Shadows, Bloom and Reverb off. `[Performance] Bloom` (the bloom and depth
  of field chain, `lowend_skipped`; not with real FSR: Low-end mode's plain scaling sets `g_fsr_on` too, so test `g_config.fsr`) and
  `[Audio] Reverb` are switches for everyone. A file with LowEnd on and no Bloom key gets Bloom off, as before.
- Test runs can override the device's settings without touching its ini: `MHGU_LOWEND=0|1|2`, `MHGU_SCALE`, `MHGU_SHADOWS`, `MHGU_BLOOM`,
  `MHGU_FPS_LIMIT`, `MHGU_NO_REVERB=1`. A phone on its charger reads charging current: unplug it to measure watts. At 30 FPS the GPU runs at
  lower clocks, so `gpuprof` times are longer: compare frame rates or watts, not milliseconds, across FPS limits.
- Internet hubs (`net/ldn.c`, the `server/` hub server): the Network page shows Hub server, Matchmaking key and Check hub server on
  Android too; empty Server is None (`LDN_DEFAULT_SERVER` is empty: the project runs no server). Players type any server they want.
- The touch menu layout can show ZL and ZR tabs beside L and R (`C_MZL`, `C_MZR`), each with `[Touch] ShowMenuZL` / `ShowMenuZR`, off by
  default (Buttons on the screen).
- Bugs that only show on some devices are common here. Test on a real phone, and on a device without Snapdragon.
