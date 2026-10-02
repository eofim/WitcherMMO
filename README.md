# WitcherMMO

**WitcherMMO** is an experimental open-source project that aims to evolve *The Witcher 3: Wild Hunt* into a more persistent multiplayer RPG experience.

This repository is a fork of [WitcherOnline](https://github.com/rejuvenate7/WitcherOnline), created by **rejuvenate7** and contributors. WitcherOnline provides the multiplayer foundation on which WitcherMMO is being developed.

> [!IMPORTANT]
> WitcherMMO is at an early development stage. The current codebase starts from the WitcherOnline baseline. Features described as goals or roadmap items below are **not** considered implemented until explicitly marked as such.

## Project direction

WitcherMMO intends to extend the existing multiplayer foundation toward a persistent shared-world architecture, including:

- Custom player characters instead of requiring every player to be Geralt.
- Server-side accounts and persistent characters.
- Character appearance, equipment, inventory and progression persistence.
- Stable network entity IDs for players, NPCs and monsters.
- NPC and monster synchronization.
- Shared monster health and combat state.
- Server-authoritative gameplay systems where practical.
- Shared quest and world state.
- Parties, guilds, trading and social systems.
- Interest management, channels or instances to control client load.
- A long-term path toward a persistent MMO-like experience while remaining a mod for *The Witcher 3*.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the development plan.

## Current foundation inherited from WitcherOnline

The upstream WitcherOnline project already provides a substantial multiplayer base, including player synchronization, combat/appearance synchronization, chat, parties, Gwent multiplayer, emotes, player riding, vehicle passengers, item trading and other multiplayer functionality.

At the time this fork was created, upstream also explicitly documented that NPC synchronization, quest progress and shared world state were not fully synchronized. WitcherMMO will treat those limitations as engineering milestones rather than hiding them.

For documentation and usage of the original project:

- [WitcherOnline repository](https://github.com/rejuvenate7/WitcherOnline)
- [WitcherOnline Wiki](https://rejuvenate.gitbook.io/witcheronline)
- [WitcherOnline Nexus page](https://www.nexusmods.com/witcher3/mods/11590)

## Development principles

1. **Preserve upstream attribution and history.** WitcherMMO will not remove credit for the work it is built upon.
2. **Avoid unsafe mass renames.** Internal identifiers such as `WitcherOnlineClient`, `modWitcherOnline` and `WitcherOnline_*` will only be migrated when the affected native/script interfaces are understood and tested.
3. **Build vertical slices first.** A working character flow is more valuable than many partially implemented MMO systems.
4. **Separate presentation from authority.** The Witcher 3 client should increasingly represent server state instead of independently deciding persistent shared-world state.
5. **Do not claim roadmap features as completed.** Documentation should clearly distinguish inherited, implemented, experimental and planned functionality.

## First milestone — Character System

The first WitcherMMO vertical slice is:

```text
Account/Login
    ↓
Character Select
    ↓
Character Creation
    ↓
Persistent Character Data
    ↓
Spawn Custom Player
    ↓
Synchronize Appearance + Equipment
    ↓
Other Players See the Same Character
```

The first implementation work will focus on understanding the existing native/client/script bridge before replacing any upstream naming or behavior.

## Repository layout

The inherited project is broadly divided into:

```text
client/   Native multiplayer client
server/   Multiplayer server
witcher/  Witcher 3 scripts/mod content
assets/   Repository media/assets
```

This structure will be evolved incrementally rather than rewritten blindly.

## Upstream credits

WitcherMMO is based on **WitcherOnline**.

Original credits preserved from the upstream project:

- [rejuvenate7](https://github.com/rejuvenate7) — Lead developer
- [x4lva](https://github.com/x4lva) — Co-developer, Gwent sync
- [Flawkee](https://github.com/flawkee) — Co-developer, native hook / NPC sync work
- [Werasik2aa](https://github.com/werasik2aa) — Author of [Witcher3-Multiplayer](https://github.com/werasik2aa/Witcher3-Multiplayer), acknowledged by WitcherOnline as an important earlier multiplayer implementation

Additional attribution and modification notices are documented in [ATTRIBUTION.md](ATTRIBUTION.md).

## WitcherMMO maintainership

WitcherMMO fork and MMO-oriented development:

- [eofim](https://github.com/eofim)

Contributions should preserve the upstream notices and clearly identify substantial WitcherMMO-specific changes.

## License

This project remains licensed under the **GNU General Public License v3.0 (GPL-3.0)**, consistent with the upstream WitcherOnline repository. See [LICENSE](LICENSE).

The GPL license file inherited from WitcherOnline has intentionally been kept intact.

## Disclaimer

WitcherMMO is an independent fan-made modding project and is not an official CD PROJEKT RED product. *The Witcher*, *The Witcher 3: Wild Hunt*, related names, characters and trademarks belong to their respective rights holders.

The WitcherMMO name identifies this fork and does not imply endorsement by the WitcherOnline authors or CD PROJEKT RED.
