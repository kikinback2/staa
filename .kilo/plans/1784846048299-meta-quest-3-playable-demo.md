# Implementation Plan: Meta Quest 3 Playable Demo for Slaves to Armok: Awakening

## 1. Overview & Architecture Strategy
This plan outlines the technical steps required to get from the current prototype state to a fully playable demo running natively on the **Meta Quest 3** (Standalone Android APK), communicating over local Wi-Fi with Dwarf Fortress running on a host PC.

### Primary Decisions
- **Target Platform:** Standalone Android APK on Meta Quest 3 built using Godot 4.x (C# / Mobile Vulkan / OpenXR).
- **Network Architecture:** UDP LAN Auto-Discovery (Port 9001) for zero-configuration host connection + in-VR fallback manual IP keypad (Port 9000 TCP Protobuf stream).
- **Gameplay Scope:** 1:1 VR physical weapon combat against DF anatomical body parts, 3D greedy-meshed terrain, and structured VR menu interaction.
- **Backend Architecture:** Dual-mode DFHack C++ plugin (`dfvr_bridge.cpp`) featuring both live DF viewscreen key/memory injection and a standalone Mock Server mode for rapid testing without launching Dwarf Fortress.

---

## 2. Key Components & Implementation Steps

### Phase 1: Godot 4 OpenXR Project Scaffolding (`frontend/`)
1. **Godot Project Configuration:**
   - Create `frontend/project.godot` configured for Godot 4.x (C# / .NET) with Mobile Vulkan rendering and OpenXR plugin enabled.
   - Configure Android export preset for Meta Quest 3 (target Android SDK 32+, OpenXR Meta vendor plugin enabled).
   - Set required Android Manifest permissions: `android.permission.INTERNET`, `android.permission.ACCESS_NETWORK_STATE`, `android.permission.CHANGE_WIFI_MULTICAST_STATE`.
2. **VR Player Rig Setup (`scenes/PlayerRig.tscn`):**
   - Implement `XROrigin3D` with `XRCamera3D` and left/right `XRController3D` nodes.
   - Add controller models and laser pointer/raycast interaction for VR menus.
3. **In-VR Network Status & Fallback Keypad UI (`scenes/NetworkUI.tscn`):**
   - Floating 3D VR canvas showing auto-discovery connection status.
   - Virtual numeric keypad allowing manual host IP entry if auto-discovery fails.

---

### Phase 2: Backend Enhancement (`backend/` & `proto/`)
1. **UDP Auto-Discovery Service (`dfvr_bridge.cpp`):**
   - Add UDP broadcast thread sending periodic beacon (`"DFVR_HOST:<ip>:<port>"`) on UDP port 9001.
   - Allow Godot C# client on Quest 3 to automatically pick up host IP on the Wi-Fi network.
2. **DFHack Standalone Mock Server Mode:**
   - Implement compile-time flag (`#ifdef MOCK_SERVER`) or runtime command-line switch allowing `dfvr_bridge` to run as a standalone C++ executable without DFHack runtime.
   - Generate mock player, map blocks (16x16 grid), and a goblin unit with standard humanoid body parts to test Quest frontend standalone.
3. **Live DF Adventure Mode Action Injector:**
   - Implement complete key injection into `df::viewscreen_adventure_menust` for `ActionRequest` handling.
   - Map `selected_option_id` to exact menu navigation keystrokes (`Attack`, selecting body part integer, executing strike).

---

### Phase 3: Spatial World & VR Physics Combat (`frontend/`)
1. **Networking Integration (`DFVRClient.cs`):**
   - Add UDP discovery listener (`UdpClient`) to automatically connect to detected host IP.
   - Maintain length-prefixed TCP streaming loop for Protobuf `GameStatePayload` deserialization.
2. **Dynamic Terrain Meshing:**
   - Refine greedy meshing in `DFVRClient.cs` to generate textured StaticBody3D chunks with concave collision shapes from DF `MapBlock` tiles.
3. **Creature Anatomical Hitboxing:**
   - Instantiate parent-child unit rigs using `EntityData` and `BodyPart` relative positions and relative volume scaling.
   - Assign `Area3D` colliders tagged with `entity_id` and `body_part_id` to layer 2 (Enemy Hitbox Layer).
4. **VR Weapon Mechanics & Swing Injection:**
   - Implement physical sword/mace attached to right `XRController3D` using `RigidBody3D` or velocity tracking.
   - Detect `Area3D` collision between weapon tip and entity body part collider.
   - Send `ActionRequest` payload (`selected_option_id = body_part_id`) to backend via TCP stream.

---

### Phase 4: Standalone Android Build, Quest Deployment & Validation
1. **Quest 3 APK Build:**
   - Build Android APK via `godot --export-release "Android" frontend/project.godot`.
2. **Device Deployment:**
   - Deploy APK to connected Meta Quest 3 via `adb install -r bin/staa_quest3.apk`.
3. **Validation & Verification Strategy:**
   - **Test 1: Auto-Discovery & Network Stability** – Verify Quest 3 discovers host PC IP over Wi-Fi and establishes Protobuf handshake without dropping frames.
   - **Test 2: Standalone Mock Mode** – Verify 90 FPS rendering on Quest 3 with 3x3 map blocks, unit hitboxes, and mock weapon hits.
   - **Test 3: End-to-End Live DF Combat** – Launch Dwarf Fortress Adventure Mode on PC, swing VR sword at an enemy in Quest 3, verify physical hit triggers corresponding attack in DF adventure screen.

---

## 3. Risks & Mitigations
- **Network Latency / Wi-Fi Jitter:** TCP streaming over Wi-Fi might experience packet burst delays. *Mitigation:* Implement client-side frame interpolation for entity movement and non-blocking socket handling.
- **Quest 3 Performance Limits:** Dynamic 3D meshing on main thread can cause frame drops. *Mitigation:* Offload greedy meshing calculations to background C# tasks (`Task.Run`) and apply meshes on main thread deferred (`CallDeferred`).

---

## 4. Final Deliverables
1. `frontend/project.godot` and OpenXR scene setup (`PlayerRig.tscn`, `Main.tscn`).
2. Updated `backend/src/dfvr_bridge.cpp` with UDP LAN broadcast & Mock Server mode.
3. Refined `frontend/DFVRClient.cs` with UDP auto-discovery, Quest VR controls, and weapon collision triggers.
4. Standalone Meta Quest 3 APK build target.
