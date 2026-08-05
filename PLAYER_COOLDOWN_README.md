# ZA Debug Tools — Player Party Always Charged v1

Target: Pokemon Legends: Z-A v2.0.2 (`B1F12FD919EAE86A`)

This build adds **Always Charged Moves (Player Party)** to the existing Debug Tools Battle section while preserving **Always Max Mega Energy** and the other original tools.

## Confirmed mapping

Two controlled mapper tests confirmed that the object passed to the cooldown updater at `main + 0x00D18420` is exactly the `ik::PokemonParam*` returned for the active player-party member. The tested captures matched party Slot 0 and Slot 1, and two separate moves on Slot 1 used the same object with independent cooldown floats.

## Safety model

- The hook calls the original cooldown updater first.
- It never calls `GetPlayerParty()` from the hot hook.
- The UI caches up to six party object pointers.
- The hook clears the four floats at `object + 0x64` only when the current object address exactly equals one of those cached pointers.
- Every nonmatching object, including opponents, is left unchanged.
- Cache replacement temporarily disables cooldown writes so the hook cannot observe a partially refreshed set.

## Build

Use `.github/workflows/build_za_cooldown_mapper.yml`. The workflow checks out clean pinned upstream source, overlays these four source files, builds Sail directly from the verified path, and builds `ZA_Debug_Tools_zip_modmanager` with Clang 20.

Artifact name:

`ZA-Debug-Tools-Player-Cooldown-v1`

## First test

1. Disable every other Debug Tools `subsdk9` mod.
2. Remove the Atmosphere cheat text file for the first test.
3. Purge PPTC.
4. Launch the game and load the save completely.
5. Open Debug Tools and expand **Battle**.
6. Enable **Always Charged Moves (Player Party)**. Enabling automatically refreshes the six targets.
7. Confirm the text says the expected number of cached targets, normally `6 / 6`.
8. Deploy one party Pokemon and test several moves.
9. Confirm the player moves recharge immediately.
10. Let an opponent use moves and confirm its cooldown behavior remains normal.
11. Change to another party Pokemon and test again.
12. Toggle **Always Max Mega Energy** and confirm it still works.

After changing the party lineup, press **Refresh Party Targets** before continuing.

The Ryubing log will contain `[PlayerCooldown]` messages when the toggle or target cache changes.
