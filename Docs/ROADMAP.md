# Generals: Zero Hour in Unreal 5.8 — Roadmap

Goal: Skirmish first. The engine reads the **original game data at runtime** (like OpenSAGE):
`.big` archives, INI files, `.map` files, W3D models, DDS/TGA textures. Logic follows EA's GPL
source release (`../_Reference/CnC_Generals_Zero_Hour/GeneralsMD`) and keeps original units
(1 Generals unit = 10 cm in Unreal, 30 logic frames/s).

## Done (M1: map viewer)
- [x] `.big` virtual file system with the original lookup order (ZH dir, then base Generals)
- [x] RefPack / zlib decompression (maps)
- [x] INI tree parser (all 136 INI files + 79 map.ini parse cleanly), Object / ObjectReskin merge
- [x] W3D parser: meshes, materials, hierarchies, HLOD, raw / time-coded / adaptive-delta animations
- [x] Runtime meshes (UStaticMesh per W3D mesh) + runtime textures (DXT kept compressed)
- [x] Materials: opaque / alpha-test / alpha / additive / multiply, house color (HOUSECOLOR meshes, ZHC textures)
- [x] `.map` loader: height map, blend tiles, objects, sides, lighting
- [x] Terrain: tile atlas + 2 blend layers with per-corner alpha, original triangulation
- [x] Map objects placed from their INI draw modules (W3DModelDraw default state, trees, props)
- [x] Original lighting model (ambient + diffuse, 3 global lights), RTS camera matching W3DView
- [x] Test harness: `Tools/gen_test.ps1` (build + offscreen run + screenshot)

## Done (M3a: simulation core)
- [x] Fixed-step logic (`gen.LogicFPS`, default 30) decoupled from rendering; drawables interpolate between
      logic frames, so the game renders at any frame rate (400-600 fps measured)
- [x] Players, factions (PlayerTemplate.ini), skirmish start: Command Center + starting units per faction
- [x] Objects with Body/Physics/AI/Locomotor data from INI; locomotor ports for treads, wheels, legs
- [x] Pathfinder: cliff flags, water polygons, playable bounds, structure footprints; A* + smoothing
- [x] Selection (click, drag box, shift), right-click move with group spreading, S = stop, HUD rings/health bars
- [x] Model condition system: condition states (+aliases, ignore lists) drive model/animation/sub-objects

## Done (M3b: infantry + combat core)
- [x] Infantry: W3D skins CPU-skinned at runtime, animations interpolated between keyframes
      (`gen.AnimInterpolation`), idle / moving / firing / dying condition states
- [x] Weapons from Weapon.ini (damage, radius, range, clips, reload, scatter, projectile vs instant hit),
      weapon sets, armor sets with per-damage-type coefficients (Armor.ini)
- [x] Targeting: idle auto-acquire in weapon range, retaliation, explicit attack order with approach
      (right-click an enemy), best weapon per target
- [x] Turrets (turn rate, recenter), whole-unit aiming for turretless units, muzzle flashes, barrel recoil
- [x] Projectiles: Bezier arcs (DumbProjectileBehavior) and homing missiles, radius damage with
      RadiusDamageAffects, delayed damage for instant-hit weapons
- [x] Body damage states (DAMAGED / REALLYDAMAGED / RUBBLE), death: SlowDeathBehavior selection by
      death type + probability, dying animation, sinking, removal; dead structures free their footprint
- [x] Camera: zoom-out limit raised well past the original (900 vs 310), proportional smooth zoom, no motion blur

## Done (M3c: effects)
- [x] ParticleSystem.ini: all 1084 systems parsed; emission volumes / velocity types, key-framed alpha and
      color, wind, slave systems, per-particle attached systems, streak ribbons. Simulated in the original's
      30 Hz client frames on the simulation clock (pauses / scales with game speed) and interpolated for
      rendering; drawn as camera-facing or ground-aligned quads batched per texture and shader
- [x] FXList.ini: ParticleSystem, Tracer, LightPulse (dynamic point lights), ViewShake (camera), TerrainScorch
- [x] ObjectCreationList.ini: CreateDebris / CreateObject with all dispositions (flying, random force,
      ground aligned, ...); free-body physics port (gravity, bounce, tumble, rest) for debris and hulks
- [x] Hook-ups: weapon FireFX at the model's fire bone, projectiles launched from the launch bone and
      detonating on the victim's hull, ProjectileDetonationFX / OCL, ProjectileExhaust trails,
      SlowDeathBehavior INITIAL / MIDPOINT / FINAL FX + OCL, FXListDie, CreateObjectDie, DestroyDie,
      InstantDeathBehavior, LifetimeUpdate, KillWhenRestingOnGround
- [x] Departures from the original, on purpose: sprites fade softly where they cut into terrain or models,
      and their outer border fades (many source textures aren't clean at the edges); particle cap 12000
      instead of 2500

## Done (M3d: unit collision)
As in the original, units are not shoved apart by physics; they avoid ending up in each other:
- [x] Pathfinder unit layer: every ground unit reserves the cells of its destination and holds the cells it
      is parked on (updateGoal / updatePos); move orders end on the nearest free spot (adjustDestination)
- [x] Collision pass per logic frame (hash grid, bounding circles, then oriented-box overlap) feeding
      AIUpdateInterface::processCollision: blocked units match the speed of the unit ahead or wait, the
      lower-priority unit steps aside in a deadlock, infantry gets out of a vehicle's way, a unit blocked
      by something parked re-paths around parked units
- [x] Attack approach picks a free spot inside weapon range per attacker (adjustTargetDestination), so a
      group lines up to fire instead of piling up; turreted units fire while rolling onto their spot
- [x] Crushing: SquishCollide infantry under any enemy crusher, vehicles under a higher CrusherLevel
- [x] Beyond the original: parked units whose footprints overlap shuffle to free ground (the original
      only did this for units within half a cell of each other)

## Next
- [ ] Effects still missing: sounds (audio milestone), TransitionDamageFX (smoke / fire on damaged units
      and wrecks), DamageFX (hit sparks per armor type), FXListAtBonePos, RayEffect / lasers, heat-haze
      smudges, debris animations and FXFinal, OCL DeliverPayload / FireWeapon / Attack, structure collapse
- [ ] M2 rendering completeness: water (WaterSet + water polygons), roads & bridges, shroud/fog of war,
      map edge / sky
- [ ] M3e logic: attack-move / guard, PreAttackDelay, minimum range, turret pitch, anti-air (once aircraft
      fly), veterancy; collision leftovers: formation-preserving group moves, wide units pathing through
      one-cell gaps, front / back two-stage vehicle crush, walls and fences
- [ ] M4 economy & building: dozers/workers, construction, production queues, supply gathering, power, upgrades
- [ ] M6 UI: control bar from WND + mapped images, command sets/buttons, radar/minimap, cursors, selection decals
- [ ] M7 audio & FX: AudioEvents, voices/EVA, music, particle systems
- [ ] M8 skirmish setup: shell map/menus, player slots, factions & generals, start positions, victory conditions
- [ ] M9 skirmish AI (AIData.ini skirmish build lists + SkirmishScripts.scb)

## Workflow notes
- C++ builds need the editor closed (Live Coding blocks external builds).
- `Tools/ue_setup.py` (re)creates materials, MPC and the Skirmish level:
  `UnrealEditor-Cmd.exe Generals1.uproject -run=pythonscript -script=C:/.../Tools/ue_setup.py`
  (use forward slashes; `\u` in the path is eaten by Unreal's command line parser).
- Command line test switches: `-GenMap=`, `-GenFaction=`, `-GenEnemy=`, `-GenCam=x,y,yaw,height`,
  `-GenObject=T1,T2` (`-GenObjectAt=x,y`), `-GenEnemyObject=T1,T2` (`-GenEnemyAt=x,y`), `-GenTestMove=x,y`,
  `-GenTestAttack`, `-GenTrace` (per-second unit state + fire/hit events), `-GenSeed=N`,
  `-GenPauseFrame=N` (freeze the simulation at a logic frame and screenshot it), `-GenModel=`, `-GenShot=`,
  `-GenTestFX=Name[,Name]` (plays FX lists / particle systems at the camera target at logic frame 30).
  Player input is ignored while `-GenShot` is active, so the real mouse can't scroll the test camera.
- Logic code mirrors the original per-frame formulas; INI rates are converted with the configured logic rate.
