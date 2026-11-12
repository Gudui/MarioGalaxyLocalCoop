# Changelog

## v0.1.0-alpha.1

Initial public alpha release for **Super Mario Galaxy** (Switch 1.0.0, Title ID `010099C022B96000`):

- **Two-player co-op**: spawns Luigi (Player 2) with independent controller input and mutual collision filtering.
- **Permanent split-screen**: dedicated split views with independent Luigi follow camera (`FollowYaw` / `OrbitViewAbout`), 1.5x native distance framing (capped at +800 units), Launch/Sling Star pairing, and per-half EFB bloom copy isolation.
- **Prologue & stage support**: Castle Garden and Gateway Galaxy playable in co-op, synchronized spin unlock, Gateway flip panel support, and nearer-player actor routing.
- **HUD & lives**: independent 1P/2P health meters (`HitPointMeter`) and life counters (`PlayerLeft`) with 1-Up routing.
- **Death isolation & crash guards**: per-player death/respawn handling for damage, abyss, black-hole, sink, and non-stop deaths; emitter pool null guard; missing resource lookup guard; cross-player status close routing.
- **SD logging**: 1-second auto-flush session logging to `sd:/GalaxyCoop/smg1_log.txt`.
