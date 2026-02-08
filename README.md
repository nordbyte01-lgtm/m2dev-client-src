# M2Dev Client Source

[![build](https://github.com/d1str4ught/m2dev-client-src/actions/workflows/main.yml/badge.svg)](https://github.com/d1str4ught/m2dev-client-src/actions/workflows/main.yml)

This repository contains the source code necessary to compile the game client executable.

## How to build (short version)

> cmake -S . -B build
>
> cmake --build build

**For more installation/configuration, check out the [instructions](#installationconfiguration) below.**

---

## 📋 Changelog

### Encryption & Security Overhaul

The entire legacy encryption system has been replaced with [libsodium](https://doc.libsodium.org/).

#### Removed Legacy Crypto
* **Crypto++ (cryptopp) vendor library** — Completely removed from the project
* **Panama cipher** (`CFilterEncoder`, `CFilterDecoder`) — Removed from `NetStream`
* **TEA encryption** (`tea.h`, `tea.cpp`) — Removed from both client and server
* **DH2 key exchange** (`cipher.h`, `cipher.cpp`) — Removed from `EterBase`
* **Camellia cipher** — Removed all references
* **`_IMPROVED_PACKET_ENCRYPTION_`** — Entire system removed (XTEA key scheduling, sequence encryption, key agreement)
* **`adwClientKey[4]`** — Removed from all packet structs (`TPacketCGLogin2`, `TPacketCGLogin3`, `TPacketGDAuthLogin`, `TPacketGDLoginByKey`, `TPacketLoginOnSetup`) and all associated code on both client and server
* **`LSS_SECURITY_KEY`** — Dead code removed (`"testtesttesttest"` hardcoded key, `GetSecurityKey()` function)

#### New Encryption System (libsodium)
* **X25519 key exchange** — `SecureCipher` class handles keypair generation and session key derivation via `crypto_kx_client_session_keys` / `crypto_kx_server_session_keys`
* **XChaCha20-Poly1305 AEAD** — Used for authenticated encryption of handshake tokens (key exchange, session tokens)
* **XChaCha20 stream cipher** — Used for in-place network buffer encryption via `EncryptInPlace()` / `DecryptInPlace()` (zero overhead, nonce-counter based replay prevention)
* **Challenge-response authentication** — HMAC-based (`crypto_auth`) verification during key exchange to prove shared secret derivation
* **New handshake protocol** — `HEADER_GC_KEY_CHALLENGE` / `HEADER_CG_KEY_RESPONSE` / `HEADER_GC_KEY_COMPLETE` packet flow for secure session establishment

#### Network Encryption Pipeline
* **Client send path** — Data is encrypted at queue time in `CNetworkStream::Send()` (prevents double-encryption on partial TCP sends)
* **Client receive path** — Data is decrypted immediately after `recv()` in `__RecvInternalBuffer()`, before being committed to the buffer
* **Server send path** — Data is encrypted in `DESC::Packet()` via `EncryptInPlace()` after encoding to the output buffer
* **Server receive path** — Newly received bytes are decrypted in `DESC::ProcessInput()` via `DecryptInPlace()` before buffer commit

#### Login Security Hardening
* **Removed plaintext login path** — `HEADER_CG_LOGIN` (direct password to game server) has been removed. All game server logins now require a login key obtained through the auth server (`HEADER_CG_LOGIN2` / `LoginByKey`)
* **CSPRNG login keys** — `CreateLoginKey()` now uses `randombytes_uniform()` (libsodium) instead of the non-cryptographic Xoshiro128PlusPlus PRNG
* **Single-use login keys** — Keys are consumed (removed from the map) immediately after successful authentication
* **Shorter key expiry** — Expired login keys are cleaned up after 15 seconds (down from 60 seconds). Orphaned keys (descriptor gone, never expired) are also cleaned up
* **Login rate limiting** — Per-IP tracking of failed login attempts. After 5 failures within 60 seconds, the IP is blocked with a `BLOCK` status and disconnected. Counter resets after cooldown or successful login
* **Removed Brazil password bypass** — The `LC_IsBrazil()` block that unconditionally disabled password verification has been removed

#### Pack File Encryption
* **libsodium-based pack encryption** — `PackLib` now uses XChaCha20-Poly1305 for pack file encryption, replacing the legacy Camellia/XTEA system
* **Secure key derivation** — Pack encryption keys are derived using `crypto_pwhash` (Argon2id)

---

### Networking Modernization Roadmap

A 5-phase modernization of the entire client/server networking stack — packet format, buffer management, handshake protocol, connection architecture, and packet dispatching. Every phase is complete and verified on both client and server.

---

#### Phase 1 — Packet Format + Buffer System + Memory Safety

Replaced the legacy 1-byte packet headers and raw C-style buffers with a modern, uniform protocol.

##### What changed
* **2-byte headers + 2-byte length prefix** — All packet types (`CG::`, `GC::`, `GG::`, `GD::`, `DG::`) now use `uint16_t` header + `uint16_t` length. This increases the addressable packet space from 256 to 65,535 unique packet types and enables safe variable-length parsing
* **Namespaced packet headers** — All headers moved from flat `HEADER_CG_*` defines to C++ namespaces: `CG::MOVE`, `GC::PING`, `GG::LOGIN`, `GD::PLAYER_SAVE`, `DG::BOOT`. Subheaders similarly namespaced: `GuildSub::GC::LOGIN`, `ShopSub::CG::BUY`, etc.
* **RAII RingBuffer** — All raw `buffer_t` / `LPBUF` / `new[]`/`delete[]` patterns replaced with a single `RingBuffer` class featuring lazy compaction at 50% read position, exponential growth, and inlined accessors
* **PacketReader / PacketWriter** — Type-safe helpers that wrap buffer access with bounds checking, eliminating raw pointer arithmetic throughout the codebase
* **Sequence system modernized** — Packet sequence tracking retained for debugging but fixed to byte offset 4 (after header + length)
* **SecureCipher** — XChaCha20-Poly1305 stream cipher for all post-handshake traffic (see Encryption section above)

##### What was removed
* `buffer.h` / `buffer.cpp` (legacy C buffer library)
* All `LPBUF`, `buffer_new()`, `buffer_delete()`, `buffer_read()`, `buffer_write()` calls
* Raw `new[]`/`delete[]` buffer allocations in DESC classes
* 1-byte header constants (`HEADER_CG_*`, `HEADER_GC_*`, etc.)

##### Why
The legacy 1-byte header system limited the protocol to 256 packet types (already exhausted), raw C buffers had no bounds checking and were prone to buffer overflows, and the flat namespace caused header collisions between subsystems.

---

#### Phase 2 — Modern Buffer System *(merged into Phase 1)*

All connection types now use `RingBuffer` uniformly.

##### What changed
* **All DESC types** (`DESC`, `CLIENT_DESC`, `DESC_P2P`) use `RingBuffer` for `m_inputBuffer`, `m_outputBuffer`, and `m_bufferedOutputBuffer`
* **PeerBase** (db layer) ported to `RingBuffer`
* **TEMP_BUFFER** (local utility for building packets) backed by `RingBuffer`

##### What was removed
* `libthecore/buffer.h` and `libthecore/buffer.cpp` — the entire legacy buffer library

##### Why
The legacy buffer system used separate implementations across different connection types, had no RAII semantics (manual malloc/free), and offered no protection against buffer overflows.

---

#### Phase 3 — Simplified Handshake

Replaced the legacy multi-step handshake (4+ round trips with time synchronization and UDP binding) with a streamlined 1.5 round-trip flow.

##### What changed
* **1.5 round-trip handshake** — Server sends `GC::KEY_CHALLENGE` (with embedded time sync), client responds with `CG::KEY_RESPONSE`, server confirms with `GC::KEY_COMPLETE`. Session is encrypted from that point forward
* **Time sync embedded** — Initial time synchronization folded into `GC::KEY_CHALLENGE`; periodic time sync handled by `GC::PING` / `CG::PONG`
* **Handshake timeout** — 5-second expiry on handshake phase; stale connections are automatically cleaned up in `DestroyClosed()`

##### What was removed
* **6 dead packet types**: `CG_HANDSHAKE`, `GC_HANDSHAKE`, `CG_TIME_SYNC`, `GC_TIME_SYNC`, `GC_HANDSHAKE_OK`, `GC_BINDUDP`
* **Server functions**: `StartHandshake()`, `SendHandshake()`, `HandshakeProcess()`, `CreateHandshake()`, `FindByHandshake()`, `m_map_handshake`
* **Client functions**: `RecvHandshakePacket()`, `RecvHandshakeOKPacket()`, `m_HandshakeData`, `SendHandshakePacket()`
* ~12 server files and ~10 client files modified

##### Why
The original handshake required 4+ round trips, included dead UDP binding steps, had no timeout protection (stale connections could linger indefinitely), and the time sync was a separate multi-step sub-protocol that added latency to every new connection.

---

#### Phase 4 — Unified Connection (Client-Side Deduplication)

Consolidated duplicated connection logic into the base `CNetworkStream` class.

##### What changed
* **Key exchange** (`RecvKeyChallenge` / `RecvKeyComplete`) moved from 4 separate implementations to `CNetworkStream` base class
* **Ping/pong** (`RecvPingPacket` / `SendPongPacket`) moved from 3 separate implementations to `CNetworkStream` base class
* **CPythonNetworkStream** overrides `RecvKeyChallenge` only for time sync, delegates all crypto to base
* **CGuildMarkDownloader/Uploader** — `RecvKeyCompleteAndLogin` wraps base + sends `CG::MARK_LOGIN`
* **CAccountConnector** — Fixed raw `crypto_aead` bug (now uses base class `cipher.DecryptToken`)
* **Control-plane structs** extracted to `EterLib/ControlPackets.h` (Phase, Ping, Pong, KeyChallenge, KeyResponse, KeyComplete)
* **CGuildMarkUploader** — `m_pbySymbolBuf` migrated from `new[]`/`delete[]` to `std::vector<uint8_t>`

##### What was removed
* ~200 lines of duplicated code across `CAccountConnector`, `CGuildMarkDownloader`, `CGuildMarkUploader`, and `CPythonNetworkStream`

##### Why
The same key exchange and ping/pong logic was copy-pasted across 3-4 connection subclasses, leading to inconsistent behavior (the `CAccountConnector` had a raw crypto bug), difficult maintenance, and unnecessary code volume.

---

#### Phase 5 — Packet Handler Registration / Dispatcher

Replaced giant `switch` statements with `std::unordered_map` dispatch tables for O(1) packet routing.

##### What changed

**Client:**
* `CPythonNetworkStream` — Phase-specific handler maps for Game, Loading, Login, Select, and Handshake phases
* Registration pattern: `m_gameHandlers[GC::MOVE] = &CPythonNetworkStream::RecvCharacterMovePacket;`
* Dispatch: `DispatchPacket(m_gameHandlers)` — reads header, looks up handler, calls it

**Server:**
* `CInputMain`, `CInputDead`, `CInputAuth`, `CInputLogin`, `CInputP2P`, `CInputHandshake`, `CInputDB` — all converted to dispatch tables
* `CInputDB` uses 3 template adapters (`DataHandler`, `DescHandler`, `TypedHandler`) + 14 custom adapters for the diverse DB callback signatures

##### What was removed
* All `switch (header)` blocks across 7 server input processors and 5 client phase handlers
* ~3,000 lines of switch/case boilerplate

##### Why
The original dispatch used switch statements with 50-100+ cases each. Adding a new packet required modifying a massive switch block, which was error-prone and caused merge conflicts. The table-driven approach enables O(1) lookup, self-documenting handler registration, and trivial addition of new packet types.

---

### Post-Phase 5 Cleanup

Follow-up tasks after the core roadmap was complete.

#### One-Liner Adapter Reformat
* 24 adapter methods across 4 server files (`input_main.cpp`, `input_p2p.cpp`, `input_auth.cpp`, `input_login.cpp`) reformatted from single-line to multi-line for readability

#### MAIN_CHARACTER Packet Merge
* **4 mutually exclusive packets** (`GC::MAIN_CHARACTER`, `MAIN_CHARACTER2_EMPIRE`, `MAIN_CHARACTER3_BGM`, `MAIN_CHARACTER4_BGM_VOL`) merged into a single unified `GC::MAIN_CHARACTER` packet
* Single struct always includes BGM fields (zero when unused — 29 extra bytes on a one-time-per-load packet)
* 4 nearly identical client handlers merged into 1
* 3 redundant server send paths merged into 1

#### UDP Leftover Removal
* **7 client files deleted**: `NetDatagram.h/.cpp`, `NetDatagramReceiver.h/.cpp`, `NetDatagramSender.h/.cpp`, `PythonNetworkDatagramModule.cpp`
* **8 files edited**: Removed dead stubs (`PushUDPState`, `initudp`, `netSetUDPRecvBufferSize`, `netConnectUDP`), declarations, and Python method table entries
* **Server**: Removed `socket_udp_read()`, `socket_udp_bind()`, `__UDP_BLOCK__` define

#### Subheader Dispatch
* Extended the Phase 5 table-driven pattern to subheader switches with 8+ cases
* **Client**: Guild (19 sub-handlers), Shop (10), Exchange (8) in `PythonNetworkStreamPhaseGame.cpp`
* **Server**: Guild (15 sub-handlers) in `input_main.cpp`
* Small switches intentionally kept as-is: Messenger (5), Fishing (6), Dungeon (2), Server Shop (4)

---

### Performance Audit & Optimization

Comprehensive audit of all Phase 1-5 changes to identify and eliminate performance overhead.

#### Debug Logging Cleanup
* **Removed all hot-path `TraceError`/`Tracef`/`sys_log`** from networking code on both client and server
* Client: `NetStream.cpp`, `SecureCipher.cpp`, `PythonNetworkStream*.cpp` — eliminated per-frame and per-packet traces that caused disk I/O every frame
* Server: `desc.cpp`, `input.cpp`, `input_login.cpp`, `input_auth.cpp`, `SecureCipher.cpp` — eliminated `[SEND]`, `[RECV]`, `[CIPHER]` logs that fired on every packet

#### Packet Processing Throughput
* **`MAX_RECV_COUNT` 4 → 32** — Game phase now processes up to 32 packets per frame (was 4, severely limiting entity spawning on map entry)
* **Loading phase while-loop** — Changed from processing 1 packet per frame to draining all available packets, making phase transitions near-instant

#### Flood Check Optimization
* **Replaced `get_dword_time()` with `thecore_pulse()`** in `DESC::CheckPacketFlood()` — eliminates a `gettimeofday()` syscall on every single packet received. `thecore_pulse()` is cached once per game-loop iteration

#### Flood Protection
* **Per-IP connection limits** — Configurable maximum connections per IP address (`flood_max_connections_per_ip`, default: 10)
* **Global connection limits** — Configurable maximum total connections (`flood_max_global_connections`, default: 8192)
* **Per-second packet rate limiting** — Connections exceeding `flood_max_packets_per_sec` (default: 300) are automatically disconnected
* **Handshake timeout** — 5-second expiry prevents connection slot exhaustion from incomplete handshakes

---

### Pre-Phase 3 Cleanup

Preparatory cleanup performed before the handshake simplification.

* **File consolidation** — Merged scattered packet definitions into centralized header files
* **Alias removal** — Removed legacy `#define` aliases that mapped old names to new identifiers
* **Monarch system removal** — Completely removed the unused Monarch (emperor) system from both client and server, including all related packets, commands, quest functions, and UI code
* **TrafficProfiler removal** — Removed the `TrafficProfiler` class and all references (unnecessary runtime overhead)
* **Quest management stub removal** — Removed empty `questlua_mgmt.cpp` (monarch-era placeholder with no functions)

---

### Summary of Removed Legacy Systems

A consolidated reference of all legacy systems, files, and dead code removed across the entire modernization effort.

| System | What was removed | Replaced by |
|--------|-----------------|-------------|
| **Legacy C buffer** | `buffer.h`, `buffer.cpp`, all `LPBUF`/`buffer_new()`/`buffer_delete()` calls, raw `new[]`/`delete[]` buffer allocations | RAII `RingBuffer` class |
| **1-byte packet headers** | All `HEADER_CG_*`, `HEADER_GC_*`, `HEADER_GG_*`, `HEADER_GD_*`, `HEADER_DG_*` defines | 2-byte namespaced headers (`CG::`, `GC::`, `GG::`, `GD::`, `DG::`) |
| **Old handshake protocol** | 6 packet types (`CG_HANDSHAKE`, `GC_HANDSHAKE`, `CG_TIME_SYNC`, `GC_TIME_SYNC`, `GC_HANDSHAKE_OK`, `GC_BINDUDP`), all handshake functions and state | 1.5 round-trip key exchange (`KEY_CHALLENGE`/`KEY_RESPONSE`/`KEY_COMPLETE`) |
| **UDP networking** | 7 client files (`NetDatagram*.h/.cpp`, `PythonNetworkDatagramModule.cpp`), server `socket_udp_read()`/`socket_udp_bind()`/`__UDP_BLOCK__` | Removed entirely (game is TCP-only) |
| **Old sequence system** | `m_seq`, `SetSequence()`, `GetSequence()`, old sequence variables | Modernized sequence at fixed byte offset 4 |
| **TrafficProfiler** | `TrafficProfiler` class and all references | Removed entirely |
| **Monarch system** | All monarch/emperor packets, commands (`do_monarch_*`), quest functions (`questlua_monarch.cpp`, `questlua_mgmt.cpp`), UI code, GM commands | Removed entirely (unused feature) |
| **Legacy crypto** | Crypto++, Panama cipher, TEA, DH2, Camellia, XTEA, `adwClientKey[4]`, `LSS_SECURITY_KEY` | libsodium (X25519 + XChaCha20-Poly1305) |
| **Switch-based dispatch** | Giant `switch (header)` blocks (50-100+ cases each) across 7 server input processors and 5 client phase handlers | `std::unordered_map` dispatch tables |
| **Duplicated connection code** | Key exchange and ping/pong copy-pasted across 3-4 client subclasses | Consolidated in `CNetworkStream` base class |

---

# Installation/Configuration
This is the third part of the entire project and it's about the client binary, the executable of the game.

Below you will find a comprehensive guide on how to configure all the necessary components from scratch.

This guide is made using a **Windows** environment as the main environment and **cannot work in non-Windows operating systems!**

This guide also uses the latest versions for all software demonstrated as of its creation date at February 4, 2026.

© All copyrights reserved to the owners/developers of any third party software demonstrated in this guide other than this project/group of projects.

<br>

### 📋 Order of projects configuration
If one or more of the previous items is not yet configured please come back to this section after you complete their configuration steps.

>  - ✅ [M2Dev Server Source](https://github.com/d1str4ught/m2dev-server-src)
>  - ✅ [M2Dev Server](https://github.com/d1str4ught/m2dev-server)
>  - ▶️ [M2Dev Client Source](https://github.com/d1str4ught/m2dev-client-src)&nbsp;&nbsp;&nbsp;&nbsp;[**YOU ARE HERE**]
>  - ⏳ [M2Dev Client](https://github.com/d1str4ught/m2dev-client)&nbsp;&nbsp;&nbsp;&nbsp;[**ALSO CONTAINS ADDITIONAL INFORMATION FOR POST-INSTALLATION STEPS**]

<br>

### 🧱 Software Prerequisites

<details>
  <summary>
    Please make sure that you have installed the following software in your machine before continuing:
  </summary>

  <br>

  > <br>
  >
  >  - ![Visual Studio](https://metin2.download/picture/B6U1Pg0lMlA486D1ekVLIytP72pDP8Yg/.png)&nbsp;&nbsp;**Visual Studio**:&nbsp;&nbsp;The software used to edit and compile the source code. [Download](https://visualstudio.microsoft.com/vs/)
  >
  >  - ![Visual Studio Code](https://metin2.download/picture/Hp33762v422mjz6lgil91Gey380NwA7j/.png)&nbsp;&nbsp;**Visual Studio Code (VS Code)**:&nbsp;&nbsp;A lighter alternative to Visual Studio, harder to build the project in this software but it is recommended for code editing. [Download](https://code.visualstudio.com/Download)
  > - ![Git](https://metin2.download/picture/eCpg436LhgG1zg9ZqvyfcANS68F60y6O/.png)&nbsp;&nbsp;**Git**:&nbsp;&nbsp;Used to clone the repositories in your Windows machine. [Download](https://git-scm.com/install/windows)
  > - ![CMake](https://metin2.download/picture/6O8Ho9N0XScDaLLL8h0lrkh8DcKlgJ6M/.png)&nbsp;&nbsp;**CMake**:&nbsp;&nbsp;Required for setting up and configuring the build of the source code. [Download](https://git-scm.com/install/windows)
  > - ![Notepad++](https://metin2.download/picture/7Vf5Yv1T48nHprT2hiH0VfZx635HAZP2/.png)&nbsp;&nbsp;**Notepad++ (optional but recommended)**:&nbsp;&nbsp;Helps with quick, last minute edits. [Download](https://notepad-plus-plus.org/downloads/)
  >
  > <br>
  >

</details>

<br>

### 👁️ Required Visual Studio packages

Make sure you have installed these packages with Visual Studio Installer in order to compile C++ codebases:

<details>
  <summary>
    Packages
  </summary>

  <br>

  >
  > <br>
  >
  > ![](https://metin2.download/picture/Rh6PJwXf0J6Y3TZFg7Zx8HVBUGNXDVDN/.png)
  >
  > ![](https://metin2.download/picture/1eoYD0IJB5k9c9BhhyWTjDVm6zidv8rM/.png)
  >
  > **Note**: **Windows 11 SDK**'s can be replaced by **Windows 10 SDK**'s, but it is recommended to install one of them.
  >
  > <br>
  >
</details>

<br>


### ⬇️ Obtaining the Client Source

To build the source for the first time, you first need to clone it. In your command prompt, `cd` into your desired location or create a new folder wherever you want and download the project using `Git`.

<details>
  <summary>
    Here's how
  </summary>

  <br>

  >
  > <br>
  >
  >
  > Open up your terminal inside or `cd` into your desired folder and type this command:
  >
  > ```
  > git clone https://github.com/d1str4ught/m2dev-client-src.git
  > ```
  >
  > <br>
  >
  > ### ✅ You have successfully obtained the Client Source project!
  >
  > <br>
  >
</details>

<br>

### 🛠️ Building the Source Code

Building the project is extremely simple, if all Visual Studio components are being installed correctly.

<details>
  <summary>
    Instructions
  </summary>

  <br>

  >
  > <br>
  >
  > Open up your terminal inside, or `cd` in your project's root working directory and initialize the build with this command:
  >
  > ```
  > cmake -S . -B build
  > ```
  >
  > A new `build` folder has been created in your project's root directory. This folder contains all the build files and configurations, along with the `sln` file to open the project in Visual Studio.
  >
  > ![](https://metin2.download/picture/icUIK7eITD7ng1jNmMtz3N1jhc6GjKS9/.png)
  >
  > ![](https://metin2.download/picture/jS099wxf40xlxdPXRy1ZR7YqO86h7N3w/.png)
  >
  > Double click on that file to launch Visual Studio and load the project.
  >
  > In the Solution Explorer, select all the projects minus the container folders, right click on one of the selected items, and click **Properties**
  >
  > ![](https://metin2.download/picture/J6aHw6uw330DeqyTaw2ZDVgRcT9hr3hr/.png)
  >
  > Next, make sure that the following settings are adjusted like this:
  >
  > 1. **Windows SDK Version** should be the latest of Windows 10. It is not recommended to select any Windows 11 versions yet if avalable.
  > 2. **Platform Toolset** is the most important part for your build to succeed! Select the highest number you see. **v145** is for Visual Studio 2026. If you are running Visual Studio 2022 you won't have that, you will have **v143**, select that one, same goes for older Visual Studio versions.
  > 3. **C++ Language Standard** should be C++20 as it is the new standard defined in the CMakeList.txt files as well. Might as well set it like that for all dependencies.
  > 4. **C Language Standard** should be C17 as it is the new standard defined in the CMakeList.txt files as well. Might as well set it like that for all dependencies.
  >
  > Once done, click Apply and then OK to close this dialog.
  >
  > ![](https://metin2.download/picture/2uacNZ4zOCYfI6vmigMTFwbuQ4tcmZ1C/.png)
  >
  > After that, in the toolbar at the top of the window, select your desired output configuration:
  >
  > ![](https://metin2.download/picture/cB0AGhHJ3TKFResDSeIgGmF7Vbo2SyGK/.png)
  >
  > Finally, click on the **Build** option at the top and select **Build Solution**, or simply press **CTRL+SHIFT+B** in your keyboard with all the projects selected.
  >
  > ![](https://metin2.download/picture/EN38Dz0Yef2edZ5Ptp4a5t4xWs1tr9V5/.png)
  >
  > **Note**: if this is **NOT** your first build after executing the `cmake -S . -B build` command for this workspace, it is recommended to click **Clean Solution** before **Build Solution**.
  >
  > <br>
  >
  > Where to find your compiled binaries:
  >
  > Inside the **build** folder in your cloned repository, you should have a **bin** folder and inside that, you should have a **Debug**, **Release**, **RelWithDebInfo** or **MinSizeRel** folder, depending on your build configuration selection.
  >
  > In that folder you should be seeing all your binaries:
  >
  > ![](https://metin2.download/picture/4cVxiU2Ac8CON58Gh70f6Do34dGBXpOz/.png)
  >
  > If you did **NOT** install the **Client** project yet, you are done here.
  >
  > If you **HAVE** the **Client** project installed, paste these 2 `.exe` files in these locations inside the Server project:
  >
  > - **Metin2_<Debug|Release|RelWithDebInfo|MinSizeRel>.exe**: inside root folder of the Client project
  > - **PackMaker.exe**: inside `assets\PackMaker.exe`
  >
  > <br>
  >
  > ### ✅ You have successfully built the Client Source!
  >
  > <br>
  >
</details>

<br>
<br>

---

<br>
<br>

## 🔥 The Client Source part of the guide is complete!

<br>
<br>

## Next steps
You should now be finally ready to proceed to [Client project](https://github.com/d1str4ught/m2dev-client) packing and entering the game for the first time!

⭐ **NEW**: We are now on Discord, feel free to [check us out](https://discord.gg/ETnBChu2Ca)!
