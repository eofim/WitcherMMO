# WitcherMMO Roadmap

This roadmap describes the intended engineering direction of WitcherMMO.

It is deliberately staged. The project should not attempt to build every MMO system at once.

## Status legend

- ✅ Established
- 🟡 In progress / investigation
- ⬜ Planned
- 🧪 Experimental

---

## Phase 0 — Fork foundation

Goal: establish a clean project identity without destabilizing the inherited multiplayer code.

- ✅ Fork WitcherOnline into WitcherMMO.
- ✅ Preserve GPL-3.0.
- ✅ Preserve and expand upstream attribution.
- ✅ Document the difference between inherited features and WitcherMMO goals.
- 🟡 Audit build process, dependencies and project structure.
- ⬜ Map native C++ ↔ Witcher Script interfaces.
- ⬜ Inventory every runtime-sensitive `WitcherOnline` identifier before renaming anything.
- ⬜ Establish a reproducible development build.

**Exit condition:** WitcherMMO can be built from source with the inherited behavior unchanged and the team understands the main client/server/script boundaries.

---

## Phase 1 — Custom Character vertical slice

Goal: a player should enter the game as a persistent custom character rather than being conceptually tied to Geralt.

### Character data

⬜ Define a server-side character model containing:

- unique character ID;
- account ID;
- name;
- sex/body type;
- character template;
- head/hair/beard/appearance selections;
- equipment appearance;
- level and experience placeholders;
- last world/location;
- creation/update timestamps.

### Client flow

⬜ Character Select.
⬜ Character Creator.
⬜ Spawn selected character.
⬜ Restore appearance after reconnect/map transitions.
⬜ Synchronize appearance to remote players.
⬜ Synchronize visible equipment.

### Initial scope

Do **not** begin with unrestricted facial sliders.

Start with deterministic modular options:

```text
Body
Head
Hair
Beard
Eyes / appearance variant
Scars / appearance variant
Torso
Hands
Legs
Boots
Accessories
```

**Exit condition:** two players can connect with different persistent characters and each client sees the same appearance/equipment for both players.

---

## Phase 2 — Accounts and persistence

Goal: stop treating the local Witcher save as the authoritative source for MMO character state.

⬜ Authentication/session protocol.
⬜ Account model.
⬜ Character slots.
⬜ PostgreSQL persistence.
⬜ Inventory persistence.
⬜ Equipment persistence.
⬜ Position/world persistence.
⬜ Progression persistence.
⬜ Migration/versioning strategy for database schema.
⬜ Validation of client-provided character data.

**Exit condition:** character state survives reconnects and is restored from the WitcherMMO server/database.

---

## Phase 3 — Network entity layer

Goal: establish the foundation required for shared-world NPCs and monsters.

⬜ Stable Network Entity ID.
⬜ Entity registry.
⬜ Spawn/despawn protocol.
⬜ Ownership/authority rules.
⬜ Position and rotation replication.
⬜ State/animation replication.
⬜ Relevancy/interest management.
⬜ World/channel/instance identifiers.
⬜ Recovery from packet loss/out-of-order state where required.

Example concept:

```text
Player          1..N
NPC             100000+
Monster         200000+
Mount/Vehicle   300000+
World object    400000+
```

The exact ranges are placeholders; the protocol should not depend on arbitrary hard-coded ranges.

**Exit condition:** generic server-owned entities can be spawned, updated and despawned consistently on multiple clients.

---

## Phase 4 — NPC and monster synchronization

Goal: multiple players interact with the same logical creature instead of independent local copies.

⬜ Spawn synchronization.
⬜ Position/movement synchronization.
⬜ Animation/state synchronization.
⬜ Shared health.
⬜ Target selection.
⬜ Death state.
⬜ Respawn state.
⬜ Status effects.
⬜ Loot ownership/drop event model.
⬜ Basic AI authority strategy.

Potential progression:

1. Host-authoritative prototype.
2. Server-mediated authority.
3. Server-authoritative state where practical.

**Exit condition:** two players can fight one synchronized monster whose health, target, death and respawn are consistent for both clients.

---

## Phase 5 — Shared combat authority

Goal: reduce divergent combat simulation and prevent persistent MMO state from being decided independently by every client.

⬜ Attack event model.
⬜ Hit validation.
⬜ Damage calculation strategy.
⬜ Signs/abilities replication.
⬜ Status effects.
⬜ Death/revive state.
⬜ PvE threat/target model.
⬜ PvP rules.
⬜ Anti-cheat/validation boundaries.
⬜ Latency compensation strategy where necessary.

**Exit condition:** shared combat produces deterministic persistent outcomes across connected clients.

---

## Phase 6 — Shared quests and world state

Goal: move beyond "players standing in each other's local saves" toward an actually shared world.

⬜ Server quest state representation.
⬜ Party/shared quest progression.
⬜ NPC alive/dead state.
⬜ Interaction flags.
⬜ Doors/objects/world triggers.
⬜ Dialog decision ownership.
⬜ Cutscene policy.
⬜ World event state.
⬜ Time/weather authority.
⬜ Instanced quest-state support where global state would conflict.

**Exit condition:** a party can complete a purpose-built WitcherMMO quest whose important state is shared and persisted by the server.

---

## Phase 7 — MMO systems

Goal: add persistent systems only after the shared-world foundation works.

⬜ Friends.
⬜ Parties.
⬜ Guilds.
⬜ Trading.
⬜ Mail.
⬜ Persistent economy.
⬜ Vendors.
⬜ Crafting.
⬜ Professions.
⬜ Reputation/factions.
⬜ PvP zones/rules.
⬜ World bosses.
⬜ Group content.
⬜ Player housing only if technically justified.

Existing WitcherOnline functionality should be reused where appropriate rather than replaced simply for branding.

---

## Phase 8 — Scaling and operations

Goal: support more concurrent players without attempting to render or simulate an unrealistic number of entities on every client.

⬜ Interest management.
⬜ Spatial partitioning.
⬜ Channels/shards.
⬜ Instances.
⬜ Server metrics.
⬜ Structured logging.
⬜ Crash/error reporting strategy.
⬜ Rate limiting.
⬜ Database backup/recovery.
⬜ Protocol versioning.
⬜ Deployment automation.
⬜ Load testing.

The target should be a **large persistent population with controlled local visibility**, not hundreds of fully simulated players in the same Witcher 3 scene.

---

# Immediate engineering target

The next work item is **not** a global rename.

The next work item is an architecture audit of:

```text
client/
server/
witcher/
```

with special attention to:

```text
Native DLL / ASI
      ↕
NativeBridge
      ↕
Witcher Script
      ↕
Network protocol
      ↕
Server
```

From that audit we can determine the safest insertion point for the WitcherMMO Character System.
