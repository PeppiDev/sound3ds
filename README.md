# Sound3DS 🎵

**English** | [Español](README.es.md)

### A good-looking music player for Nintendo 3DS / 2DS (optimized for Old 3DS)

**Sound3DS** is an MP3 player and streaming client for Nintendo 3DS/2DS. The performance goal is a smooth interface on Old 3DS/2DS; performance and audio output must be validated on real hardware.

## 📲 Install with a QR code
<p align="center">
  <a href="https://github.com/PeppiDev/sound3ds/releases/latest/download/sound3ds.cia">
    <img src="docs/qr-cia.png" alt="QR code to install sound3ds.cia with FBI" width="220">
  </a>
</p>

On your console, open **FBI → Remote Install → Scan QR Code** and scan the code: it downloads and installs the `.cia` of the latest release. When it finishes, Sound3DS shows up on the HOME menu. Requires Wi-Fi and a console with CFW (Luma3DS).

<p align="center">
  <img src="docs/screenshot-soundcloud.png" alt="Sound3DS playing pyro (Slowed) By ZERTAL from SoundCloud" width="420">
  <br>
  <em>Streaming "pyro (Slowed) By ZERTAL" from SoundCloud</em>
</p>

---

## 🌟 Main features

1. **SoundCloud search and streaming**:
   - Search through the v2 API and full tracks over HLS + AAC (fMP4 segments), or progressive MP3 when available.
   - AAC decoded on the console itself with Helix AAC (fixed point, optimized for ARM); 30-second previews are marked "30s".
   - Streaming URL resolution and provider redirects.
   - Touch search using the system's software keyboard (`swkbd`).
2. **Multithreaded architecture**:
   - Audio thread (decoding and feeding the hardware) with a slightly higher priority than the main thread.
   - Search, cover art and streaming run on their own network threads; the UI keeps animating while they wait, and every transfer can be cancelled.
   - All threads use the application's default core (Core 0); `APT_SetAppCpuTimeLimit(30)` is enabled for future tests on Core 1.
3. **Audio pipeline with two backends**:
   - MP3 decoding with `minimp3` and AAC with Helix AAC, to 16-bit PCM at the track's sample rate.
   - **NDSP** (preferred): a 4-buffer queue with no gaps between blocks and real pause. Requires `sdmc:/3ds/dspfirm.cdc`, which you can dump with [DSP1](https://github.com/zoogie/DSP1).
   - **CSND** (automatic fallback if `dspfirm.cdc` is missing): a looping PCM ring per channel, refilled behind the playback head.
   - The top screen shows which backend is active.
4. **Reactive waveform / FFT visualizer**:
   - 128-point integer FFT with sine/cosine tables precomputed in ROM (<0.04 ms per frame).
   - 16 logarithmic frequency bands with smooth peak decay.
5. **SoundCloud account and playlists**:
   - **Playlist** search (Songs / Playlists buttons next to the search bar): playlists open like an album and download in full, with a queue that resumes if it's interrupted.
   - With your account connected, **Likes**, **Playlists** (your own and saved ones) and **Feed** tabs (reposts are marked).
   - The account is connected from your PC with [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share): your password never goes through the app.
6. **Fast local library**: it's read in the background and kept in an index on the SD card, so after the first time it opens instantly even with hundreds of songs. An **All songs** album at the start of the cover flow holds your whole library.
7. **Shuffle, repeat and headphone controls**: shuffle and repeat (off / list / song) buttons, and the button on wired headphones with a mic works like on a phone (1 click pause, 2 next, 3 previous), even with the lid closed.
8. **Extensible theme engine**:
   - **Modern Glow**: a dark cyberpunk/minimalist look with violet/neon accents and continuous antialiased bars.
   - **Y2K Classic Tunes**: 2000s player style (classic iTunes / Winamp), with brushed aluminum background, a retro green phosphor LCD display and a spectrum analyzer with discrete LED segments (green/yellow/red).

---

## 🎮 Controls

### Main menu
- **Top screen**: the mascot, a cube with little square eyes that reacts to what you do (and falls asleep if you leave it alone).
- **Bottom screen**: pick **SoundCloud** or **Local** with the D-pad + (A) or by tapping the card. The palette button changes the theme, and the "now playing" pill takes you to what's playing.

### Player (SoundCloud and Local)
- **Top screen**: cover art, title, artist, time, equalizer and the small mascot.
- **Search bar**: searches in SoundCloud; filters your songs in Local (empty = all).
- **List**: SoundCloud results 50 at a time (account lists load more when you reach the end) and up to 2000 songs in Local, in pages of 5.
- **Account tabs** (with your account connected): **Search · Likes · Playlists · Feed**, below the search bar. Tapping the active tab or the title reloads the list.
- **Progress bar**: drag and release to seek.
- **Transport**: shuffle, previous, play/pause, next and repeat (off → whole list → this song). Shuffle mixes the list you're playing from (an album, a playlist, your Likes…) without repeating songs until all of them have played.
- **Bottom row**: theme palette, minigame, download (SoundCloud only), pager and home button to go back to the menu.
- **In Local**, the top screen is the album cover flow and the list shows the songs of the focused album. The first album, **All songs**, has your whole library (or whatever matches the filter).

### Buttons
- **D-pad up / down**: move through songs (or cards in the menu)
- **D-pad left / right**: previous / next page
- **(A)**: play the focused song, or pause/resume the one playing
- **(B)**: back to the menu
- **(L) / (R)**: previous / next track
- **(ZL) / (ZR)** (New 3DS and New 2DS XL): previous / next account tab
- **(Y)**: download the focused song (SoundCloud)
- **(SELECT)**: switch visual theme
- **(START)**: exit
- **Volume**: the console's volume slider

### Settings
The **gear** button (in the menu next to the palette, and in the player next to the download button) opens the settings:
- **Language**: English (default) or Spanish.
- **RGB LED while playing**: the notification LED (the StreetPass one) glows in rainbow colors while music is playing. Works on every model of the 3DS family.
- **L/R with the lid closed**: **Tap** (1 tap pauses, 2 taps change track), **Hold** (hold L or R for 1 second to change track, L+R together to pause, so they don't get pressed in your pocket) or **Off**.
- **SoundCloud account**: the connected account and a **Disconnect** button (deletes the session from the SD card).
- **Receive from PC**: opens the camera to scan a Sound3DS Share QR code (songs or your account).

Navigate by touch or with the D-pad and **(A)**; **(B)** or the **X** at the top closes it. Changes are saved to `sdmc:/3ds/sound3ds/settings.ini`.

### Minigame
The **gamepad** button (next to the palette) switches the top screen between the player and *Cubo vs. Ruido* (Cube vs. Noise), without stopping the music:
- **Circle Pad**: move the cube and dodge the noise blocks (more of them spawn with the bass)
- **(X)**: quick dash; you can't be hit while it lasts
- **(Y)**: shockwave that clears nearby noise (charged by collecting 4 notes)

You have three lives; when you lose, your score and high score are shown and a new round starts on its own. If you hide it, the game freezes until you open it again.

### Sleep mode
If you close the lid (or flip the 2DS sleep switch) while music is playing, the app keeps playing with the screens off. Sound comes out of the speakers, or the headphones if they're plugged in. Controls: by default a single tap of **(L)** or **(R)** pauses/resumes, a double tap of **(L)** goes to the previous track and a double tap of **(R)** to the next one (you can switch to *Hold* or turn them off in Settings); when you open it, everything goes back to normal. With no music playing, the console sleeps as usual.

### Headphone button
Wired headphones with a mic and an inline button (the usual phone kind) work like on a phone, with the lid open or closed:
- **1 click**: pause / resume
- **2 clicks**: next track
- **3 clicks**: previous track

### Local music and cover flow
Drop your MP3s into `sdmc:/music/` (or send them over Wi-Fi with [Sound3DS Share](#-sound3ds-share-connect-your-account-and-send-music-from-your-pc)), no folder organizing needed: Local mode groups them by **album** based on their tags. Before copying them, tag them with [Mp3tag](https://www.mp3tag.de/) or similar:
- **Title**, **Artist**, **Album** and **Track number** (the order within the album)
- **Album artist** if the album has several artists
- Embedded **cover art** (JPEG or PNG): this is what the cover flow shows

The top screen shows albums like the iPod cover flow and the bottom screen lists the songs of the focused album. **D-pad left/right** (or the Circle Pad) changes album; **up/down** and **(A)** pick the song. Songs without an album go to "No album" and, without tags, the title and artist are taken from the file name `Artist - Title.mp3`.

### Downloading from SoundCloud
In SoundCloud, focus a song and tap the **arrow** button (or press **(Y)**): it downloads to `sdmc:/music/SoundCloud/` in the background, with its metadata and cover art, and shows up in the **SoundCloud** album in Local mode. Downloaded songs get a ✓. Tracks that SoundCloud only offers as a 30-second preview can't be downloaded.

An open **playlist** has a **Download** button: it downloads every song to `sdmc:/music/SoundCloud/<playlist>/` and shows up in Local as an album with its name and cover art. The queue is saved on the SD card: if you quit, go to HOME, the console turns off or the Wi-Fi drops, Sound3DS asks to resume it next time you open it.

### SoundCloud account
To see your **likes, playlists and feed** on the console:

1. On your PC open [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share) and click **Connect SoundCloud**. SoundCloud's official sign-in page opens: log in as usual (Google or Apple work too).
2. Share shows a QR code. On the console open **Settings → Receive from PC** and scan it.
3. Done: the **Likes · Playlists · Feed** tabs and your name show up. The session renews itself.

**Important:** Sound3DS stores your SoundCloud session on the SD card. Anyone who copies the SD card could use your account: don't share the card, and use **Disconnect** in Settings if you lend or sell it. The app only **reads** from your account (it doesn't post, buy or change anything). This is not an official SoundCloud integration: it uses the same API as their website and could stop working if SoundCloud changes it. If the session expires, just connect it again.

---

## 📲 Sound3DS Share: connect your account and send music from your PC
<p align="center">
  <a href="https://github.com/PeppiDev/sound3ds-share">
    <img src="https://raw.githubusercontent.com/PeppiDev/sound3ds-share/main/docs/share-pc.png" alt="Sound3DS Share on the PC" width="640">
  </a>
</p>

**[Sound3DS Share](https://github.com/PeppiDev/sound3ds-share)** is the companion app for your PC (Windows or Linux). It's **needed to connect your SoundCloud account** (steps in [SoundCloud account](#soundcloud-account)), and it also lets you **fill up Local mode without taking out the SD card**:

1. Open **Sound3DS Share**, browse your folders and pick songs. They're grouped by album with their cover art and tags, and you can fix anything missing (album, artist, track number, cover) before sending.
2. Click **Generate QR**.
3. On the console, open **Settings → Receive from PC** and scan it: the songs download over Wi-Fi to `sdmc:/music` in the background (music keeps playing) and show up in **Local**, each one in its album in the cover flow.

Sound3DS Share downloads and instructions are in its repository: **https://github.com/PeppiDev/sound3ds-share** (the standalone Sound3DS QR app still works, but you don't need it anymore).

---

## 🛠️ Building and running

The project uses an isolated **Podman** container with the official `devkitpro/devkitarm` toolchain.

### 1. Build the `.3dsx`
```bash
./scripts/build.sh
```

### 2. Run it in the Azahar emulator (Citra fork)
```bash
./scripts/run_azahar.sh
```

### 3. Test the HLS/AAC path on the PC
```bash
tests/host/run.sh <folder with playlist.m3u8, init.mp4 and seg0.m4s, seg1.m4s...>
```
Builds the demuxer and Helix AAC for the PC, decodes the segments and compares the result against ffmpeg (it must exceed 40 dB SNR).

### 4. Build the `.cia`
```bash
./scripts/build_cia.sh
```
Requires `makerom` ([Project_CTR](https://github.com/3DSGuy/Project_CTR)) and `bannertool`; by default it looks for them in `~/.local/share/3ds-tools/` (you can set `MAKEROM=` and `BANNERTOOL=`). The icon, banner and banner sound are in `cia/`.

### 5. Install on your console (Old 3DS / 2DS / New 3DS)
- **As a `.cia`**: serve the file with a local server (for example `serve_cia.py`) and use *Remote Install → Scan QR Code* in FBI. It stays on the HOME menu.
- **As a `.3dsx`**: copy `sound3ds.3dsx` and `sound3ds.smdh` to `sdmc:/3ds/sound3ds/` and launch it from the **Homebrew Launcher**.

Binaries aren't stored in the repository; every version is published on GitHub Releases.

---

## 📌 Current status and next steps

- Tested on real hardware: Old 2DS, New 3DS XL and New 2DS XL (Luma3DS). The CSND fallback still needs testing on a console (Azahar doesn't play it).
- Streaming supports progressive MP3 and HLS with AAC-LC in fMP4. DRM-encrypted SoundCloud tracks can't be played.
- AAC decoder load measured by a user on a real Old 3DS: a stable 12-13% (the top screen shows it as a percentage).
- The SoundCloud `client_id` is hardcoded and SSL verification is disabled; both will be revisited when evaluating providers.
- The YouTube Music evaluation is in [`docs/YOUTUBE_MUSIC_EVALUATION.md`](docs/YOUTUBE_MUSIC_EVALUATION.md) (in Spanish).

## 🤖 AI usage

This project was built with AI assistance (Claude).

## 🙌 Credits and third-party licenses

- [minimp3](https://github.com/lieff/minimp3) (CC0) for MP3, RealNetworks' [Helix AAC](https://github.com/pschatzmann/arduino-libhelix) (RPSL) for AAC, [stb_image](https://github.com/nothings/stb) (public domain / MIT) for cover art and [quirc](https://github.com/dlbeer/quirc) (ISC) to read QR codes.
- [devkitPro](https://devkitpro.org): libctru, citro2d/citro3d, libcurl, mbedTLS and jansson.
- Third-party license texts are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## 📄 License

Sound3DS is released under the [MIT License](LICENSE). Third-party components keep their own licenses (see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)); the Helix AAC decoder in `source/helix-aac/` is under the RealNetworks Public Source License.

## Versioning and release notes

See [`CHANGELOG.md`](CHANGELOG.md) (in Spanish) for the history.
