# ZA Debug Tools — Cooldown Object Mapper v1

This is a **read-only diagnostic build** for Pokémon Legends: Z-A **v2.0.2** (Build ID beginning `B1F12FD919EAE86A`). It does not remove cooldowns yet. Its purpose is to identify the reliable link between the six player-party Pokémon exposed by Party Inspector and the live battle objects whose four cooldown floats are stored at `object + 0x64`.

## What this build adds

Under **Windows → Cooldown Object Mapper**, the mod can:

- refresh pointers for party slots 0–5 using the same `cmn::GameData::GetPlayerParty()` path as Party Inspector;
- arm one player capture and one opponent capture;
- detect the next cooldown increase after a move is used;
- record the live cooldown object, its vtable, its four cooldown values, and all aligned qwords within the verified `0xA8`-byte object;
- compare those object values against party pointers and stable 32-bit party identifiers;
- write a complete snapshot between `[CooldownMapper]` and `[/CooldownMapper]` markers in the Ryubing log.

## Safety choices

- The mapper calls the original cooldown function and never writes cooldown values.
- The hook is installed through a Sail symbol restricted to game version `202` rather than blindly using the offset on other versions.
- The first instructions at the hook location are PC-independent instructions, avoiding the branch/ADRP relocation limitation of LibHakkun trampolines.
- Object reads stop at `+0xA0`; the constructor allocation path for this concrete object requests `0xA8` bytes.
- Captures use atomics and a sequence lock so the UI cannot display a partially written record.
- Party APIs are called only from the UI thread, never from the battle hook.

## Build with GitHub Actions

Use the **overlay ZIP** with the same Git repository/fork that already built Debug Tools successfully. Extract it over the repository root and allow it to replace the listed source files. The dedicated workflow is added under a new filename, so it does not overwrite the repository’s existing build workflow. This preserves the repository's submodule gitlinks.

For a fresh local repository, begin with a recursive clone of the original project, extract the overlay over that clone, commit, and push:

```bash
git clone --recursive https://github.com/Martmists-GH/switch-mods.git
cd switch-mods
# Extract the overlay contents over this directory.
git add .
git commit -m "Add ZA cooldown object mapper"
git push
```

Then:

1. Open **Actions → Build ZA Cooldown Mapper → Run workflow**.
2. Download the `ZA-Debug-Tools-Cooldown-Mapper-v1` artifact.
3. Extract/install the produced mod-manager ZIP in Ryubing.

The included dedicated workflow (`.github/workflows/build_za_cooldown_mapper.yml`) uses Clang 20 and `RelWithDebInfo`, matching the toolchain that previously linked this project successfully. The full-source ZIP is a review snapshot; because ZIP archives do not preserve Git submodule gitlinks, the overlay applied to a real recursive clone/fork is the recommended build path.

## Clean test procedure

1. Close Ryubing completely.
2. Disable the original Debug Tool mod and every earlier custom Debug Tools build.
3. Enable only this Cooldown Mapper build.
4. Disable the Dynamic 60 FPS mod.
5. Temporarily move `B1F12FD919EAE86A.txt` out of the game’s cheats folder. Do not merely uncheck entries; earlier logs showed the full cheat file being executed.
6. Purge the PPTC cache after changing the enabled mod.
7. Launch the game and deploy one of your Pokémon near an opponent.
8. Open **Windows → Cooldown Object Mapper**.
9. Confirm that all six party slots show nonzero pointers and plausible species/form values.
10. Wait about one second with the Pokémon deployed.
11. Click **Arm PLAYER capture**, close the menus, and use exactly one move.
12. Reopen the mapper. A PLAYER object and cooldown jump should now be shown.
13. Click **Arm OPPONENT capture**, close the menus, and let the opponent use exactly one move.
14. Reopen the mapper and click **Write snapshot to Ryubing log**.
15. Close the game normally and upload the resulting log.

Useful log markers:

```text
[CooldownMapper] Armed PLAYER capture
[CooldownMapper] Captured PLAYER ...
[CooldownMapper] Armed OPPONENT capture
[CooldownMapper] Captured OPPONENT ...
[CooldownMapper]
...
[/CooldownMapper]
```

## Expected next step

The snapshot should reveal either a direct party pointer/identifier inside the player battle object or a stable field that differs between player and opponent. We can then make a second diagnostic that follows only that specific, validated field. Cooldown removal will not be enabled until player matching consistently accepts player objects and rejects opponent objects.
