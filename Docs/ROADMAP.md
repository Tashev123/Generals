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

## Done (M3e: damage states, controls, test units)
- [x] TransitionDamageFX: FX lists, object creation lists and bone-attached particle systems when a body
      crosses into DAMAGED / REALLYDAMAGED / RUBBLE (smoke columns on battered tanks, explosions and
      smoke when a building's state drops); ParticleSysBone systems that live while a condition state shows
- [x] Buildings die through their own die modules (FXListDie + CreateObjectDie + DestroyDie): explosion,
      flying debris, removal
- [x] Mouse scheme: left click selects own units / orders the selection (attack the enemy under the
      cursor, else move), left drag box-selects, right click clears the selection, right drag scrolls
- [x] Test battle groups for both sides in front of the player's base until production exists
      (`-GenNoTestUnits` turns them off)

## Done (M4a: production and building)
- [x] Command data: CommandSet.ini, CommandButton.ini, the mapped-image atlases (button icons) and
      generals.csf (button names / tooltips)
- [x] Money (10000 starting cash) and power (EnergyProduction of finished buildings); a power shortage
      slows builds as in ThingTemplate::calcTimeToBuild
- [x] Unit production: ProductionUpdate queue per factory (paid when queued, refunded on cancel), units
      leave by the building's UnitCreatePoint / NaturalRallyPoint; prerequisites (Object lists)
- [x] Construction: dozers / workers place buildings (clear, flat-enough ground, nothing in the way),
      the site starts at 1 hit point as AWAITING_CONSTRUCTION and is raised by a dozer over BuildTime;
      one dozer per building; units and props under the footprint are moved / cleared; unfinished
      buildings can be resumed; selling (3 seconds, SellPercentage refund)
- [x] Command bar (stand-in for the WND control bar, drawn by the HUD): the selected object's command
      set with the real icons and costs, greyed out with a reason when unavailable, production queue with
      progress (click to cancel), tooltips; money and power read-out; building placement with a preview
      model and a green / red footprint (left click places, right click cancels)

## Done (M4b: supply gathering, first flight)
- [x] Supply piles (SupplyWarehouseDockUpdate: boxes in stock, piles that vanish when empty) and supply
      centers (SupplyCenterDockUpdate: every box delivered pays ValuePerSupplyBox)
- [x] Docking as in DockUpdate / AIDock: approach a waiting spot, queue until the dock lets one unit in,
      entry / action / exit positions from the building's DockStart / DockAction / DockEnd / DockWaitingNN
      bones (lanes through buildings that allow it), a box per SupplyWarehouseActionDelay, hand-over after
      SupplyCenterActionDelay
- [x] Gatherers (SupplyTruckAIUpdate, ChinookAIUpdate, WorkerAIUpdate) with the original autopilot:
      loaded -> nearest own supply center, empty -> nearest pile within SupplyWarehouseScanDistance,
      nothing to do -> wait at home and retry; any other order takes them off autopilot, clicking a pile
      or a supply center puts them back on and that dock is preferred from then on
- [x] A finished supply center comes with its gatherer (SpawnBehavior one-shot), and gatherers built at a
      supply center start working by themselves; supply centers keep SupplyBuildBorder clear of piles
- [x] Helicopter flight for air-only locomotors: lift holding PreferredHeight over the terrain, hover
      movement, straight-line "paths", aircraft keep their own destinations apart and ignore ground
      units; the body leans with speed and banks in turns; only anti-air weapons target them in the air
- [x] All model draw modules of an object are shown, not just the first (cargo net under the Chinook,
      the crane of a supply dock); W3DSupplyDraw hides box bones as a pile runs down
- [x] HUD: "$ value" floating up where supplies are delivered; a selected pile shows what it is still
      worth, a selected gatherer its boxes

## Done (M9a: skirmish AI)
- [x] The computer player is driven by the original data: `Data\Scripts\SkirmishScripts.scb` (script
      lists, conditions, actions and team definitions per faction and difficulty) and `AIData.ini`
      (timings, SideInfo, SkirmishBuildList)
- [x] Script engine per AI player: OR-of-AND conditions, one-shot / difficulty / evaluation-delay
      handling, flags, counters and countdown timers, subroutines, false-actions, per-team evaluation
      of scripts that name a team, `<This Team>`, generic team scripts, sequential team scripts
- [x] Base building (AIPlayer / AISkirmishPlayer): the build list is moved and rotated onto the start
      position, dozers / workers build it in order and rebuild what is destroyed, a power plant is
      forced when power runs short, lost dozers are replaced, supply centers go next to the richest
      pile within reach (and follow on to the next pile), base defenses on a ring around the base
      facing the map's approach paths (Center / Flank / Backdoor waypoint paths)
- [x] Economy: the AI keeps the gatherers per supply center that AIData.ini asks for
- [x] Teams: selection by production priority and condition, build queue across the factories,
      recruiting idle units, ready teams run their OnCreate script; success / failure priority changes
- [x] Team orders used by the skirmish scripts: follow the approach path to the enemy, hunt, guard /
      attack an area (inner / outer perimeter, combat zone polygon triggers), move, merge, disband
- [x] Map data for it: polygon triggers, waypoint path labels and waypoint links
- [x] `-GenDifficulty=Easy|Normal|Hard`, `-GenNoAI` (passive enemy, as before); with an AI the enemy no
      longer gets a free battle group, it raises its own
- [x] Upgrades, promotions and special powers (see M4c): the computer player spends its general's points
      on one of its side's SkillSets, researches the upgrades its scripts ask for, uses upgrade and
      unit-build command buttons on its teams, and fires its powers at the most valuable spot of the
      enemy base (AIPlayer::computeSuperweaponTarget)
- [ ] M9b, not done yet: script actions for unit abilities (hunt with a command button), garrisoning,
      transports and combat drops, capturing, tunnel networks, attack priority sets (these are no-ops),
      conditions on named map objects (always false), the soldiers inside Stinger sites, alliances and
      more than one enemy, victory / defeat
## Done (M5a: jets and airfields)
- [x] Airfields (ParkingPlaceBehavior): parking spaces, hangars and runways from the model's bones, one
      space per plane (taken when the plane is ordered, so an airfield builds only what it can park),
      runway reservations with a second plane queueing behind the first, repairs for whatever is parked,
      hangar doors that stand open while their space is in use, planes on the ground die with the airfield
- [x] Planes (JetAIUpdate): built in the hangar, taxi to their space, and on an order taxi out, pause with
      afterburners, take off (pairs on the two runways go together), carry the order out, fly home when
      out of ammunition, when the target is dead or after ReturnToBaseIdleTime, wait for the runway, land,
      taxi back, reload; hunt and guard orders resume after reloading; orders given during takeoff or
      landing wait; with the airfield gone they look for another and otherwise circle and lose health
- [x] WINGS locomotor: minimum speed (no stopping: circling at CirclingRadius while idle, arrival when
      abeam of the goal), takeoff lift that grows along the runway, exact heights and the extra friction
      and lift of "ultra accurate" mode for takeoff and landing, nose up / down with the climb rate
- [x] Attack passes: a plane flies at its target, fires when it is ahead and in range, breaks away when
      inside the weapon's minimum range; clips that only reload at base (AutoReloadsClip = RETURN_TO_BASE)
- [x] Helicopters built at an airfield appear on the helipad; land order (click an own airfield with
      aircraft selected) for planes and helicopters, helicopters take off again once repaired
- [x] Shot down: JetSlowDeathBehavior (rolls, falls, hits the ground, blows up; on the ground it just
      blows up) and HelicopterSlowDeathBehavior (spirals down spinning, rotor comes off, hull left behind)
- [x] AirborneTargetingHeight (above it only anti-air weapons reach an aircraft), locomotor sets
      (taxiing / normal, the Aurora's supersonic attack and sluggish return sets)
- [x] HUD: ammunition of the selected plane; a build button says when the airfield is full
- [ ] Not done yet: stealth (Stealth Fighter), countermeasure flares, the Aurora's "attackers miss"
      bonus, lock-on delay, airfield rally point for helicopters, guard-airspace command, aircraft
      carrier decks, ejecting pilots, Chinook / Helix transport duty and combat drop
## Done (M4c: upgrades and general's powers)
- [x] Upgrade.ini, Science.ini, Rank.ini, SpecialPower.ini; upgrade buttons research at the building
      (paid when queued, refunded on cancel, BuildTime regardless of power), player upgrades reach
      everything the player owns or builds later, object upgrades only their building or unit
- [x] Upgrade modules (UpgradeMux: TriggeredBy any / all, ConflictsWith): weapon set, armor set, max
      health, locomotor set, power plant control rods, command set, model condition, sub-objects,
      granted science, weapon bonus (WeaponBonus = PLAYER_UPGRADE DAMAGE ...), auto heal, unpausing a
      special power; the Supply Lines income bonus
- [x] Ranks and promotions: skill points for what a player destroys (SkillPointValue / ExperienceValue),
      five ranks with their purchase points, sciences with prerequisites; units and buildings that need
      a science (Paladin, Stealth Fighter, Nuke Cannon ...) unlock when it is bought
- [x] Special powers (SpecialPowerModule): shared per-player timers that are ready the moment the science
      is bought, own timers for buildings; OCLSpecialPower with science upgrades (A10 strike 1-3,
      paradrop 1-3 ...) and the CreateLocation rules, CashHackSpecialPower, CashBountyPower
- [x] DeliverPayloadAIUpdate: aircraft that fly in from the map edge in formation, open up at the
      delivery distance, drop their payload or fire what hangs under their wings, dive and strafe (A10),
      come round again or leave; bombs that fall with the aircraft's speed, go off at a height
      (HeightDieUpdate) and "fire" their weapon where they die (FireWeaponWhenDeadBehavior); parachutes
      (free fall, canopy opens, steers for the target, sets the soldier down); DeletionUpdate;
      SmartBombTargetHomingUpdate is in but no vanilla power tested here uses it
- [x] Working powers: A10 Thunderbolt strike, paradrop, daisy cutter, emergency repair, carpet
      bomb, artillery barrage, EMP pulse (disables vehicles and base buildings, drops aircraft), cash
      hack, rebel ambush, anthrax bomb (with its lingering cloud: FireWeaponUpdate), cash bounty
- [x] Missiles fly on their own locomotor (MissileAIUpdate: launch speed until ignition, then full
      speed; the weapon's speed only with UseWeaponSpeed); radius damage spares the launcher's producer
      and, for NOT_AIRBORNE weapons, whatever is in the air
- [x] Interface: upgrade buttons (price, done marker, why not), special power buttons with a recharge
      clock in the building's command bar and in a shortcut column on the right, targeting mode with
      the power's radius on the ground, the general's button (rank, points, progress) and its
      promotions panel, tooltips with the game's own descriptions
- [ ] Not done yet: what ObjectCreationUpgrade makes (drones, the Overlord's and Helix's add-ons),
      stealth / radar / veterancy upgrades, mines (cluster mines power, the mines upgrade), sneak attack
      (tunnels), frenzy, GPS scrambler, spy satellite / spy drone / CIA intelligence (need fog of war),
      leaflet drop, Spectre gunship, the superweapon buildings (particle cannon, nuke, SCUD storm),
      units' own abilities (SpecialAbilityUpdate), the look of an EMP'd object, poison that lingers on
      its victims, the delivery decal on the ground. Their buttons are shown greyed out.

## Done (M4d: rally points and repair)
- [x] Rally points (ExitInterface): buildings of KindOf AUTO_RALLYPOINT take one; new units drive out by
      the door and then on to it, each ending on the nearest free spot (only what moves on the ground,
      as in the original; a supply center's gatherers go there before they start work); helicopters
      fly to their airfield's rally point; a spot that can't be reached on foot is refused
- [x] Dozers and workers mend buildings (DOZER_TASK_REPAIR): RepairHealthPercentPerSecond of the
      building's health per second, free of charge, one repairer per building (sole healing
      benefactor); a dozer idle for BoredTime mends the nearest damaged building within BoredRange by
      itself (computer players' dozers too)
- [x] Vehicles are mended at war factories (RepairDockUpdate) with the docking sequence of the supply
      docks: they queue on the waiting spots, drive onto the pad one at a time and are whole again
      after TimeForFullHeal however bad the damage
- [x] Controls: with only factories selected, left click the ground to set their rally point (shown as a
      line and a marker while the factory is selected); with dozers selected, left click a damaged
      building; with damaged vehicles selected, left click a war factory of yours
- [ ] Not done yet: bridge repair, the computer player's repair queue for bridges, repair cursors

## Done (M5b: garrisons and transports)
- [x] Containers (OpenContain and what is built on it): who may enter what (ActionManager::canEnterObject:
      allowed / forbidden kinds, relationship, slots, nobody else's occupied building, no side's base
      building but one's own), the walk there (AI_ENTER), occupants hidden, untargetable and carried
      along, exits by the ExitStart / ExitEnd bones one ExitDelay apart, "evacuate"
- [x] TransportContain: slots (a vehicle takes several), passengers that come with the vehicle
      (Troop Crawler, Listening Outpost), infantry firing out (Humvee, Battle Bus, Listening Outpost),
      healing on board (Troop Crawler, ambulance), the LOADED condition, armed riders switching the
      transport to its upgraded weapon set; a destroyed transport hurts or kills its passengers
      (DamagePercentToUnits) and the survivors scatter
- [x] GarrisonContain: civilian buildings, bunkers, fire bases, the palace; an occupied building takes
      its occupants' side and colours and goes back when emptied; occupants shoot from the window
      (FIREPOINT bone of the model showing) nearest their target with the GARRISONED range and damage
      bonus from GameData.ini; a building beaten down to "really damaged" turns them out
- [x] HealContain: wounded infantry walk into a barracks, are whole after TimeForFullHeal and come out
- [x] TunnelContain: all tunnels of a player share one space (MaxTunnelCapacity units, vehicles too):
      in at one, out at any other; occupants heal; the last tunnel caving in buries them
- [x] Chinook (ChinookAIUpdate): lands when something wants to get on or off, takes off afterwards
- [x] BuildVariations: a template that only lists variations (the Technical's three chassis) becomes one
      of them when it is made, and counts as the template it was asked for
- [x] Interface: the container's command bar lists its occupants (click one to let it out) and has
      "evacuate"; garrisoned buildings without a command set get the same inventory; "n / max" over a
      selected container; left click a container with units selected to send them in
- [x] Computer players: TEAM_LOAD_TRANSPORTS, TEAM_GARRISON_NEAREST_BUILDING, TEAM_EXIT_ALL
- [ ] Not done yet: combat drop (rappelling), Overlord / Helix bunkers and other add-ons
      (OverlordContain), the Combat Cycle's riders, hackers in the Internet Center, units standing in
      view on a fire base (IsEnclosingContainer = No: they are hidden like the rest), clearing a
      garrison (flash-bangs, toxins, flame), stealth garrisons, TEAM_ENTER_NAMED (named map buildings),
      guarding from the tunnels, the unload-when-damaged scripts (UNIT_HEALTH on <This Object>),
      enter / exit sounds and cursors

## Done (M3f: attack-move, guard, veterancy, victory)
- [x] Attack-move (AIAttackMoveToState): to the goal, stopping to fight whatever comes within vision range
      on the way, then on again; the order ends on arrival
- [x] Guard for the player (AIGuardMachine with GuardInnerModifierHuman / GuardOuterModifierHuman): a spot,
      or an object that is then escorted; the GUARD_WITHOUT_PURSUIT and GUARD_FLYING_UNITS_ONLY buttons
- [x] Veterancy (ExperienceTracker): experience for kills by the victim's level, ExperienceRequired,
      veteran / elite / heroic with their weapon sets, the VETERAN / ELITE / HERO weapon bonuses of
      GameData.ini (damage, and rate of fire: applied to the delay between shots and to reloads), more
      health per level (HealthBonus_*), the level's upgrade (self-repair of elite units), units made
      as veterans by a science (VeterancyGainCreate), chevrons over the unit
- [x] Victory (VictoryConditions, VICTORY_NOBUILDINGS): a player with no building left that counts
      (MP_COUNT_FOR_VICTORY) is defeated and everything it still has dies; with one alliance left the
      game is decided and says so
- [x] Controls: the attack-move and guard buttons (or A / G), then a left click on the spot (for guard
      also a unit or building to escort); right click cancels
- [ ] Not done yet: the score screen and going back to a menu, the level-gain animation and promotion
      sounds, experience shared with a carrier (ExperienceSink), HORDE / NATIONALISM and the other
      area weapon bonuses, moods (aggressive / alert) and attack priorities during attack-move, the
      difficulty weapon bonuses of the computer player

## Next
- [ ] Economy leftovers: terrain flattening under buildings, construction scaffolding animation
      timing, door animations, multiple-factory build bonus, hackers / black markets / oil derricks
- [ ] Aircraft leftovers: see M5a; water as flight surface
- [ ] Effects still missing: sounds (audio milestone), DamageFX (hit sparks per armor type),
      FXListAtBonePos, RayEffect / lasers, heat-haze smudges, debris animations and FXFinal,
      OCL DeliverPayload / FireWeapon / Attack, StructureCollapseUpdate / topple, rubble left behind
      (KeepObjectDie), FlammableUpdate
- [ ] M2 rendering completeness: water (WaterSet + water polygons), roads & bridges, shroud/fog of war,
      map edge / sky
- [ ] M3e logic: PreAttackDelay, minimum range, turret pitch; collision leftovers: formation-preserving group moves, wide units pathing through
      one-cell gaps, front / back two-stage vehicle crush, walls and fences
- [ ] M6 UI: control bar from WND + mapped images, command sets/buttons, radar/minimap, cursors, selection decals
- [ ] M7 audio & FX: AudioEvents, voices/EVA, music, particle systems
- [ ] M8 skirmish setup: shell map/menus, player slots, factions & generals, start positions, score screen

## Workflow notes
- C++ builds need the editor closed (Live Coding blocks external builds).
- `Tools/ue_setup.py` (re)creates materials, MPC and the Skirmish level:
  `UnrealEditor-Cmd.exe Generals1.uproject -run=pythonscript -script=C:/.../Tools/ue_setup.py`
  (use forward slashes; `\u` in the path is eaten by Unreal's command line parser).
- Command line test switches: `-GenMap=`, `-GenFaction=`, `-GenEnemy=`, `-GenCam=x,y,yaw,height`,
  `-GenObject=T1,T2` (`-GenObjectAt=x,y`), `-GenEnemyObject=T1,T2` (`-GenEnemyAt=x,y`), `-GenTestMove=x,y`,
  `-GenTestAttack`, `-GenTrace` (per-second unit state + fire/hit events), `-GenSeed=N`,
  `-GenPauseFrame=N` (freeze the simulation at a logic frame and screenshot it), `-GenModel=`, `-GenShot=`,
  `-GenTestFX=Name[,Name]` (plays FX lists / particle systems at the camera target at logic frame 30),
  `-GenTestBuild=Building` (first local dozer constructs it at the camera target), `-GenTestQueue=Unit[,Unit]`
  (queued at the local building that produces it), `-GenTestSelect=Template` (selects it, so the command
  bar shows in the screenshot; other players' objects too, nearest the camera target),
  `-GenTestGather=Frame` (at that logic frame all local gatherers are sent to the supply pile nearest the
  camera target), `-GenSpeed=N` (simulation speed, up to 8x: fast-forward through build times and supply
  runs; `-GenTrace` adds `[supply f..]` lines), `-GenNoTestUnits` / `-GenTestUnits` (default battle group
  off / on), `-GenTestJets="Frame,order[,x,y];..."` (at that logic frame every local aircraft gets the order:
  attack the enemy nearest x,y / move to x,y / land at the nearest own airfield / stop; `-GenTrace` adds `[jet f..]`
  state changes and `[jetpos f..]` positions during takeoff and landing),
  `-GenTestPowers="Frame,what[,args];..."` (the local player's upgrades, promotions and powers, in turn:
  `upgrade,Name` / `skill,Points` / `science,Name` / `power,Name[,x,y]` / `money,Amount` / `generals`
  (opens the promotions panel) / `click,CommandButton` and `target,x,y` (the interface's own click path) /
  `aiskill,Points` (experience for every computer player) / `rally,Type,x,y` / `damage,Type,Percent` /
  `repair,Type` (our dozers mend it) / `fix,Type` (our damaged vehicles go there for repairs) /
  `attackmove,x,y` / `guard,x,y` (everything of ours that moves and fights) / `vet,Type,Level` /
  `destroy,Type,x,y` (anybody's object of that type nearest the spot) /
  `enter,Unit,Type[,x,y]` (our units of that type, or `*`, get into the container of that type nearest the
  camera target or x,y) / `evacuate,Type[,x,y]`; `-GenTrace`
  adds `[upgrade f..]`, `[science f..]`, `[power f..]`, `[rally f..]`, `[repair f..]`, `[contain f..]`, `[vet f..]`, `[victory f..]` lines and rank / points / weapons to the status lines),
  `-GenDifficulty=Easy|Normal|Hard` and `-GenNoAI` for the computer player (`-GenTrace` adds
  `[ai f..]` lines: what it builds, the teams it starts, a status line per second). `-GenObject=` also places finished buildings, e.g. a supply center next to a pile.
  Player input is ignored while `-GenShot` is active, so the real mouse can't scroll the test camera.
- Logic code mirrors the original per-frame formulas; INI rates are converted with the configured logic rate.
