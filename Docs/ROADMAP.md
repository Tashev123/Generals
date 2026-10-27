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
- [x] Done since: the Stealth Fighter's stealth (M6b), the airfield's rally point for helicopters (M4d),
      Chinook / Helix transport duty (M5b); flares, the Aurora's "attackers miss", ejecting pilots and the
      combat drop are M5c below
- [ ] Not done yet: aircraft carrier decks (FlightDeckBehavior: campaign only), the guard-airspace
      command of jets; LockonTime (a delay before anti-air may fire at a jet) is in the code but no
      object in the game's data uses it
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
- [ ] Not done yet (what ObjectCreationUpgrade makes is in M4j; mines, frenzy, the leaflet drop,
      the gunship and the sneak attack in M4i; the superweapon buildings in M4h):
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
- [ ] Not done yet (the combat drop is M5c): Overlord / Helix bunkers and other add-ons
      (OverlordContain: M4j), the Combat Cycle's riders, hackers in the Internet Center, units standing in
      view on a fire base (IsEnclosingContainer = No: they are hidden like the rest), (clearing a
      garrison: M4l) stealth garrisons, TEAM_ENTER_NAMED (named map buildings),
      guarding from the tunnels, the unload-when-damaged scripts (UNIT_HEALTH on <This Object>),
      cursors

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
- [ ] Not done yet: the level-gain animation,
      experience shared with a carrier (ExperienceSink), HORDE / NATIONALISM and the other
      area weapon bonuses, moods (aggressive / alert) and attack priorities during attack-move, the
      difficulty weapon bonuses of the computer player

## Done (M6a: fog of war and radar)
- [x] Fog of war (the shroud half of PartitionManager): per player and per 40-unit cell a count of who
      is looking; objects look as far as their ShroudClearingRange (a construction site: its own
      footprint), for their owner and its allies; what they saw lingers 5 s (UnlookPersistDuration);
      nothing looks out of a transport or a tunnel, a garrison does. A skirmish map starts explored
      (Multiplayer.ini UseShroud = No): the terrain is known, what moves on it is not
- [x] What a player sees of an object (PartitionData::getShroudedStatus): in view, partly in view,
      remembered (a building seen before, shown the way it was last seen), or hidden. A building
      destroyed out of sight stays on screen until somebody looks there again (ghost objects). Things
      that leave the view stay drawn for 2 s (5 s if dying). Hidden things can't be clicked, show no
      health bars or chevrons, play no effects and emit no particles
- [x] On screen (W3DShroud): one texel per cell, 255 / 127 / 0 for in view / fogged / unexplored and
      beyond the map's edge, eased over a second, bilinear; applied by a post-process material
      (M_Shroud) from each pixel's world position, in gamma space like the original's multiply
- [x] Radar (Radar.cpp / W3DRadar): the minimap in the bottom left corner - terrain in its own colours,
      darker where it lies low, water in the radar water colour; a 2 x 2 blip per object by its
      RadarPriority (buildings under units, the player's own on top, only what is in view); the fog of
      war over it; the box of what the screen shows; events (under attack, construction and upgrade
      finished) as spinning triangles, with the "under attack" messages, at most one per 10 s
- [x] Having a radar (Player::hasRadar): RadarUpgrade modules (USA: every command center; China: the
      radar upgrade; GLA: a radar van), off during a power shortage unless DisableProof, gone with the
      building; the dish unfolds (RadarUpdate)
- [x] DynamicShroudClearingRangeUpdate, and with it the powers that waited for the fog: spy satellite,
      radar van scan, spy drone, CIA intelligence (the player sees what the enemy sees for a while).
      A defeated player sees the whole map
- [x] Controls: left click or drag on the radar looks there; while aiming an order or a power a left
      click on the radar gives it at that spot; Space jumps to the last radar event
- [ ] Not done yet: shroud generators (ActiveShroudUpgrade), spy vision limited to kinds of units (the satellite hack of
      the Internet Center), bridges and roads on the radar, hero icons, beacons, the radar's own frame
      art, the grid decals of the spy satellite / drone, right click on the radar as a move order

## Done (M6b: stealth and detection)
- [x] StealthUpdate: innate stealth or stealth given by an upgrade (StealthUpgrade: camouflage, camo
      netting) or by a power; StealthDelay; the forbidden conditions (attacking, moving above
      MoveThresholdSpeed, firing the named weapon, taking damage, riders attacking, no black market);
      nothing is stealthed inside a transport, a garrison's occupants are
- [x] What stealth means: an enemy's stealthed unit is not drawn, can't be clicked, is not on the radar,
      is no target (auto-acquire, attack orders, attacks under way end), doesn't show its weapon fire,
      doesn't block a building site; a stealthed unit doesn't open fire by itself unless its data says so
      ("AutoAcquireEnemiesWhenIdle = Yes Stealthed") and doesn't shoot back until it is detected
- [x] StealthDetectorUpdate: every DetectionRate, what is stealthed within range (of the kinds asked for)
      is shown up until the next look; idle enemies nearby attack at once; radar events and messages
      for a unit found and for one of ours being found
- [x] The look (Drawable::setStealthLook): our own stealthed units are faint and pulse between
      FriendlyOpacityMin and solid, a detected one glows red; done per object with custom primitive
      data and a dither pattern in the W3D material (the lighting stays that of the solid model)
- [x] GPS scrambler (GrantStealthBehavior: a circle growing from 20 to 100 that stealths the infantry
      and vehicles it passes over), through the stealth module every object inherits from
      Default\Object.ini's DefaultThingTemplate
- [x] Also from DefaultThingTemplate: elite and heroic units mend themselves (the inherited
      AutoHealBehavior); the three veterancy upgrades the game makes in code now exist
- [ ] Not done yet: disguises (the bomb truck), a garrison of stealthed units looking empty to the enemy,
      a carrier taking its rider's stealth (combat bike), demo traps going off (mines: M4i), the
      detectors' sonar ping effects, HintDetectableConditions, the heat-vision look proper (a tint here)

## Done (M7a: sound)
- [x] The data: AudioSettings.ini, SoundEffects.ini, Voice.ini, Speech.ini and Music.ini (each kind on top
      of its DefaultSoundEffect / DefaultDialog / DefaultMusicTrack), MiscAudio.ini, Eva.ini - 4048
      events. The sounds come out of the archives as they are: WAV (PCM and all the IMA ADPCM kinds in
      use) decoded by the game, music MP3 decoded frame by frame while it plays; the localized folder
      first. Of 6451 files the events name, 6368 are there and decode (the rest aren't in the archives)
- [x] What is heard (GameAudio.cpp, GameSounds.cpp, the deciding parts of MilesAudioManager.cpp): who an
      event is for (player / allies / enemies / everyone); sounds in the world not played beyond their
      MaxRange or where the local player can't see; one line at a time per unit; each event's Limit
      (an interrupting event takes the place of its oldest: a Quad Cannon's rattle); the channel
      counts of AudioSettings.ini (25 world, 4 interface, 3 speech), where the less important gives
      way; volume = event x random shift x the kind's volume, falling off as MinRange / distance;
      the microphone near the spot the camera looks at; louder zoomed in; what has become too faint
      gives up its channel
- [x] Sounds that last: attack / loop / decay parts; an object's ambient sound by damage state (a
      one-shot plays once, a loop is picked up again when the camera comes back), a building site, the
      sound of setting off or of moving (and their badly-damaged versions), weapons that fire in a
      stream (FireSoundLoopTime); a jet's engines are off while it is parked; a helicopter going down
- [x] Where sounds come from: weapons, the Sound part of FX lists (explosions, deaths), the voices
      that answer orders (select, move, attack, attack aircraft, guard, enter / garrison / enter
      hostile / get healed, supply, repair, build, unload - chosen as CommandXlat.cpp does), the two
      creation voices, damage states, promotions, getting into and out of transports, stealth on and
      off, research done, special powers, radar on / off, under attack, stealth found, money
      delivered, the interface's click and its refusal
- [x] The adviser (Eva.cpp): one line at a time by priority, not more often than its event allows,
      dropped when stale, in the local side's voice. Wired so far: low power, insufficient funds, base
      under attack, building lost, unit lost, upgrade complete, general's promotion
- [x] Music: the local side's skirmish tracks in turn, the first ten seconds in
- [x] In Unreal: every sound goes to the mixer as a stereo stream the game has already placed between
      the speakers (as the original's default "fast 2D positional" sound did; Unreal's own 3D panner
      depends on the speakers it believes it has); a limiter on the main mix keeps a volley of guns
      from clipping (the original's mixer just clipped)
- [x] Fixed on the way: a helicopter shot down while still climbing went on up for a quarter of a
      minute (its fall now follows the locomotor's lift rule: no lift on the way up, what is left of
      it on the way down)
- [ ] Not done yet: volume settings in the interface; most of the adviser's 49 events (superweapons,
      ally under attack, stolen / sabotaged buildings, heroes detected, beacons); the voices of
      special abilities and moods (crush, fear, task complete, eject, weapon modes, hack, group
      select); sounds placed or scripted on maps; turret and door sounds; the morning / evening /
      night variants of sounds; menu, victory and defeat music; speech that may not be interrupted;
      surround output
- Not listened to: test runs render sound with Unreal's offline device (nothing reaches the speakers)
  and are checked from recordings - decoder output against a second decoder sample by sample, and in
  the mix each sound's time, match, level and left / right balance. Playback through the sound card
  and the mouse-driven voice paths are untested

## Done (M8a: skirmish menu and score screen)
- [x] The game now starts in the skirmish menu (SkirmishGameOptionsMenu): eight slots - the player, and
      for each of the others open / closed / easy, medium or hard army - each with colour, side (the
      twelve armies, or random) and team; the map with its picture, start positions, supply docks and
      tech buildings; starting cash (the choices of Multiplayer.ini). A click on a start position
      hands it round as the original does; colours can't be taken twice; a map with fewer start
      positions closes the slots it has no room for
- [x] The map selection (SkirmishMapSelectMenu): the 47 multiplayer maps of Maps\MapCache.ini by name,
      with their number of players and their own preview picture
- [x] Starting (GameLogic::startNewGame): random sides and colours are drawn; random start positions
      put the first player anywhere, team mates next to each other and everybody else as far from the
      rest as possible; computer players get the level of their slot; "Loading", then the game.
      The setup is kept in the user's settings and is there again next time
- [x] The score (ScoreKeeper): units and buildings built, lost and destroyed (by the SCORE kinds, as
      the original counts them), supplies collected, money spent on buildings and upgrades
- [x] After the game (ScoreScreen): four seconds of "victory" / "defeat", then the score screen on the
      side's own backdrop with its music - everybody who played, best score first, in their colours;
      "continue" goes back to the menu. Defeated while others fight on: "return to game" to watch
- [x] During the game: Esc, F10 or the MENU button (top right) stops the game and offers return,
      restart (the same setup again) and exit (to the score screen). The menu's music is the shell's
- [x] All twelve sides can be chosen. Of the nine generals' armies three have been tried: an Air Force
      General player (its own base and command bar) and Stealth and Toxin General computer players
      (they build up with their own buildings and attack); the other six are untested
- [ ] Not done yet: the menus are drawn by the game's own simple interface code, not from the
      original's window files (SkirmishGameOptionsMenu.wnd, ScoreScreen.wnd, QuitMenu.wnd) - the same
      choices from the same data, a different look; the main menu and everything else in the shell;
      limit superweapons and game speed; observers; the load screen with the
      generals' portraits; battle honours and the advice of the score screen; options (sound,
      resolution, scroll speed)
- Not the original's: "Start with a battle group" in the menu (on by default: the test army this
  project gave the player before production existed; switch it off for a proper game)

## Done (M2b: water, roads and bridges)
- [x] Standing water (W3DWater's renderWater): every water area of the map, drawn on the terrain's own
      grid, its opacity at each point the depth there over TransparentWaterDepth - shores show through,
      deep water has the map's full opacity. The texture drifts and wobbles, sparkles on top. Water.ini
      and the map's own map.ini (colour, texture, opacity, depth; the radar's water colour too)
- [x] Rivers: the ribbon between a river polygon's banks, its texture flowing along it, foam and
      opacity from the edge texture
- [x] The water's shade is worked out as the original does (the terrain's lights on a flat surface,
      times the WaterSet's colour for the time of day), so it follows the map's lighting
- [x] Roads (W3DRoadBuffer), to the letter: the map's road point pairs chained up, corners rounded in
      30 degree steps (or tight, or mitred where the map says so), T-, Y-, slanted-T and four-way pieces
      where three or four of a kind meet, the fading end piece where a road is flagged as joining
      another kind (which also puts it on top), every piece with its part of the road type's texture
      (Roads.ini: 63 kinds - asphalt, dirt, sidewalks, rails, parking lines, helipads, words ...)
- [x] Roads lie on the terrain: each piece is cut along the terrain's triangles, lit and shadowed like
      the ground under it; pieces that overlap are stacked in the original's drawing order
- [x] Bridges (W3DBridgeBuffer): the bridge type's model - left end, spans, right end - laid between
      the map's two bridge points, as many spans as fit, stretched to end on the points; lit like the
      terrain; on the radar in the type's colour
- [x] Bridges in the game logic (TerrainLogic's Bridge, the pathfinder's layers): every bridge has its
      own cells over the ground's; a path steps onto a bridge and off it only at its two ends; ground
      too close under a deck is closed. So units cross on the deck while others pass below (viaducts).
      Units know which layer they are on and stand at the deck's height; destinations, parked units
      and unit-to-unit collisions are kept apart by layer. An order given on a deck goes to the deck
- [x] Tried on: Tournament City / Urban, Alpine Assault, Heartland Shield, Mountain Guns, Twilight
      Flame, Lone Eagle, Winding River (there the computer player's attack waves cross the bridges)
- Bridges are indestructible, as in Zero Hour itself: its GenericBridge object has an ImmortalBody and
  the towers that repaired bridges in Generals are compiled out for map bridges ("since they aren't
  destructable, don't need towers"). Only a campaign script could break one; skirmish has none that do
- [ ] Not done yet: AdditiveBlending water, the water's sky reflection and the moving water grid -
      none of the skirmish maps uses them (WaterType 0, no map.ini overrides; see M2c); the clouds
      over bridges (M2c)
- Different from the original on purpose: its roads hang a little above the highest ground under them
  and are blended in drawing order; here they are draped on the ground and their texture's alpha is a
  dithered mask (so shadows fall on roads, and nothing floats or flickers)

## Done (M4e: capturing buildings, tech buildings, their incomes)
- [x] Capture Building (SpecialAbilityUpdate): Rangers, Red Guards and Rebels once their side has
      bought the upgrade at the barracks ($1000, 30 s). The unit walks up to the building, turns to
      it, unpacks, raises its flag for 20 s while the building flashes, then the building is its
      player's; the flag is packed away and the power recharges (15 s). Any other order calls it off.
      What may be captured is ActionManager's canCaptureBuilding: a finished structure of an enemy, or
      a capturable one of nobody's, not garrisoned, not being sold, not hidden by stealth
- [x] A building changing hands (Object::defect, onCapture): what it was building is cancelled and
      paid back, whoever is inside is put out, parked aircraft go with their airfield, its upgrades
      are looked at again for the new owner, power and radar follow, the score counts it. A computer
      player sells an enemy's base building it has taken, as the original's does
- [x] Tech buildings: Oil Derrick ($1000 when taken, then $200 every 12 s), Oil Refinery (vehicles 10%
      cheaper - prices on the buttons, in the queue, on a sale and for the computer player all go
      through one cost function), Reinforcement Pad (a vehicle of the owner's side by parachute every
      2 minutes), Artillery Platform (fires for whoever holds it). Destroyed, they are nobody's again
      and their rubble stays (TechBuildingBehavior, KeepObjectDie)
- [x] The same modules elsewhere: GLA Black Market ($20 every 2 s, AutoDepositUpdate), USA Supply Drop
      Zone (OCLUpdate: a cargo plane drops six crates every 2 minutes, each $250 to the building that
      picks it up - MoneyCrateCollide); the objects of Crate.ini are loaded
- [x] Red Guards come out in pairs for one price (ProductionUpdate's QuantityModifier)
- [x] The computer player: buys the capture upgrade (AI_PLAYER_BUILD_UPGRADE), sends its capture
      teams (TEAM_HUNT_WITH_COMMAND_BUTTON / CommandButtonHuntUpdate: the nearest capturable building,
      tech buildings first), knows whether a tech building is near (SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE)
- [x] Parachutes leave a cargo plane at the plane's speed (KeepContainerVelocityOnExit) and brake
      towards their target, so drops land where they are meant to
- [x] Tried on: Tournament Desert (both derricks, by the player and by a hard China / USA computer
      player, which also took the player's Command Center), Tournament City (refinery), El Scorcho
      (pad, platform); 20000 frames of a computer player's game without an error
- [ ] Not done yet: (crates: M4k) the original's attack priority sets (the capture hunt uses "tech buildings first"
      instead); cursor feedback over a building that can't be captured; the flash is in the detected
      colour, not the capturer's; the flag animation isn't stretched to the 20 s; a pad's vehicle
      doesn't move on to a rally point; the score screen doesn't show the buildings captured; the
      Capture button was driven through the interface's click path by test steps (button, then the
      building), but picking the building under the real mouse cursor was never tried

## Done (M4f: hackers and the units' own abilities)
- [x] Hackers at the internet (HackInternetAIUpdate): the button sits a hacker down where it stands;
      it unpacks (7.3 s, give or take half), then makes $5 every 2 s (6 / 8 / 10 with rank, and a
      point of experience each time), the sum floating up over it. Any order makes it pack up first
      (5.1 s) and only then carry the order out. In an Internet Center (InternetHackContain) whoever
      comes in starts hacking by itself, a little faster, and leaves without packing
- [x] SpecialAbilityUpdate as a whole (it did the flag capture only): persistent abilities that
      trigger again and again, the things an ability leaves in the world and how many of them it
      may have out, running clear afterwards, turning about after the animation, giving up stealth
      before the trigger. Who may use what on what is ActionManager's canDoSpecialPowerAtObject
- [x] Black Lotus: captures a building by laptop from a distance (6 s), knocks a vehicle out for 15 s,
      takes $1000 out of an enemy supply center (both sums float up)
- [x] Hacker: an enemy building is dead for as long as the hacker keeps at it (and 2 s more), binary
      raining over it. Hacked things (DISABLED_HACKED) do nothing, as after an EMP
- [x] Colonel Burton: timed charges (20 s) and remote ones on buildings and vehicles - one per target,
      10 / 8 at most - and the detonator, greyed out while there is nothing to set off; he runs clear
      after planting. Remote charges go with him when he dies, timed ones tick on. What a charge kills
      teaches him. The knife is a FIRE_WEAPON button: the weapon of that slot, at what is clicked
- [x] Tank Hunter: TNT on a vehicle or building (10 s). GLA Rebel: booby trap on an own or neutral
      building, which goes off under whoever captures it, walks into it or plants a charge on it (not
      under the trapper's friends), and when the building dies (StickyBombUpdate with its
      geometry-based damage, Object::checkAndDetonateBoobyTrap)
- [x] Missile Defender: paints a vehicle with a laser and fires the laser-guided missiles at twice
      the rate for as long as it stays within 250
- [x] The beams (W3DLaserDraw): the hackers' arc of numbers and the laser pointer, drawn by the
      original's rules (segments on an arc, glow lines of growing width, tiled scrolling texture)
- [x] Weapon sets: a slot marked "AutoChooseSources = NONE" (Burton's knife, the laser-guided
      missiles) is never picked by itself, only when locked in by a button or an ability
- [x] Contact weapons (WeaponTemplate::isContactWeapon: a knife, a terrorist's bomb) are carried right
      up to the target, which is no obstacle to their bearer
- [x] What a unit goes for by itself (PartitionFilterRejectBuildings): a player's idle units leave
      plain enemy buildings alone - only base defences and buildings somebody shoots out of are fired
      on unasked; the computer player's units take any. A unit busy with an ability doesn't drop it
      to shoot back
- [x] The computer player: its hackers hack the internet when idle (TEAM_USE_COMMANDBUTTON_ABILITY),
      and TEAM_HUNT_WITH_COMMAND_BUTTON works with any of these abilities, by the original's rules
      (enemies only, nothing hidden, no vehicle that is already dead in the water)
- [x] Tried on Tournament Desert: each ability by test steps, Burton's buttons through the command
      bar's own click path, 26000 and 24000 frames of a hard China / USA computer player's game
      without an error (China's hackers earning from about minute 10)
- [ ] Not done yet: Jarmen Kell's shot at a driver, hijackers, saboteurs, car bombs (the
      CrateCollide abilities and FIRE_WEAPON hunts; pilots are M5c); the Helix's napalm bomb and the bomb truck's
      disguise; mines can't be cleared, so neither can a planted charge; the line of sight an
      approach may ask for; the laser's flare at both ends; nothing shows on a vehicle that is hacked
      besides the falling numbers; picking a target under the real mouse cursor was never tried for
      any of these, and how any of it sounds was only checked in the log (the hackers' typing loop
      starts and ends with the hack)

## Done (P1: big games - eight players on the largest map)
Measured on Twilight Flame against seven hard computer players (`-GenBots=7 -GenPerf`), at normal
speed, looking at the thick of it. Before: 40-45 frames a second by minute 13 and falling, 13-26
frames over 33 ms every ten seconds, a logic step of 11.6 ms (up to 61). After: about 100 at minute
13 and 120-160 at minute 30, none over 33 ms, a logic step of 1.9 ms that no longer grows.
- [x] Objects by where they are (FGenPartition, the original's PartitionManager): a grid of cells
      rebuilt every logic step. "Who is near enough to shoot at", "the nearest enemy anywhere" (a
      hunt, outward ring by ring) and "who does this blast reach" look at the cells around a spot
      instead of at every object in the game. The target search was 43% of a logic step
- [x] The path search keeps its working memory (stamped, never cleared): on this map every search
      used to allocate and clear room for all of its 395,641 cells
- [x] Which ground connects with which (the original's PathfindZoneManager), relabelled when a
      building appears or goes: a goal that can't be reached is replaced by the nearest place that
      can, instead of the whole map being searched to find there is no way. A search around parked
      units, or from ground nobody labelled, gives up after 25,000 / 60,000 cells and makes do
- [x] The search leans towards the goal (weighted A*, `gen.PathWeight`, 13 = 1.3), and a path is
      straightened by looking at most 48 cells ahead, halving on a blocked view
- [x] A unit closing in on a target plans its way again only when the target has moved out of
      reach of where it is headed (it did every second: an army marching on a base asked for its
      way hundreds of times)
- [x] Models are posed and skinned only while some part of them is being drawn (or casts a shadow);
      out of view their animation runs on, and the pose is caught up when somebody looks or asks
      for a bone. That was 6 ms of every frame for 400 animated models
- [x] A crash found on the way: a garrisoned soldier whose window had gone with the building's old
      look, every other window taken, indexed the windows with "none" (PlaceAtGarrisonPoint)
- Why Twilight Flame: it is the largest of the 47 maps (4900 x 4900, 629 x 629 path cells) and has
  eight bases - the longest searches, the most cells to clear for each, and the dearest failures (a
  search that found no way around a parked crowd looked at the whole map; up to 49 ms). None of
  this came from the original's design: it has a partition, zones and a path queue. They were
  shortcuts of this port. (Why the original itself is slow there was not measured.)
- (What was still open here is in the next section.)

## Done (P1b: big games - what a frame costs)
The same game after a second round, at normal speed: 200-255 frames a second at minute 13-15 (about
100 after the first round, 40-45 before it) and 250-280 at minute 30; game thread 3.9-5.0 ms, render
thread 3.2-3.6 ms, a logic step 2.4-4.4 ms, no frame over 33 ms; memory levels off at about 3.4 GB
from minute 15. A crowd of 240 infantry on one screen: 216-249 frames a second (71-81 before).
- [x] Particles are one primitive per kind of blending (UGenQuadBatchComponent: masked, darkening,
      smoke, glow - drawn in that order, after the water and the models' glass, as the original
      drew its particles last): one run of vertices filled once a frame, a draw per texture out of
      it. They were a procedural mesh per texture: sending those was 1.5 ms of every frame on the
      game thread and, with the infantry's, 3.7 ms on the render thread - for two dozen small meshes
- [x] An infantryman's skin is its own kind of mesh (UGenSkinMeshComponent): triangles, texture
      coordinates and colours go to the renderer once, a pose is new positions and normals written
      over the old, and its bounds are roomy so they seldom change. The skinning itself works from
      the bones' axes, already in Unreal's frame, instead of a quaternion per vertex
- [x] Why the procedural meshes were so dear: the project has ray tracing switched on (Lumen's),
      and with it the engine's procedural mesh throws away and rebuilds its ray-tracing geometry on
      every update, besides taking every vertex apart and putting it together again on both
      threads. Particles and infantry are no longer in the ray-traced scene (they never mattered
      to it); their shadows are the shadow maps', as before
- [x] Animated models take turns (`gen.PoseBudget`, 80 a frame): every model is posed when its
      animation reaches a new frame of its own; the blending in between is shared out
- [x] Preloading (the original's KINDOF_PRELOAD): the models and animations of what the sides in
      the game can build are read and built a few milliseconds a frame from the start, their
      default looks first, the night's only on night maps. The first tank of its kind used to hold
      up the frame it appeared in (20-40 ms). Costs memory: about 120 MB for two sides, 500 MB for
      three
- [x] Moving things only tell the renderer where they are while they are in or near what the
      camera takes in (`gen.CullTransforms`); the rest are put in their place when the view comes
      to them. A model is some eight parts, each carried to the render thread when it moves
- [x] The computer players' scripts: the logic keeps a list of each player's objects
      (GetObjectsOf, kept up as objects come, go and change hands). A player's power, its units of
      a kind, its things in an area were each a walk through every object in the game, a dozen
      times a step per player. 1.3 ms a step for seven players became 0.8
- What a frame is now made of at minute 13 (4.3 ms of game thread): the logic step shared out over
  the frames it isn't in (0.5), the models' ticks and poses (0.6), the pass over the drawables
  (0.6), the HUD (0.45), effects (0.4), the engine's own (1.5-2). Nothing stands out any more
- (What was still open here is in the next section.)

## Done (P2: 240 frames a second - the simulation beside the frames)
The player's screen is 240 Hz: a frame has 4.17 ms, on the game thread and on the render thread
both, and not one may come late thirty times a second. The same game as above (Twilight Flame, seven
hard computer players, minute 13-15, normal speed): 350-370 frames a second, game thread 2.2-2.6 ms,
render thread 2.7-2.8 ms, 0.4-1.3% of frames over 4.2 ms and next to none over 8.3. Zoomed all the
way out over a whole base: 285-300. 240 infantry on one screen: 290-340. (Before: 200-255, with a
logic step of 2.5-4.5 ms inside one frame in six.) These with nothing else at work on the machine;
with other programs using the graphics card (two game clients, up to half of it, during the last
measurements) the same scene ran at 275-315 with 1-15% of frames over 4.2 ms.
- [x] The logic step runs on a thread of its own (GenSimThread.h, which says how). The screen
      blends between the last two steps that are *finished* and runs a few milliseconds behind the
      step clock (6-9 ms in the heavy game: about what a step takes from being asked for to being
      done); a step that is late holds the blend for the difference, not the frame
- [x] One gate between the two threads. The game thread is in the logic while its game code runs
      (from the start of the world's tick to the end of the logic's own, and while the HUD is
      drawn); the step has the rest of each frame and stops at safe points - between two objects,
      two scripts, two teams, every few hundred cells of a path search - when the game thread
      wants in. The game thread waits 0.01 ms a frame on average, about 1 ms at the worst. So a
      burst of path requests no longer costs a frame anything: the step takes longer, that is all
- [x] The step no longer touches anything of the engine's. Models are made and taken away by the
      drawing pass from notes the step leaves; a weapon's flash goes on the bone of the model as
      drawn a frame later; sounds asked for by the step are played by the game thread's next
      update; where a projectile leaves from is the launch bone of the model's base pose turned
      with the turret (W3DModelDraw::getProjectileLaunchOffset), not of the pose on the screen
- [x] What the game thread still does each frame, trimmed: the models' animation is one loop of the
      game's (not four hundred engine tick functions), run while the step has the floor, the posing
      itself on many threads; an object's look is checked every fourth frame, in turn, its place
      every frame, and not at all while it stands still; the radar's blips are a 128 x 128
      texture redrawn fifteen times a second (as the original's are a texture), not a rectangle
      per object per frame; particle systems the camera can't see are passed over; the engine's
      physics, which had nothing to simulate, is off
- [x] Models are reused: a shell that has landed, wreckage that has sunk away leave their model
      behind, hidden, for the next of the kind. A battle made and unmade hundreds a minute - each
      several engine objects created, registered and later collected as garbage. And the drawing
      pass builds models, and rebuilds them for a new look, 0.7 ms' worth a frame (`gen.DrawBudgetMs`):
      a new building is twenty or thirty parts, and three of those in one frame was a late frame
- [x] Two engine settings (Config/DefaultEngine.ini), both to be undone by changing one line.
      Shadows come from cascaded shadow maps instead of virtual ones (`r.Shadow.Virtual.Enable=0`):
      from the game's camera the picture is the same (Progress/86 and 87), and the render thread
      does 0.9 ms less a frame, the game thread 0.8. The engine collects its garbage every ten
      minutes instead of every minute (`gc.TimeBetweenPurgingPendingKillObjects=600`): a collection
      is one frame of about 20 ms however little garbage there is (it walks every engine object,
      about 100,000 here; measured in the editor's build). (Since P2b: once an hour, and when a
      pause doesn't show.)
- [x] The computer players, again: a base building list asked two questions of every object in the
      game for each building on it (is the spot safe, where is a dozer) - 3.5 ms in one go
      every two seconds per player. Now of the objects near the spot, and of the player's own
- (What was still open here is the next section.)

## Done (P2b: what P2 left open - path bursts, parts per model, memory, garbage)
The same heavy game after it, nothing else at work on the machine: 333-349 frames a second, game
thread 2.6-2.8 ms, render thread 2.8-2.9 ms, 1-3% of frames over 4.2 ms.
- [x] A budget of path work per step, as the original's path queue has
      (Pathfinder::processPathfindQueue): once a step has searched 5000 cells (`gen.PathCellsPerStep`)
      further requests wait in a queue for the next steps; a unit waiting for its path counts as on
      its way. And a search - which touches only the pathfinder's own grids - is done outside the
      gate, as is sorting the objects into the partition's cells: the game thread comes and goes
      meanwhile. A step's longest: 59 -> 12 ms; a late step holds the picture in 9-26 frames per ten
      seconds instead of 17-75. 240 infantry sent off at once: about 48 wait a step or two
- [x] Fewer parts per model. A W3D model is a dozen or two meshes, each on a bone of its own. Those
      that nothing moves or hides are now one mesh (FGenModelPlan in GenAssets.h): what moves is
      told beforehand by the thing's draw modules (turret, recoil and muzzle flash bones, the lists
      of things to show and hide, upgrades, the supply pile's boxes, and its animations - a channel
      counts if it ever has its bone anywhere but where it is; the exporter wrote one for every
      bone), and should a pose ever move one of the merged parts after all, or something hide it,
      it becomes a part of its own again for every model of the kind (ApplyPose, ShowSubObject).
      See-through meshes stay as they were (they are drawn back to front, each by where it is).
      The picture is the same (Progress/89 and 90: one base both ways; bases of all three sides
      compared pixel by pixel differ only where smoke drifts). In an eight-player game of 17
      minutes the net caught two kinds of model nobody had announced (2 ms each, once).
      In the heavy game: 4700 parts on screen -> 3400, draw calls 978 -> 900, 14,000 fewer engine
      objects, 90 MB less. **The frame takes as long as before** (measured the same day, both
      ways): the parts that are left do move - banners, fans, turrets and barrels, the Nuke
      Cannon's legs - and a part costs the render thread too little for a quarter fewer to show.
      So skinning the rest on the GPU would not buy frames either, and isn't planned for that
- [x] What preloading takes, measured (`gen.AssetMemory`): in the eight-player game 1262 model
      files - meshes 15 MB as read, 852 animations 34 MB, the built meshes 48 MB, 789 textures
      34 MB (they are DXT as in the original). About 130 MB, all of it used: nothing to trim
- [x] Garbage. Nine in ten of the engine objects a game threw away were model parts: a model that
      changes its looks - a building going up, standing, damaged - took its parts down and put new
      ones up. They are kept and used again now (GenModelComponent.cpp, GFreeParts): 22,200
      objects of garbage in thirteen game minutes -> 2,800. A collection still costs one frame of
      11-20 ms in the editor's build, whatever there is to collect (it starts from the engine's
      own 48,000 permanent objects; the engine's incremental collecting was tried and has that
      first slice of 8-9 ms too). So the game asks for one when a long frame doesn't show - the
      menu is opened, the score comes up, a map is loaded - and during play the engine is left to
      do it once an hour (`gc.TimeBetweenPurgingPendingKillObjects=3600`)
- [ ] Not done yet: one path per unit where the original plans one for a group

## Done (M4g: building sites, factory doors, selling - and the draw modules' states behind them)
What a building looks like while it goes up, makes a unit or is sold is not a matter of timing in
the original but of W3DModelDraw's condition states, of which this port had the simple half. A war
factory has eight draw modules: the building, its door, cranes, welding arms, the lights that flash
when a tank is done, three layers of scaffolding.
- [x] W3DModelDraw::setModelState as it is: every draw module of an object has its own state and the
      one waiting behind it. `TransitionState = <from> <to>` (1733 in the data) is shown on the way
      between two states with those keys - scaffolding going up and coming down, soldiers getting to
      their feet; `WaitForStateToFinishIfPossible` lets the current animation run out first. The
      flags: RANDOMSTART, START_FRAME_FIRST / LAST, MAINTAIN_FRAME_ACROSS_STATES (an animation
      going on from where the last state's had got to), RESTART_ANIM_WHEN_COMPLETE, and
      ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT (925 states): the model stands as deep in the ground
      as the building is unfinished. A new drawable starts with no conditions and is then told the
      object's, as the original's is - so a new site's scaffolding rises instead of being there
- [x] ParticleSysBone systems on every module's state (scaffolding has sparks and smoke), not only
      the first module's. Animations stand still on what an EMP or a hacker has shut down
      (Drawable::getShouldAnimate), and on a building without power where the module says so
- [x] Building sites: scaffolding up while the dozer drives over, then the building rises out of
      the ground behind it as the dozer works (and stays flagged as being built when the dozer
      leaves, as the original has it); finished, the scaffolding comes down
- [x] The ground under a new building is levelled (TerrainLogic::flattenTerrain): to its mean
      height or the height at the middle, whichever is lower - lowered, never raised - a cell
      beyond the footprint all round; the terrain's mesh there is rebuilt, a chunk a frame
- [x] ProductionUpdate's doors: a finished unit waits for the door to open (DoorOpeningTime: a war
      factory's 3.25 s), the door stands open (DoorWaitOpenTime) and closes; CONSTRUCTION_COMPLETE
      for its duration and ACTIVELY_CONSTRUCTING while anything is in the queue (cranes at work).
      QueueProductionExitUpdate: one unit out of the door per ExitDelay (Red Guards one behind the
      other, not as one)
- [x] Selling as BuildAssistant does it: scaffolding up for 1.5 s (at twice the pace), the
      building sinks over 3 s, scaffolding down for 1.5 s, then the money and gone - six seconds,
      not three; unselectable from the start
- [x] Things of the POWERED kind - Patriot batteries, Gattling cannons, speaker towers, the
      superweapons - stand idle while their owner uses more power than it makes
      (DISABLED_UNDERPOWERED), with the sound of it
- [x] MultipleFactory (shorter build times with several factories of a kind) is read and applied;
      GameData.ini has it at 1.0, which is none
- [x] Tried on Tournament Desert: a war factory put up, a Crusader built (door opens at f1200,
      tank out at f1299, door closes at f1390), the factory sold - frame by frame in pictures
      (Progress/91-93); one put up on a slope (1250, 990: levelled to 38.1); two Patriots with
      and without a power plant; hard computer players of all three sides build as before; the
      heavy eight-player game: 364-370 frames a second, no errors; seventeen minutes of an
      eight-player game at double speed: no errors, 3-5 objects in a transition at any time.
      Levelling a site costs the frame it is drawn in up to 3.6 ms per terrain chunk (one chunk,
      rarely two or four)
- [ ] Not done yet: PRISTINE_BONE_POS_IN_FINAL_FRAME (weapon bones taken from a state's last
      frame); setAnimationCompletionTime for abilities (the capture flag's animation isn't
      stretched to its 20 s); the darker tint of a disabled object; roads that cross levelled
      ground keep their old height; an airfield's hangar doors are still its parking places'
      own (they stand open while a plane is at home); the "building disabled" sound is heard by
      its owner only, wherever the camera is

## Done (M5c: what keeps aircraft alive - flares, the Aurora's run, pilots - and the combat drop)
- [x] Countermeasures (CountermeasuresBehavior, with Upgrade_AmericaCountermeasures): whether a missile
      will be fooled is decided when it is fired (EvasionRate, while flares are on board or in the
      air); the first volley goes out after the reaction time, then one per DelayBetweenVolleys until
      they are spent, fanned out over VolleyArcAngle and thrown with the aircraft's own speed; a fooled
      missile turns for the nearest flare of the last volley MissileDecoyDelay after launch and does no
      damage where it goes off (flare or no flare, as in the original); nothing is launched while
      supersonic; flares are reloaded when the aircraft taxis to its parking space (or after
      ReloadTime where the data gives one)
- [x] The Aurora's attack run (JetAIUpdate::getSneakyTargetingOffset): while it has a target, and for
      AttackersMissPersistTime after, whoever fires at it aims SneakyOffsetWhenAttacking beside it - at
      a spot, not at the plane - and shots already in the air lose it. Tried: three Gattling tanks and
      three missile soldiers never touch it on the way in, and shoot it down two seconds after the bomb
- [x] Flight height over water (Locomotor::getSurfaceHtAtPt): the preferred height counts from the
      water, not from the lake bed under it (a Comanche over Tournament Lake: 143 above the bed, 100
      above the water)
- [x] Pilots: EjectPilotDie (a veteran or better vehicle or plane lets its pilot out as it dies, on the
      ground or by parachute; VeterancyLevels of die modules are honoured now) and the helicopters'
      OCLEjectPilot as the rotor comes off; the pilot has the rank of what he flew; ObjectCreationList
      CreateObject learnt PutInContainer (a parachute), InheritsVeterancy, InvulnerableTime and
      RequiresLivePlayer; a parachute given no landing spot opens after falling (or rising) its
      ParachuteOpenDist and comes down below where it opened, as in ParachuteContain
- [x] The pilot climbs into a vehicle (VeterancyCrateCollide with IsPilot, by way of the enter order
      and ActionManager::canEnterObject): one of ours, on the ground, trainable, not yet heroic, of the
      kinds the data asks for - it gains his ranks and he is gone. A computer player's pilot looks for
      one by himself every second (PilotFindVehicleUpdate: within ScanRange, at least MinHealth), and
      walks home once if there is none. "On the ground" is the locomotor actually in use, as the original
      checks it, so an aircraft parked at its airfield is one too: the plane has chosen its
      SET_TAXIING locomotor there and the pilot can climb into it (a helicopter on a pad keeps its air
      locomotor, so it is not; and an aircraft in the air never is).
- [x] Combat drop (ChinookAIUpdate, AIRappelState, W3DRopeDraw): the button, then a click on the ground
      or on anything with room inside that is no side's own base building; the helicopter flies there
      (over a building at least MinDropHeight above its roof), hangs still, lets down a rope from each
      RopeStart bone - they fall under gravity and sway - and the soldiers who can (KINDOF_CAN_RAPPEL)
      go down them one after the other, PerRopeDelay apart, at RappelSpeed, in the attitude of the
      RopeEnd bone. On a building each kills up to two enemies inside; one who killed two dies himself,
      the others go in (or stand beside it when it is full). Then the ropes are let go and fall. Orders
      given during the drop wait until it is over; its supplies are lost, as when it lands
- [x] Tried on Tournament Desert: two missiles leave a Comanche for a flare; an elite Comanche's pilot
      comes down by parachute and makes a Crusader elite; a computer player's pilot does the same by
      himself; six Rangers slide down onto open ground, into an empty building, and into one held by
      two Red Guards (the first kills both and dies, three take the building); the drop by the
      interface's own click path. 22 minutes against a hard USA computer player without an error.
      Speed was measured with another program busy on a core (the eight-player game 315-322 frames a
      second, 2.7-4.5% of frames over 4.2 ms; a scene none of this touches was as much slower than
      the day before: 300-328 against 334-374)
- [x] Tried the same for a plane parked at its airfield: on Tournament Desert an airfield with a veteran
      pilot standing beside it and a Raptor produced there - the plane keeps its parking space with the taxi
      locomotor, the pilot is told to enter and walks over, and the Raptor becomes Veteran; a Crusader takes
      the same pilot as before. The computer player's pilot uses the same check when he scans for a vehicle.
- [x] A pilot sent from far away can walk over the airfield to the plane (the original's
      m_ignoreObstacleID, which was not ported): a footprint cell remembers which object put it there
      (PathfindCellInfo's obstacle id), and a search or a destination test may cross the cells of the object
      it is going into - or of the airfield that object is parked on - as
      Pathfinder::validMovementPosition does ("if (toCell->isObstaclePresent( m_ignoreObstacleID ))
      return true") and checkDestination ("if (cell->isObstaclePresent( ignoreId )) continue").
      AIEnterState::onEnter holds it for the container the unit is walking to and onExit gives it up;
      findPath's airfield case takes the airfield itself when the goal is something parked there, and only
      then - the original's note is that this is for things parked on the airfield, not for anything
      hovering over an obstacle (a Chinook over a supply dock). The cached region labels are not changed by
      it, so a search still knows what it cannot reach: the exception lasts for that search only, and a goal
      on the footprint it is allowed to cross counts as reachable. Tried on Tournament Desert with an
      airfield at (1000,900) and a veteran pilot at (1000,1400): his path ends on the airfield's own cells
      and he walks over the compound to the Raptor produced there, which becomes Veteran at frame 1962
      (before this he stopped outside and never got in). The same pilot ordered into a Crusader at
      (1200,600) still goes round the airfield (through (1070,1234) and (1163,1013)) and ranks it up, so
      nothing else may walk over a building's cells for him.
- [ ] Not done yet: CombatDropKillFX (units have no per-unit effects yet); a soldier shot on the rope
      dies where he hangs; a helicopter shot down during a drop leaves the soldiers on the ropes
      sliding on (the original drops them); nothing shows a jet being locked on to (LockonCursor);
      the pilot's hop out of a wreck on the ground (he appears beside it); computer players never
      order a combat drop (no skirmish script does)

## Done (M3g: the effects that were missing - hits, fire, falling trees and buildings, lasers)
- [x] DamageFX (DamageFX.ini, ActiveBody::doDamageFX): what a hit looks and sounds like, by the armor
      set's DamageFX, the kind of damage and the attacker's rank; the major effect from AmountForMajorFX
      up, otherwise the minor one; at most one per kind of damage and ThrottleTime on one victim
- [x] Fire and smoke on damaged models (ActiveBody::updateBodyParticleSystems): the GameData.ini systems
      on the bones named for them (FireS / FireM / FireL / SmokeS / SmokeM / SmokeL nn), on at most so
      many of each, picked at random, made afresh when the damage state changes; a burning object gets
      bigger fire, fire in place of smoke, twice as many, and the "Aflame" bones' own
- [x] Catching fire (FlammableUpdate): FLAME and PARTICLE_BEAM damage counts down FlameDamageLimit (and
      counts afresh after FlameDamageExpiration without any); then the object is aflame for
      AflameDuration - the AFLAME condition, the burning sound, AflameDamageAmount every
      AflameDamageDelay with the BURNED death type - and after BurnedDelay it smoulders and never burns
      again. FireSpreadUpdate: embers, and the nearest thing that can burn within SpreadTryRange is lit.
      HighlanderBody (the burnable trees): takes damage, dies only of the unresistable kind
- [x] Toppling (ToppleUpdate, and W3DTreeBuffer's toppling of W3DTreeDraw's trees - here they are
      objects, so one piece of code): what a vehicle with CrusherLevel above 1 drives into goes over,
      away from it; it accelerates down, bounces by BounceVelocityPercent until it lies still, with the
      effects of going over and of bouncing; a lamp post or sign turns to fall flat, leaves its stump
      and dies the TOPPLED death (lying there until its slow death sinks it); a tree sinks SinkDistance
      over SinkTime and is gone
- [x] Laser weapons (Weapon::createLaser, LaserUpdate, W3DLaserDraw): a weapon with a LaserName strikes
      at once and shows a beam from its LaserBoneName bone as drawn to where it struck (ten higher on
      something standing on the ground), for the laser object's lifetime, with the flares at both
      ends; a scattered shot draws to the ground where it went. Laser Crusader and Paladin, the laser
      Patriot, the Avenger, the point defence lasers' beams, the ECM and microwave streams
- [x] Civilian buildings coming down: StructureCollapseUpdate (a shudder for the collapse delay, then
      down under gravity less CollapseDamping, with a burst of the BURST or DELAY effects and debris
      every burst delay, the FINAL ones when it is under) and StructureToppleUpdate (after the topple
      delay it falls over away from what killed it - slowly while its StructuralIntegrity holds - the
      AngleFX on the way, the crushing weapon fired in lines across the ground it comes down on).
      Either way the POST_COLLAPSE ruin stays. ObjectCreationLists can be run at a position
- [x] Ruins stay (the default DestroyDie belongs only to objects with no die module of their own): a
      dead structure that brings an effect or debris list for its death but nothing that removes it is
      "never truly destroyed" and keeps its RUBBLE look for good, as the game's data says it should
- [x] Thrown wrecks and bodies: ObjectCreationList ApplyRandomForce (the dying object itself is thrown
      and set spinning - a Technical, a rocket buggy) and CreateDebris with an AnimationSet and FXFinal
      (W3DDebrisDraw: one animation at first, one in flight, one on landing with the landing effect -
      the Technical's gunner)
- [x] Tried on Tournament Desert: rifle fire sparks on an Overlord; a Dragon tank sets a Crusader
      alight; a tank through a block of 24 trees, over a lamp post (down at frame 140, lying the way
      it was pushed); two Laser Crusaders' beams on an Overlord (they need power, and stood idle
      without it); a high-rise down in 74 frames and a tall tower over in 126, both ruins left; two
      Technicals thrown 14 and 20 high with their gunners. Two 20-minute games against hard computer
      players without an error. `gen.DamageFX 0` / `gen.BodyParticles 0` switch the two off for the
      performance review (in the eight-player scene neither showed in the frame time; the toppling
      test costs 0.01 ms a step)
- [ ] Not done yet: DamageFXOverride of a weapon; FXListAtBonePos (only the garrison-clearing weapons'
      effects use it), FireWeaponCollide (a burning tree hurting what touches it), BoneFXUpdate /
      BoneFXDamage (24 civilian buildings' own fire and debris by damage state), the BURNED model
      condition, trees pushed aside without falling (MoveOutwardTime of bushes and grass), toppling
      by blast (the nuke, the wave of a broken dam); a laser's
      PunchThroughScalar and widening / decaying beams; OCL FireWeapon and Attack (the superweapons'
      missiles: with the superweapons); heat-haze smudges (two particle systems use SMUDGE.tga)

## Done (M3h: winding up and aiming; fences and crushed cars; moving as a group)
- [x] PreAttackDelay (Weapon::getPreAttackDelay, PER_SHOT / PER_ATTACK / PER_CLIP): the wait before
      the shot, with the PREATTACK condition of the weapon's slot lasting exactly as long (the Scud
      launcher raising its missile, the Tomahawk); an attack given up starts the wait afresh
- [x] MinimumAttackRange: a target nearer than that is backed away from - to the smaller of nine
      tenths of the range and a cell and a half beyond the minimum - and then fired at
- [x] MinTargetPitch / MaxTargetPitch: a weapon can't be used on what lies above or below those angles
- [x] Turret pitch (TurretAI AllowsPitch, PitchRate, NaturalPitch, FirePitch, MinPitch,
      GroundUnitPitch): the barrel bone is raised towards the target (or to the fixed firing pitch) at
      the pitch rate before the shot and comes back down with the turret; a sub-object that is hidden
      takes with it whatever hangs on bones below its own (W3DModelDraw::doHideShowBoneSubObjs: the
      launcher's spare missile)
- [x] Fences (Pathfinder::classifyFence): an object with a FenceWidth closes the cells along its line
      to everything but what can crush (a surface of its own in the path search, so the two kinds
      of unit have their own maps of what can be reached); a crusher drives through and the piece
      topples, lies flat, sinks and is gone. Fences and shrubs don't stand in a building's way
      (BuildAssistant::isRemovableForConstruction): whatever of them reaches into the footprint is
      cleared when the site is laid down
- [x] Crushed vehicles (PhysicsBehavior::checkForOverlapCollision, CrushDie): a car has three crush
      points - middle, front and back; the crusher takes the one its course passes closest to (the
      nearer on a tie) and flattens that end once it has driven past it: the FRONTCRUSHED /
      BACKCRUSHED models, both for the whole car, with the CrushDie sounds by their chances
- [x] The pathfinder's margins (Pathfinder::classifyMap, internal_classifyObjectFootprint): open
      ground beside a cliff counts as cliff, the cell after that is "pinched"; beside a structure,
      open cells with fewer than two open sides or four open neighbours are shut (a gap a cell wide
      between two buildings, or a building and a cliff, is no way through for anybody on the ground)
      and the ones touching it are pinched; a pinched cell costs 24 more to step on. Building
      placement goes by the land itself, not by these margins. `gen.PathCliffMargin 0` (before a map
      loads) switches all of it off
- [x] Group moves (AIGroup::groupMoveToPosition): no more grid around the goal. Each unit goes to
      the place it holds in the group, moved over to the goal - measured from the unit nearest the
      goal, no further out than six times its size, the nearest getting their paths first. Three
      infantry or three vehicles and more, going at least MinDistanceForGroup, travel as columns: one
      way six cells wide is found over open ground (Pathfinder::findGroundPath: narrower places cost
      extra, pinched ground is not used, parked vehicles are in the way), infantry get three lanes
      beside it and three or five files at the end (by GroupMovementPriority: those that move at the
      front in front), vehicles two lanes and three files, rank behind rank. A click inside the
      group's own area (GroupMoveClickToGatherAreaFactor) gathers it in round the spot; helicopters
      get places on rings of their own
- [x] Formations (Ctrl+F, AIGroup::groupCreateFormation): the selected units keep their exact places
      relative to one another and move at the pace of the slowest (not counting badly damaged
      ones), along the group's way if there is one; Ctrl+F again undoes it; an "F" beside the health
      bar marks them; a formation's members crowd each other less (0.55 of the blocked speed)
- [x] Routes without stops (AIFollowPathState): a unit on a route of several points doesn't brake for
      the ones on the way (the rest of the route counts as distance still to go), has reached one
      once it is abeam of it, skips points within a cell of where it is, and reserves ground only
      for the last
- [x] Tried on Tournament Desert: a Scud launcher raising and firing; a Tomahawk backing off; a
      Crusader through a chain-link fence (two pieces down at frames 64 and 72) while a Ranger walks
      round its end; a tank over three cars - head-on, sideways, from behind; a power plant laid
      over the fence (four pieces cleared); twelve Rangers ending three abreast in four ranks; six
      tanks, five Rangers and a dozer 1700 away by a way with nine corners, all there in ranks; a
      formation of tanks and Rangers at the Rangers' pace; eight units gathered in by a click in
      their midst; a tank and a Ranger sent at a one-cell gap between two power plants going round.
      Twilight Flame's bridge is still crossed. 20-minute games against hard computer players on
      Tournament Desert and Alpine Assault without an error
- [ ] Not done yet: the computer players' teams still get their orders unit by unit (AIGroup for
      teams: with the scripts' team actions); attack-move and guard still spread over a grid;
      waypoint mode (Alt-click adding points to a route); groupScatter; the angry mob's nexus;
      turn costs and the straight-line shortcut of the original's searches; a path ending on
      pinched ground stepping back a cell; hovercraft's sloppy arrival; walls (DEFENSIVE_WALL - none
      in the skirmish data)

## Done (M4h: missiles that fly themselves; the SCUD storm, the nuclear missile, the particle cannon)
- [x] Missile flight (MissileAIUpdate on a THRUST locomotor, Locomotor::moveTowardsPositionThrust):
      a missile leaves along its launch bone with InitialVelocity and coasts until IgnitionDelay is
      over; then it is armed, shows its exhaust and pushes itself - the thrust goes where it has to,
      within MaxThrustAngle of the nose, gravity counted in, the nose following the velocity at the
      locomotor's turn rate, but not before DistanceToTravelBeforeTurning has been flown. With a
      PreferredHeight it climbs to it and keeps it until DistanceToTargetBeforeDiving; within
      DistanceToTargetForLock (half of it for a spot) it is moved straight at the target; out of
      fuel it falls or goes off (DetonateOnNoFuel); its target gone, it is taken away. It goes off
      on its target's shape or on the ground near where it was going. HeightDieUpdate works for
      missiles. `gen.MissileFlight 0` gives the old straight flight; `gen.MissileTrace 1` logs a
      missile's state, velocity and thrust every half second
- [x] Launch bones as the original takes them: in the pose of the state's first animation's first
      frame (its last with PRISTINE_BONE_POS_IN_FINAL_FRAME) - a launcher's raised arms - with the
      turret's pitch on top of its turn (the Tomahawk and the SCUD launcher fire upwards), the
      numbered bones in turn (ShotsPerBarrel), and the launch bones' sub-objects hidden from the
      front as the clip empties (ProjectileBoneFeedbackEnabledSlots)
- [x] FireWeaponWhenDeadBehavior by its upgrades (StartsActive, TriggeredBy, ConflictsWith): the
      anthrax beta warhead in place of the plain one
- [x] ObjectCreationList FireWeapon (a weapon fired from the building at the spot) and Attack (the
      building attacks the spot with its own weapon, so many shots, the weapon locked): attacking a
      position, ScatterTarget tables (every shot of a clip to another entry, times
      ScatterTargetScalar), the ATTACKING model condition for as long as an attack is under way
- [x] SCUD storm: the arms rise for the PreAttackDelay, nine missiles leave one after another,
      climb, come over and dive on their nine spots, die 15 above the ground and fire their warhead
      there (with the anthrax field); the arms go under and come back loaded
- [x] Nuclear missile (NeutronMissileUpdate, NeutronMissileSlowDeathBehavior,
      MissileLauncherBuildingUpdate): the silo's doors open as the power comes ready, stand open,
      and close after the launch; the missile lifts off slowly, climbs, makes for a spot high over
      the target and comes straight down; 100 above the ground it dies and sends out its blasts,
      each after its delay - what is within reach is toppled, hurt (in full inside the inner
      radius), pushed away and later scorched
- [x] Particle cannon (ParticleUplinkCannonUpdate): by the power's clock it charges (its outer
      nodes glow), raises its antenna, links the nodes to it with lasers and stands ready; fired, a
      beam goes up from the dish and, BeamTravelTime later, comes down on the target - widening for
      WidthGrowTime, staying TotalFiringTime, narrowing away - with TotalDamagePulses pulses of
      PARTICLE_BEAM damage to whatever is within the beam's half width times DamageRadiusScalar,
      TotalScorchMarks scorch marks with the ground-hit effect, a burning remnant at every pulse and
      the ground it passes over seen. The power's button, used again while it burns, sends the beam
      somewhere else at ManualDrivingSpeed (the fast one after a double click). Without power it
      stops at once. Beams can widen and narrow (LaserUpdate's width scalar)
- [x] A disabled building's powers (Object::pauseAllSpecialPowers): while it is without power,
      hacked or under EMP the clocks of its own powers stand still and they can't be used
- [x] Tried on Tournament Desert: missile defenders, a Tomahawk (up at 70 degrees, 75 frames to a
      target 280 away), a SCUD launcher (straight up, 110 frames), Patriots against a Comanche and
      a Comanche's rockets all hit; the storm's nine missiles came down within a few units of their
      spots, killed a tank and left poison; the nuclear missile went off over its target and its
      sixth blast hurt 39 things; the cannon's beam killed two tanks and, sent 135 further east,
      went there at 20 a second. On their real clocks: the cannon charging at f6849, antenna from
      f6999, ready at f7200, firing 300 frames, packed up 135 later; the silo's doors opening 240
      frames before the missile is ready and shut again 300 after the launch; with too little
      power the cannon's beam was cut off in its first frame. `-GenPowersReady` lets a test fire a
      power without waiting out its timer
- [ ] Not done yet: delivery decals on the ground; the superweapons' countdowns shown to
      everybody (PublicTimer) and the view of the target they give (ViewObjectDuration); missiles
      colliding with what is in their way (ProjectileCollidesWith), being jammed (ECM) and killing
      a garrison (GarrisonHitKill: M4l); the nuclear missile's shaking on the pad; the
      cannon's sounds (charging, firing, the beam on the ground) and its "swath of death" for
      scripts. (The computer players' scripts fire superweapons through the same call as the
      player's button - SkirmishFireSpecialPowerAtMostCost - but no test game has been watched
      long enough to see one do it)

## Done (M4i: the generals' powers that were still greyed out - frenzy, the leaflet drop, mines, the Spectre gunship, the sneak attack)
- [x] Frenzy (WeaponBonusUpdate, TempWeaponBonusHelper): the power's marker gives every unit of
      ours and our allies' within BonusRange that can attack (and whoever rides in them) the
      FRENZY_ONE / TWO / THREE weapon bonus for BonusDuration; a bonus given for a while runs out
      by itself. Tried: a Battlemaster's 60 a shot is 66 for the ten seconds, then 60 again
- [x] Leaflet drop (LeafletDropBehavior): the bomber's container sheds its leaflets as it falls;
      from Delay after it appeared until it is gone, and once more as it dies, enemy soldiers and
      vehicles within AffectRadius are disabled for DisabledDuration. Tried: three units disabled
      for 600 frames
- [x] Mines (MinefieldBehavior): one mine object stands for NumVirtualMines of them. It goes off -
      its DetonationWeapon, under the trespasser, kept within the mine's own footprint - for whoever
      of the DetonatedBy sides drives or walks onto it, and again for every RepeatDetonateMoveThresh
      he goes on; its health says how many are left, and shot at it goes off for each mine's worth
      of health it loses. One that regenerates lies there as rubble after its last and has its
      mines back with its health (the inherited AutoHealBehavior); one that doesn't is gone. When
      what laid it dies it stops regenerating and withers (StopsRegenAfterCreatorDies,
      DegenPercentPerSecondAfterCreatorDies). Workers set none off unless WorkersDetonate says so
- [x] Laying them (GenerateMinefieldBehavior): the cluster mines power (the bomb lays MineName where
      it dies, scattered so many to the square foot, each sliding or falling to its place before
      it is live) and the buildings' mines upgrade (a ring round the building, or rings a mine
      apart with SmartBorder; DistanceAroundObject and MinesPerSquareFoot fall back on GameData's
      StandardMinefieldDistance / Density; none in water, on a cliff, or further under a
      structure than SkipIfThisMuchUnderStructure). The upgrade named in UpgradedTriggeredBy takes
      them up and lays UpgradedMineName in their place
- [x] What the rest of the game does about mines: one may build on them (the enemy's go off under
      the new site, anybody else's are taken away - when the site is laid and again when the
      building is done); a building sold takes its mines with it; a building captured hands them
      over; nothing fogged remembers a mine
- [x] The look: a mine field's model is nothing but eight bones with a blinking light on each
      (ParticleSysBone) - so a model without meshes now gets a drawable all the same, and the bone
      systems of a hidden drawable are stopped (W3DModelDraw::doStartOrStopParticleSys): the owner
      sees the lights, the enemy sees nothing until a detector finds them; no stealth pulse or
      detected glow on mines (the original's own exception)
- [x] Tried on Tournament Desert: cluster mines dropped between two Rangers and a pub - 8 mines
      laid, both Rangers dead on the way, the counts going down in the log; the Command Center's
      upgrade - 13 mines in a ring after its 20 seconds, lights seen by the owner and not by the
      enemy; a power plant put on an enemy mine (all eight go off at once, the site unharmed:
      buildings shrug off LAND_MINE); a Command Center sold (its mines gone)
- [x] The Spectre gunship (SpectreGunshipDeploymentUpdate on the command center,
      SpectreGunshipUpdate on the aircraft): made beyond the map's edge farthest from the target
      and an orbit's radius further out, it flies in on its fast locomotor with its doors shut,
      changes to the slow one inside its orbit radius, opens its doors and circles the target
      anticlockwise for OrbitTime - always making for the "satellite position" a little ahead of
      itself on the circle - then flies straight on until it is off the map. While it circles,
      every HowitzerFiringRate it looks for an enemy inside the targeting reticle (not one almost
      under it: "a fair distance from the ship"); its gattling gun fires at that, or strafes the
      ground towards the reticle a StrafingIncrement a frame with its dust (GattlingStrafeFX); once
      the gattling has been on the spot for HowitzerFollowLag the howitzer drops a shell within
      RandomOffsetForHowitzer of it each time. Its owner has it selected as it is sent for and
      moves the reticle with a click (kept inside the attack area; the ParticleUplinkCannon
      cursor); a computer player's looks for targets in the whole area by itself. The area and
      the reticle are drawn for the owner (plain circles for the decals' pictures)
- [x] What the gunship needed, and other things get: a rider that rides in the open - a
      PORTABLE_STRUCTURE in a HelixContain - is not hidden, may fire from on board, and is drawn on
      a bone of what carries it (W3DDependencyModelDraw's AttachToBoneInContainer; the Overlord's
      and Helix's add-ons want the same); AutoAcquireEnemiesWhenIdle = No is honoured (92 objects
      say so: left alone they pick no targets; what takes no orders either - ForbidPlayerCommands -
      neither); ShouldDrawPips = No; a particle system by itself at a spot from the logic
- [x] FiringTracker's continuous fire (found missing while the gattling fired): a weapon with
      ContinuousFireOne / Two speeds up to its CONTINUOUS_FIRE_MEAN and then _FAST bonus as it
      keeps firing - at another target too, within ContinuousFireCoast - with the barrels' model
      states and VoiceRapidFire, and winds down (CONTINUOUS_FIRE_SLOW) a coast after its last
      shot. A Gattling tank's shots come 12, then 6, then 4 frames apart
- [x] The sneak attack (a SPECIAL_POWER_CONSTRUCT button: the power is placed like a building, its
      ReferenceObject following the cursor and going only where that could be built - a computer
      player moves its spot to the nearest such place): the tunnel's start digs itself up for five
      seconds, thumping the ground three times (FireWeaponUpdate with InitialDelay; a clip that
      doesn't reload fires once), then dies into the tunnel itself, which has the health its start
      had left (CreateObjectDie's TransferPreviousHealth; whoever was shooting at the start shoots
      on at the tunnel) and is an exit of the owner's tunnel network like any other
- [x] Tried on Tournament Desert: the gunship against three tanks, a power plant and two soldiers
      (in from (1005, -250) at 290 a second, in orbit at f219 some 185 out, the gattling from f225,
      the howitzer from f243, the reticle moved by a `steer` step, gone off the map at f863 with
      everything in the area dead); a Gattling tank's rate of fire; the sneak attack at a spot
      that is level (refused on the slope in the middle of the map), its start damaged to 300 and
      the tunnel coming up with 300, three Rebels in at the base and out at the new tunnel 600
      away. A hard three-computer game ran 178000 frames (99 minutes of game) without an error; in
      another the American computer player sent for the gunship twice (73 shells, 735 gattling hits)
- [ ] Not done yet: the red tint of frenzied units (and the look of disabled ones); (clearing
      mines: M4m) demo traps; the radar events and voices of
      mines found; rectangular borders (AlwaysCircular = No); the gunship's afterburner sound and
      its decals' pictures; shock waves (a weapon's ShockWaveAmount throwing things about: the tunnel's
      thumps, others); the tunnel being placed at an angle of the player's choosing

## Done (M6c: selecting - what a click lands on, the game's cursors, what the mouse is on, groups)
(The user's priority of 2026-10-13: "the cursor, the animation when hovering over unit or building", and no
more selecting something by clicking near it.)
- [x] Picking as W3DView::pickDrawable does it: the ray under the mouse against the models themselves
      (RTS3DScene::castRay -> HLodClass / MeshClass / OBBoxRenderObjClass::Cast_Ray) - the triangles of
      every mesh that shows, as it is posed (skins too), and the model's own boxes (the PICKBOX the
      artists gave soldiers and other small things). Hidden parts are not in the way, nor translucent
      ones (glass, lights, muzzle flashes); the nearest hit wins. Only what the pick is for can be hit
      (PICK_TYPE_SELECTABLE: no shrubbery, mines, FORCEATTACKABLE or CLICK_THROUGH things, no hulks or
      rubble). Before, a click anywhere inside a thing's bounding sphere took it. If the ray meets
      nothing, eight more are tried three pixels round it (`gen.PickSlack`: a soldier seen from 900 up
      is a few pixels); `gen.PickBoxes 0` leaves the boxes out
- [x] What the mouse is on (InGameUI's moused-over drawable): looked for whenever the mouse or the
      view moves, and thirty times a second otherwise. It shows its health bar ("selected drawables
      and drawables that have been moused over") and, after Mouse.ini's 800 ms, its name on its
      owner's colour (a supply pile: what it still holds)
- [x] The look of a selection: no ring on the ground (the original has none); the health bar where
      and as wide as the original has it (Object::getHealthBoxPosition / getHealthBoxDimensions,
      computeHealthRegion), in its colours (green to red with a darker outline; blue to cyan for a
      building site or something disabled), the number of the unit's group at its left end; and what
      is selected flashes white (Drawable::flashAsSelected: at once, gone in four frames) - a tint
      added to the light the model is drawn with (ObjectTint, custom primitive data 2..4 of the W3D
      materials; UGenModelComponent::SetTint - there for the frenzy and disabled looks too)
- [x] The game's cursors: Mouse.ini's kinds and the 52 animated cursors of Data\Cursors (32 x 32,
      16 colours), read (FGenCursors) and made into the platform's own cursors frame by frame - the
      system draws them, so they don't lag behind the mouse at any frame rate - at one, two or three
      screen pixels a pixel by the height of the screen (`gen.CursorScale`), animated by the files' own
      steps and paces. Which one shows is InGameUI::createCommandHint's business: the arrow over the
      interface and in the menus; with nothing selected, Select over something of ours and the arrow
      otherwise; with units selected, Move, AttackObj over an enemy, Select over something of ours,
      EnterFriendly / GetHealed / EnterAggressive, DoRepair, GetRepaired, ResumeConstruction, Dock,
      SetRallyPoint, and GenericInvalid where they can do nothing; no cursor while a building is being
      placed (Mouse.ini has no picture for "Build": the building is under the mouse); a button's own
      CursorName / InvalidCursorName while its order or power is aimed (the cross, "Target", if it
      names none); the eight Scroll cursors by the way the mouse pushes the view. One function works
      out what a click would do (EvaluateClick): the click does it, the cursor shows it beforehand
- [x] The selection keys (CommandMap.ini): a double click on a unit of ours takes all of its kind on
      the screen (with Alt: on the map); E everything like the selection, Q every unit but dozers and
      harvesters, W every aircraft - on the screen, or if that adds none, on the map; Ctrl + a number
      makes the selection that group (a unit is in one group only), the number selects it (again
      within 20 frames: the view goes there), Shift + number adds it, Alt + number looks at it
- [x] Tried: pick scans across a tank, a soldier, a dozer and the command center (`#` only where the
      model is; the old pick took the middle nine of the seventeen steps whatever was there); each
      cursor case by `pick,...,hover` steps (the log names the cursor, the picture has it drawn in);
      the flash held for a picture (`gen.SelectFlashHold`); the platform's cursors made in a real
      window (`gen_test.ps1 -Window`); groups and keys by `selkey` steps; a pick costs about 55
      microseconds among 900 objects; a hard three-computer game of 36000 frames without an error
- [ ] Not done yet: none of it was tried with a real mouse (test runs have none) - how the cursors sit
      and feel is the user's to say; force attack (Ctrl: the ground, one's own things, FORCEATTACKABLE
      ones) and waypoint mode (Alt) with their cursors; the cursors of abilities not in yet (hijack,
      sabotage, defector, "OutRange"); the radius decals on the ground while a power is aimed (a
      plain circle stands in); a drag takes units by where they stand, as the original's does; the
      name of a disguised unit; the selection voice of a group is one unit's

## Done (M4j: what an upgrade adds to a vehicle - the Overlord's and Helix's add-ons, the Avenger's turret, propaganda, drones)
- [x] ObjectCreationUpgrade (133 modules in the data): the upgrade runs an ObjectCreationList from the
      object; a nugget's ContainInsideSourceObject puts what it makes aboard what made it ("stillborn"
      when it doesn't fit)
- [x] OverlordContain and HelixContain: one add-on - a PORTABLE_STRUCTURE - rides in the open on a
      bone of its carrier (W3DDependencyModelDraw: FIREPOINT01 in the Overlord's turret, CHASSISBONE
      under the Helix). It takes nobody's seat, faces the way the carrier does (or the carrier's
      turret: PassengersInTurret), looks as battered as the carrier, changes hands with it and dies
      with it; what it kills counts for the carrier (ExperienceSinkForRider). An Overlord with a
      bunker on it is a transport of the bunker's kind (the "redirect"): whoever boards goes into the
      bunker, the exit buttons and the count show the bunker's five places, they shoot out, get out
      around the Overlord, and lose half their health when the Overlord dies. A Helix carries its
      add-on besides its five places; its soldiers shoot out once the bunker is on
      (PassengersFireUpgrade) and have the GARRISONED bonus on board. PayloadTemplateName: the
      Avenger comes with its laser turret aboard the same way - it had no anti-air lasers before
- [x] A rider that has a turret turns the turret (it used to be "aimed" at once, like a soldier at a
      window); a player's attack order to a carrier whose riders may shoot goes to them as well
      (AIGroup::groupAttackObject)
- [x] PropagandaTowerBehavior (the speaker tower, the Overlord's and the Helix's tower, the Emperor):
      every DelayBetweenUpdates it sends out its pulse and looks who of ours and our allies' is
      within Radius (no buildings, nobody inside something). They fire faster (ENTHUSIASTIC, and
      SUBLIMINAL with the upgrade: 125% each in GameData.ini) and mend by HealPercentEachSecond -
      from one tower at a time - until they leave or the tower dies, is sold, is without power, or
      rides in something that is itself being carried
- [x] STATUS damage, found on the way: the Avenger's target designator did 200 points of real damage a
      shot. It marks its target (FAERIE_FIRE) for as many milliseconds as its "damage" says, and
      whoever shoots at a marked target shoots half again as fast (TARGET_FAERIE_FIRE)
- [x] Drones (SlavedUpdate; GenDrones.cpp): made beside the vehicle that bought them and tied to it.
      Four times a second a drone does the first of these that applies: mends its master when that
      is below RepairWhenBelowHealth% (the battle drone: over the master, 18 to 24 up, the arm
      unpacks, then turn by turn it waits, reaches out, welds - sparks, RepairRatePerSecond - draws
      back and zips RepairRange to another spot on its fast locomotor); flies to the master's target,
      as far as AttackRange from the master allows (a scout drone within
      DistToTargetToGrantRangeBonus of it gives the master DRONE_SPOTTING); flies ahead to where the
      master is going (ScoutRange); mends what little harm the master has; or keeps within
      GuardMaxRange of it, drifting from spot to spot. Nobody can select one (NO_SELECT). When the
      master dies the drone has no lift left and is dead when it comes down; when the drone dies the
      master's upgrade is taken back so that it can be bought again (UpgradeDie). The height rule of
      their locomotors (RELATIVE_TO_HIGHEST_LAYER - nothing but the drones uses it) was missing:
      without it they sat on the ground
- [x] Tried on Tournament Desert: three Overlords, one upgrade each (the tower on at f350, the bunker
      at f495, the gattling cannon at f640 - it turned on two Chinooks at once); three tank hunters
      into the bunker, firing out of it, out again, in again, and out with 50 of 100 health when the
      Overlords were killed; a Red Guard at 60 of 120 mended to 85 in twenty seconds beside the
      Overlord's tower, a Battle Master from 200 to 400 beside a speaker tower (which does nothing
      without a power plant); the same three add-ons on three Helixes, the tank hunters firing out of
      the one with the bunker; an Avenger's turret shooting a Helix down (60 hits) while its
      designator no longer hurts anything; three Humvees with a drone each - the battle drone
      mending its Humvee from 120 (22 points in the first five seconds), all three going with the
      Humvees into a fight, the hellfire drone's missiles on the Humvees' target, a scout drone
      killed and bought again, all of them falling when the Humvees were killed. The gunship's run
      again (its gattling is such a rider: first hit at f242 where it was f225 - the turret turns
      now). A hard three-computer game ran 41000 frames without an error: the Chinese computer put gattling cannons on two of its Helixes and its speaker towers were at work (America was out of that game early, so no computer's drones were seen)
- [ ] Not done yet: an add-on taking over the stealth of a stealthed carrier; the Overlord's
      "rider" draw modules (W3DOverlordTankDraw and the others are drawn as the plain ones - the
      add-on is simply placed on the bone every frame); the selection flash of an add-on with its
      carrier; MobMemberSlavedUpdate (the angry mob) and the slaves of a SpawnBehavior that are made
      anew; ExperienceScalarUpgrade; whether DRONE_SPOTTING lengthens any weapon's range was not
      measured

## Done (M4k: crates - what the dead leave behind, salvage)
- [x] Crate.ini's CrateData (the crate system: CreationChance, VeterancyLevel, KilledByType,
      KillerScience, a weighted pick among CrateObject lines, OwnedByMaker) and CreateCrateDie: what
      dies may leave a crate where it lay (at the edge of a dead building), nobody's unless the data
      says otherwise, never for killing an ally. In the game's data only the salvage crate is ever
      left - by a vehicle a GLA player's salvager kills (164 objects have the module; the lines that
      gave veteran tanks money and level-up crates are commented out)
- [x] CrateCollide's rules for who takes a crate (alive, not neutral, a unit - or a building when
      BuildingPickup - on the ground, not in a parachute, Required / ForbiddenKindOf, ForbidOwnerPlayer,
      HumanOnly, PickupScience), now found through the partition of objects instead of by looking at
      every object each frame; ExecuteFX
- [x] SalvageCrateCollide, only for SALVAGERs: better armour if it can have it (ARMOR_SALVAGER: the
      battle bus, twice - ARMORSET_CRATEUPGRADE_ONE / _TWO and the CRATE_UPGRADE armour sets), else a
      better weapon (WEAPON_SALVAGER: a technical's cannon and then its rockets, the quad cannon's,
      the Marauder's ... - the CRATEUPGRADE weapon sets and model states), else with LevelChance a
      rank, else $25 to $75. It is seen and taken by GLA players only (W3DScienceModelDraw's
      RequiredScience, PickupScience) and lies for 30 to 35 seconds. UnitCrateCollide (two free
      Crusaders) too; the money crates as before
- [x] Found on the way: a model state's HideSubObject / ShowSubObject lines change a part already in
      the list in place, as the original's parser does, instead of adding it again at the end - the
      order counts, because showing a part shows what hangs on its bone (an upgraded technical had
      the old gun's muzzle flash burning for good)
- [x] Drones and salvage crates are there to be clicked on but not selected: a click on our own drone
      or on a crate with units selected sends them there; an enemy's drone can be attacked (what is
      picked is the template's SELECTABLE, as in the original, not whether it may be selected now)
- [x] Tried on Tournament Desert: a quad cannon and two technicals killing six supply trucks (six
      crates where they died), driving over three of them (cannon, the battle bus's armour, the quad
      cannon's better guns); then crates placed by hand - a technical to its cannon and its rockets,
      the battle bus to its second armour, a Humvee driving over one and leaving it lying; two free
      Crusaders and $1000 from crates. A hard three-computer game ran 41000 frames without an error:
      31 crates left, 17 of them taken by the GLA computer's units (6 weapons, 3 ranks, money)
- [ ] Not done yet: the crates' decal on the ground (TERRAIN_DECAL_CRATE) and the "MoneyPickUp"
      picture rising from a money crate (a floating sum stands in); a salvager's VoiceSalvage when sent
      to a crate; VeterancyCrateCollide for crates that aren't pilots (it does nothing in the original
      either: it asks the crate's own AI where it is going), HealCrateCollide and ShroudCrateCollide
      (no object in the data uses them); a computer player's units don't go for crates - the
      original's notifyCrate forgets the crate before it looks it up

## Done (M4l: clearing a garrison, switching weapons, the shot at a driver, hijackers, car bombs)
- [x] GarrisonHitKill (DumbProjectileBehavior and MissileAIUpdate): a Ranger's flash-bang (10), a
      Dragon tank's flame and a toxin truck's stream (2 each) that hit a garrisoned building kill that
      many of those inside (of the kinds the data names) and are gone without going off, the
      building's FX at its middle. A shot scattered to a spot near what it was aimed at (the
      flash-bang) still knows what that was and clears it when it comes down on it. KILL_GARRISONED
      damage (the Microwave tank's building clearer: one inside per shot). ImmuneToClearBuildingAttacks
      (the GLA's fortified buildings and the like). The kills are the shooter's (experience, score)
- [x] What a weapon would do, for choosing among a unit's weapons, as the original estimates it
      (WeaponTemplate::estimateWeaponTemplateDamage, ActiveBody::estimateDamage): a token 1 for
      AllowAttackGarrisonedBldgs and SURRENDER weapons against a garrison that can be cleared, for
      KILL_GARRISONED only against one, flames only for shrubs among shrubs, DISARM only against mines
      and traps, a sniper never at an empty building; the weapon bonus in the damage
- [x] A garrison whose last occupant dies goes back to its owner at once (it waited for somebody to
      leave before), and whoever was shooting at it as an enemy's stops
- [x] SWITCH_WEAPON buttons (nine in the game: the Ranger's flash-bangs and back, ...): the weapon of
      that slot until switched back (LOCKED_PERMANENTLY), the button lit while every selected one has
      it in hand, greyed without a weapon there
- [x] Found on the way: the "SMUD..." particle systems (the Microwave tank's heat haze, the GPS
      scrambler's) are drawn by the original as a distortion of the picture behind them - and not at
      all without the hardware or with heat effects off. They were a cloud of white squares; they
      are not drawn now
- [x] Jarmen Kell's shot at a driver (KILL_PILOT, the FIRE_WEAPON button, only at an enemy vehicle
      that has a driver, is on the ground and is no drone): the vehicle stands empty - nobody's,
      unable to move or shoot, its passengers turned out at once, "the pilot losing his face" heard -
      until any infantry climbs in: it is theirs then, and the soldier is its driver.
      Tried: Jarmen Kell selected, his button, a Humvee 180 away clicked - its driver shot at once (f86); a Rebel sent in climbed aboard and drove it for the GLA; a Crusader next, which waited for the rifle's 30-second reload. Units drop a target that has come over to their side or stands empty.
- [x] The hijacker (ConvertToHijackedVehicleCrateCollide, HijackerUpdate) and the terrorist's car bomb
      (ConvertToCarBombCrateCollide), sent into a vehicle with the enter order as in the original
      (the "enter aggressively" cursor). A hijacker takes an enemy vehicle on the ground - no
      aircraft, boat, drone, IMMUNE_TO_CAPTURE, or transport with people in it, nothing hijacked
      before: "vehicle stolen" for its owner, the HijackDriver sound, the higher rank of the two for
      both, a dozer's work dropped. If the vehicle can eject a pilot he rides in it, hidden and out
      of everybody's reach, and is set down where it was when it is destroyed (under a parachute if
      it was in the air); otherwise he is gone into it. A car bomb is any vehicle with a CARBOMB
      weapon set (the civilian cars): his side's from then on with that set - SuicideCarBomb, which
      blows up the car with what it is driven into - his rank, the FX; a booby trap on the car goes
      off first; and if its driver is sniped the car goes up ("a dead-man's trigger for my
      dynamite"). Tried: a hijacker into a Humvee (riding; out again when it was killed), one into a
      Battle Master (gone into it), one into a supply truck; a terrorist into a compact car, the car
      sent at a power plant 260 away (exploded on it, the plant left at 800). The step
      `attack,Type,x,y` was added for that (everything of ours attacks the object of that type nearest
      the spot)
- [x] Tried on Tournament Desert, a German pub with six soldiers in it each time: three Rangers
      switched to flash-bangs (the first grenade killed all six, the Rangers stopped), the Microwave
      tank (one a shot, six in 15 frames), a Dragon tank and a toxin truck (two a hit, three hits).
      A hard three-computer game ran 41000 frames without an error (nobody held a garrison in it, so nothing was cleared).
- [ ] Not done yet: projectiles running into what they weren't aimed at (projectileHandleCollision
      with anything in their path: only the target, or the building a scattered shot was meant for,
      counts); the heat haze itself (a refraction material could draw it); the combat cycle's rider
      killed by the shot instead (RiderChangeContain isn't in); the Ranger's VoiceSwitchWeapon;
      force attack on a building once it is nobody's again; a hijacked vehicle doesn't take on the
      hijacker's vision range; computer players don't hijack or make car bombs yet (their scripts do
      it with command-button hunts)

## Done (M4m: clearing mines)
- [x] WEAPONSET_MINE_CLEARING_DETAIL: a dozer or worker with nothing to build or mend has its
      disarming tool out (DozerAIUpdate / WorkerAIUpdate: "maybe go clear some mines, if I feel like
      it"; "no mine clearing fun while I'm on the job") - the DISARM weapon of its MINE_CLEARING_DETAIL
      weapon set. AntiMine weapons may be aimed at mines and demo traps only; a mine is ANTI_MINE |
      ANTI_GROUND (getVictimAntiMask), so any ground weapon may shoot one it can see
- [x] DISARM damage (MinefieldBehavior::disarm): the mine is gone - or, one that grows back, empty
      for now (its health to the least, nothing left in it, rubble, no target until it has grown
      back); a booby trap or demo trap is simply removed
- [x] An idle dozer goes for a mine it can see within its vision range (getNextMoodTarget looks as far
      as the unit sees; it drives up to it, as a mood target is gone after), and with the weapon's
      ContinueAttackRange it ignores stealth while at it and goes on to the next mine of that player
      within the range of where the first one lay (AIAttackFireWeaponState). Who is clearing mines -
      attacking with an anti-mine weapon - doesn't set off the mines he touches, for as long as he
      keeps touching them (MinefieldBehavior's immunes)
- [x] Command_DisarmMinesAtPosition (FIRE_WEAPON aimed at a spot, USES_MINE_CLEARING_WEAPONSET: the
      button is there even before the tool is out): the tool comes out, and the nearest mine within
      ContinueAttackRange of the spot is cleared first - seen or not ("rather grody hack for disarming
      stuff") - then the rest; with none there it just drives to the spot. FIRE_WEAPON buttons aimed
      at a spot work now (they were only aimed at objects)
- [x] Found on the way: a dozer - which has a weapon now - gets no attack cursor or order over what its
      tool can't be used on ("for the case of dozers, we don't ever want to see an attack cursor
      unless it's valid on a mine"): only what has a weapon for the thing clicked attacks it
- [x] Tried on Tournament Desert with three cluster mines of China's: an American dozer told to clear
      them (the first after 4 seconds, then one every 5 - its reload - none went off); a dozer
      standing 100 short of two of them, which it detects, clearing them by itself. A hard three-computer game ran 41000 frames without an error; the computers' dozers disarmed three of China's mines by themselves.
- [ ] Not done yet: the "mines found" radar events and voices; VoiceClearMines; a computer player's
      dozers told to clear a path (its scripts' "clear mines" actions); the CLEARMINES radius decal
      while aiming

## Done (M4n: demo traps, "detonate now" buttons, the bomb truck's disguise)
- [x] DemoTrapUpdate (the GLA's demo trap and the generals' variants): its three "bogus weapons" are
      in the VETERAN set, and the slot in hand is the mode - proximity (an enemy on the ground within
      TriggerDetonationRange sets it off, looked for every ScanRate; IgnoreTargetTypes; with friends
      near too only where AutoDetonationWithFriendsInvolved; a dozer disarming it doesn't), manual
      (it waits), or detonation (the "detonate" button: off it goes). Its buttons are the
      SWITCH_WEAPON buttons of M4l. It goes off when it is killed too (DetonateWhenKilled), and not
      while being built or sold
- [x] FIRE_WEAPON buttons with no target ("location based fire weapon" where the unit stands): a bomb
      truck's "detonate now", a fake building's, the convoy truck's
- [x] The bomb truck's disguise (SPECIAL_DISGUISE_AS_VEHICLE, StealthUpdate's DisguisesAsTeam): aimed
      at any vehicle on wheels or tracks, it takes that vehicle's look - or the look of what that is
      itself disguised as - over DisguiseTransitionTime, the model changing at the halfway point
      (DisguiseFX, the DisguiseStarted sound, the DISGUISED model state). From then on it is stealthed
      - no enemy picks it as a target - but in plain sight: to its own side as that vehicle in its own
      colours, to everybody else in the colours of the player whose vehicle it copied. Detected, it
      drops the disguise for good (DisguiseRevealTransitionTime, DisguiseRevealFX). The drawing side
      makes an object's models anew whenever the template it is drawn as changes
- [x] Found on the way: units of the kinds CANNOT_RETALIATE (the bomb truck, the Avenger...), IMMOBILE
      and DRONE don't fight back on their own when shot (ActiveBody::shouldRetaliate) - a bomb truck
      used to drive into whatever shot at it
- [x] Tried on Tournament Desert: three demo traps beside a pub two Rangers walked to - the one set to
      manual let them pass, the next one in proximity mode went off as they came by, the third went
      off at its "detonate" button; a bomb truck under fire from a Humvee disguised as that Humvee
      (f91), stealthed a frame later, and the Humvee stopped shooting. A hard three-computer game ran 41000 frames without an error (no computer player uses traps or disguises in it).
- [ ] Not done yet: the disguise's fade in and out (the model just changes at the halfway point);
      DisguiseRevealedSuccess / Failure sounds; the name an enemy sees over a disguised unit is its
      own; a computer player doesn't disguise its trucks or set its traps to manual; trains as
      disguises are not ruled out (no trains in the port)

## Done (M6d: force attack (Ctrl), waypoint mode (Alt), weapons fired at a spot)
- [x] Force attack (Ctrl held, MSG_META_BEGIN_FORCEATTACK; CommandTranslator::evaluateForceAttack):
      with something of ours selected a click no longer selects or orders as usual - on a thing it is
      an attack on it whoever it belongs to (our own, a friend's, a civilian building, a prop that
      only Ctrl can click: FORCEATTACKABLE, which the mouse-over picks up at all times so its name
      shows), on the ground an attack on that spot. Whether it can be done goes by the first of the
      selection, as canAnyForceAttack does: something that can't move (or rides inside something)
      must have it in reach, a spot needs a weapon that fires at the ground, our own stealth and a
      disguise don't hide a target from it. The cursors are the game's ForceAttackObj /
      ForceAttackGround / GenericInvalid (an impossible shot); voices VoiceAttack, VoiceBombard
- [x] A forced attack is kept at (ATTACK_CONTINUED_TARGET_FORCED): the rule that drops a target that
      is ours, a friend's, unmanned or an emptied building lets it be. Riders allowed to fire join in
      (groupForceAttackObject / groupAttackPosition: a Humvee's passengers, an Overlord's add-on)
- [x] Attacking a spot (AIAttackState on a position): the unit drives to a free place within range
      on its side of the spot (outside the minimum range), stops, turns its turret - or itself - to it
      and fires, on and on until it is given another order. "Only Primary attacks the ground" unless
      a switch button has locked another weapon. A FIRE_WEAPON button aimed at a spot
      (MSG_DO_WEAPON_AT_LOCATION) goes the same way now, with the button's MaxShotsToFire and its
      weapon locked till the clip is empty: the toxin tractor walks up to the spot and sprays it, the
      Comanche's rocket pods, the Dragon tank's wall of fire. (Before, they fired once from wherever
      they stood.) The shots of an ObjectCreationList's Attack nugget (the SCUD storm, the gunship)
      still fire from where they are, as before
- [x] Waypoint mode (Alt held, MSG_META_BEGIN_WAYPOINTS): a click adds a waypoint for what moves in
      the selection (MSG_ADD_WAYPOINT -> groupMoveToPosition with addWaypoint): each one's own point
      keeps its place relative to the one nearest the spot, as a group move does, and goes on the
      end of the route it is driving - or, moving somewhere, after that place; or, idle, it sets
      off for it (AIUpdateInterface::privateFollowPathAppend). The cursor is Waypoint; while Alt is
      held the selected units' routes are drawn, from each unit through the points still ahead
      (W3DWaypointBuffer: blue lines, a node at each point)
- [x] Tried on Tournament Desert: a Crusader shelled our own power plant (800 -> 500) on a Ctrl-click,
      then on a Ctrl-click on the ground 390 away drove 240 up and fired at the spot; a Patriot
      battery got GenericInvalid for a spot out of reach and fired at one in reach; a toxin tractor
      sent to spray a spot 340 away drove there and sprayed it (screenshot); two tanks given three
      Alt-clicks drew their routes side by side and drove through them, a fourth point added on the
      way included. A hard three-computer game ran 41000 frames without an error.
- [ ] Not done yet: a plane's attack runs on a spot (force attack on the ground leaves jets out, and a
      jet's forced attack on one of ours is dropped like any other); force attack with shrubbery
      (a flame weapon may aim at a tree: PICK_TYPE_SHRUBBERY); a garrisoned building of ours can't be
      given orders (its soldiers fire on their own); the waypoint line is drawn flat (no EXLaser
      texture, no SCMNode model at the points)

## Done (M6e: pictures on the ground - radius cursors, a strike's target, a gunship's area)
- [x] RadiusDecalTemplate (Texture, OpacityMin / Max, OpacityThrobTime, Color - none: the owner's
      colour -, OnlyVisibleToOwningPlayer), read from InGameUI.ini's 29 radius cursors, from
      SpectreGunshipUpdate's AttackAreaDecal / TargetingReticleDecal and from the DeliverPayload
      nuggets' DeliveryDecal. Drawn as Unreal deferred decals with M_RadiusDecal (texture times colour,
      alpha times the throbbing opacity, a little glow for night maps; `gen_setup.ps1 -Only decal`),
      projected down onto the ground; models take no decals, so they pass under units as the
      original's did. A handful of decal components are made once and reused
- [x] The radius cursor (InGameUI::setRadiusCursor / handleRadiusCursor): while a button with a
      RadiusCursorType is aimed it lies under the cursor - sized by the power's RadiusCursorRadius, or
      for the weapon kinds by the weapon of the button's slot (ATTACK_DAMAGE_AREA its damage radius,
      ATTACK_SCATTER_AREA its scatter, ATTACK_CONTINUE_AREA / CLEARMINES its ContinueAttackRange), and
      for GUARD_AREA by the unit's guard range (vision times GuardInnerModifierHuman). The red ring the
      HUD drew instead is gone
- [x] A strike's target (DeliverPayloadAIUpdate::m_deliveryDecal): from the moment the aircraft is
      sent until it heads off the map, its owner sees the marker where it is going in (the daisy
      cutter's red cross-hair, the carpet bomb's...). A gunship's area and reticle are decals now too
- [x] Tried on Tournament Desert: the A10 strike aimed from the command center (its orange target
      ring under the cursor), a Crusader's guard order (the shield and the 270-wide ring, the tank
      under it untouched), a gunship over Battlemasters (its blue area), a daisy cutter on its way
      (screenshots 136-138)
- [ ] Not done yet: with the cursor over the radar the cursor's decal doesn't move to the place the
      radar points at; the decals don't wrap round buildings (the original's did:
      allowWorldAlign); a script's "hide the UI" doesn't hide them (no scripts)

## Done (M6f: the control bar from ControlBar.wnd and the side's ControlBarScheme)
- [x] A reader for the game's .wnd layouts (Core/GenWnd: windows, rectangles in the creation
      resolution, status bits, the draw data of the three states, texts, fonts, colours, children),
      and `Tools/wnd_dump.py` to look at one (`wnd_dump.py Window\ControlBar.wnd`, from the archives)
- [x] ControlBarScheme.ini (Game/GenControlBarScheme): per side - the frame's image parts by layer,
      the images and places of the bar's own buttons, the money / power bar places, the command
      buttons' border colours by ButtonBorderType, the build-up clock's colour, the right panel's
      emblem, the promotions panel's picture. The scheme is the player's side's (America,
      AmericaAirForceGeneral, ...), as setControlBarSchemeByPlayerTemplate picks it
- [x] The bar laid out from ControlBar.wnd: the command set in ButtonCommand01..14 (slots run down
      the columns), the production queue in ButtonQueue01..09 while something is being built (the
      first one under the inverse build-up clock, drawn as the original's drawRemainingRectClock),
      otherwise the selected unit's SelectPortrait and its UpgradeCameo1..5 (lit once it has them),
      nothing selected: the side's emblem; the radar in LeftHUD; money in the scheme's place; the
      power bar as W3DPowerDraw draws it (production on a log scale, green / yellow / red pieces,
      the slider at the consumption); the general's experience bar (GeneralsExp); the general's
      button (flashing while there are points to spend); the options button (the in-game menu) and
      the idle worker button (InGameUI::selectNextIdleWorker: our dozers and workers with nothing to
      do, in turn, the view on each); the frame's layers under and over the windows. What of the bar
      takes the mouse is what its windows say (neither SEE_THRU nor NOINPUT), so clicks on the open
      parts of the frame reach the world
- [x] The special power tray from the faction's SpecialPowerShortcutWinName (GenPowersShortcutBar*.wnd):
      the powers it has in ButtonCommand1.. from the bottom up, each over its tray picture, at the
      screen's right edge. The promotions panel from GeneralsExpPoints.wnd: the side's
      PowerPurchaseImage, the rank sections' texts, the points to spend, the experience bar, the
      purchase buttons of the three PurchaseScience command sets in ButtonRank1/3/8NumberN, "Done"
- [x] The 800 x 600 layouts keep their shape: scaled to the screen's height and centred (the
      original stretched them to the screen's width); `gen.ControlBarScale` makes all of it smaller
      or larger. Found on the way: the mapped images of HandCreated are read last, over
      TextureSize_512's (Image::load) - the bar's frame is defined in both
- [x] Tried on Tournament Desert: a war factory with three units queued, a Crusader selected
      (portrait and drone / training / armour cameos), the promotions panel at rank 5 (screenshots
      139-141). A hard three-computer game with the new bar ran 41000 frames without an error.
- [ ] Not done yet: the tooltip is still the port's own box (ControlBarPopupDescription.wnd isn't
      used); the bar can't be minimised (ButtonLarge), beacons and the communicator are shown
      greyed; the veterancy chevrons over the portrait, UAttack and the rank-up arrow (GenArrow);
      the OCL timer window (a supply drop zone's countdown) and the observer windows; the empty
      queue cells' picture; the game's own fonts (the engine's is used, at the windows' sizes)

## Done (M2c: the sky box, cloud shadows and the light map, shots on bridge decks)
- [x] The sky box (WaterRenderObjClass: GameData.ini's DrawSkyBox = Yes, SkyBoxScale 8.4,
      SkyBoxPositionZ -100): the model new_skybox.w3d - a top and four sides, a TSMorning texture each -
      scaled so, centred on the camera every frame at that height, drawn as it is (its material is all
      emissive: M_Sky, unlit; `gen_setup.ps1 -Only sky`). The height fog stops short of it. With the
      view pulled back further than the original's, the sky now shows beyond the end of the terrain
      instead of a flat fill. (Only campaign maps give it other textures.)
- [x] Cloud shadows and the light map (UseCloudMap / UseLightMap, on at the medium and high detail of
      GameLOD.ini): the terrain shader multiplies the ground by TSCloudMed - mapped from the world's
      x / y, one repeat every 315 units, sliding by (-0.02, -0.03) of it a second - and by the light
      map TSNoiseUrb (fterrainnoise2.nvp); roads get both too (roadnoise2.nvp). No clouds at night.
      The ground is about a fifth darker for it, and mottled, as the original's is (both textures
      average about 0.86-0.9)
- [x] Bridges' decks: a shell or an armed missile that comes down through the deck of a bridge it was
      over goes off on the deck (DumbProjectileBehavior / MissileAIUpdate::update with
      getHighestLayerForDestination: "blow'd up!"); a building can't be placed on a deck ("we're on
      a bridge. This is somewhat restricted.")
- [x] Not needed for skirmish, checked: WaterType is 0 (translucent standing water) and no map.ini of
      the game overrides it, AdditiveBlending, the cloud map or the sky textures - the reflected sky,
      the water grid and the other water types are campaign things
- [x] Tried: Tournament Desert at the farthest zoom looking past the map's north end (the sky), the
      terrain with its moving cloud shadows, an Inferno Cannon on Twilight Flame force-attacking the
      ground under the bridge - its shells burst on the deck (screenshots 142-144). A hard three-computer game ran 41000 frames without an error.
- [ ] Not done yet: the clouds over bridges (W3DBridgeBuffer draws the cloud stage too); a script's
      macro texture (setMacroTexture: no scripts)

## Done (M7b: the superweapons' countdowns and what the adviser says about them)
- [x] The superweapon countdowns (InGameUI's superweapon timers; InGameUI.ini's
      SuperweaponCountdownPosition X 0.90 Y 0.01, Arial 10): every player's powers with a PublicTimer
      - on a finished building, not waiting for a promotion - player by player and by the power's name,
      in the owner's colour, "Particle Uplink Cannon: 3:58" with the name up to the column and the time
      after it; one that is ready flashes white every 500 ms; past 82 % of the screen's height a "..."
      (the bottom margin). Seen by everybody, as in the original
- [x] The adviser on superweapons (27 of Eva.ini's events): "detected" when a particle cannon, nuclear
      missile silo or SCUD storm (the generals' variants too) is finished
      (Player::onStructureConstructionComplete), "ready" when its countdown runs out - not for one
      ready at the start of the game, again after a power cut ends - and "launched" when it is fired
      (SpecialPowerModule::aboutToDoSpecialPower, which also covers an enemy's GPS scrambler and sneak
      attack), each as Own / Ally / Enemy as the local player sees the owner ("treating NEUTRAL as ally")
- [x] Checked and left as the original has them: "ally under attack" is never said (the radar's
      under-attack check only ever runs for the local player's own things), nor the six "hero
      detected" lines (nothing raises them); "building sabotaged" waits for the saboteur
- [x] Tried on Tournament Desert: our particle cannon and an enemy's nuclear missile silo; at 4:00 the
      adviser said ours was ready, fired it said it was launched, the countdowns read 3:58 (ours, blue)
      and 1:55 (theirs, red) a second later. A hard three-computer game ran 41000 frames without an error.

## Done (M7c: a dozer's "task complete", fear, turrets, the maps' own ambient sounds)
- [x] A building finished by one of the local player's dozers or workers: "<name> construction
      complete." at the top of the screen (DOZER:ConstructionComplete) and the builder's
      VoiceTaskComplete (DozerAIUpdate)
- [x] VoiceFear (ActiveBody::attemptDamage): on falling below a quarter of its health and still alive,
      one time in four (the game's own random numbers, so replays stay the same)
- [x] The turret loop (TurretAI::startRotOrPitchSound): TurretMoveLoop plays while a turret turns or
      tilts and stops when it is still. Wired and traced on an Overlord; every TurretMoveLoop event in
      the data has no sound file, so it is silent - as in the original
- [x] The maps' ambient sounds (Object::updateObjValuesFromMapProperties): an object's
      objectSoundAmbient replaces its template's ambient sound (empty: none), objectSoundAmbientEnabled
      switches it on or off; where the map says nothing, endless loops start on and one-shots start
      off. Objects with a sound and nothing to draw (the Amb_ markers, W3DDefaultDraw) now play it.
      Tried on Alpine Assault: the loops the map places start and play
- [x] Checked and left as the original has them: no sound in the data has the morning / evening /
      night variants, so time of day changes nothing
- [x] A hard three-computer game ran 41000 frames without an error.

## Done (M7d: the voices and sounds that were still missing)
- [x] Order voices as CommandXlat's pickAndPlayUnitVoiceResponse picks them. The weapon an attack uses
      can call for a line of its own: a Ranger's flash-bangs VoiceSubdue (VoiceClearBuilding at a
      building), mine clearing VoiceDisarm; from a button, the shot at a driver VoiceSnipePilot and the
      knife VoiceMelee; at a spot, flames or poison from a weapon other than the first
      (VoiceFlameLocation, VoicePoisonLocation) and the Comanche's rocket pods (VoiceFireRocketPods).
      Switching weapons: VoicePrimaryWeaponMode / VoiceSecondaryWeaponMode. Moving: a GLA worker with the
      worker shoes says VoiceMoveUpgraded; a click on a salvage crate with a salvager selected is the
      original's salvage order (a move to the crate, VoiceSalvage, the move cursor). A terrorist in a car
      bomb adds MiscAudio's TerroristInCar... line to select, move and attack
- [x] Brought in line with the original on the way: force attack answers with VoiceAttack only (its
      message carries no target), force attack on the ground only with VoiceBombard (nothing without
      one), a waypoint only from units standing still, a combat drop only with VoiceCombatDrop, and mine
      clearing with VoiceDisarm (the VoiceClearMines used before isn't in the data)
- [x] A refused building site (PlaceEventTranslator): why, at the top of the screen
      (displayCantBuildMessage: shroud, objects in the way, too close to supplies, restricted terrain,
      not flat enough), the builder's VoiceNoBuild and the "can't do that" beep; the building stays on
      the cursor. The checks run in BuildAssistant's order so the reason is the same one, and sites are
      only legal where the player is looking right now (SHROUD_REVEALED - not in the fog)
- [x] Money (Money::deposit / withdraw): the cash register on every payment and every income, not only
      supply deliveries - units, buildings, upgrades, refunds, selling, oil derricks, hackers, bounties,
      crates, the cash hack (heard by the local player only)
- [x] Things happening: a garrison's door (OpenContain's EnterSound / ExitSound, once a frame); a
      parachute opening; debris landing (an ObjectCreationList's BounceSound, each time it comes down);
      buildings and vehicles losing their power and getting it back (a power cut, an EMP, a hacker -
      not again while already down for another of those; heard where it happens, as in the original,
      where this port had played the building's as a notification); a jet's wheels on the runway
      (AircraftWheelScreech); VoiceLowFuel from aircraft circling over their lost airfield; the stealth
      detectors' IR show on every look (the lens flare at the IRFX bone; both IRPing events have no
      sound in the data - "Dustin says no beep")
- [x] Checked and left as the original has them: VoiceCrush wants force move, whose key is commented
      out of CommandMap.ini; VoiceGroupSelect is read but never played; no jet has a LockonTime (so no
      LockonTickSound); AllCheerSound is multiplayer, BattleCrySound a demo key; WinSound, LossSound,
      NameSound and the like are Generals Challenge's; the End_ music tracks are the campaign's - a
      skirmish has the menu's, the load screen's and the score screen's music (the side's
      LoadScreenMusic goes with the load screen, M8)
- [ ] Not done yet: the trucks' landing and powerslide sounds go with the vehicles' wheels, treads and
      dust (W3DTruckDraw / W3DTankDraw / W3DTankTruckDraw, not ported); an ObjectCreationList's
      FadeSound (one use)
- [x] Tried on Tournament Desert: Rangers switched to flash-bangs (their mode line), sent at a power
      plant (VoiceClearBuilding); a dozer's power plant aimed at that power plant (legal=0, objects in
      the way, VoiceNoBuild and the beep), then put down elsewhere (paid: MoneyWithdraw); a garrison
      entered and left (GarrisonEnter, GarrisonExit); a Patriot battery without power (the power down
      at once, the power up when a power plant was finished); a Humvee killed (three VehicleDebris
      bounces); a paradrop (four ParachuteOpen); a listening outpost next to Jarmen Kell (found him, the
      lens flare blinking on its mast). A Raptor coming back screeched onto the runway (JetSkid); with its airfield destroyed while it was out it circled the ruins and said its RaptorVoiceLowFuel, until the out-of-ammo damage brought it down (a jet already on approach when its airfield falls lands on the ground there - so does the original's: its taxi state switches to the ground locomotor before it looks for the airfield). A hard three-computer game ran 41000 frames without an error.

## Done (M2d: vehicles on the ground - the body on its springs, wheels, treads, dust)
- [x] The body of a tank, truck or motorcycle on the ground (Drawable::calcPhysicsXformTreads / Wheels /
      Motorcycle): it sits on the slope under it (the ground's smoothed normal of getHeightMapHeight - the
      map had no normals before), hangs on springs that pull it towards the ground's tilt, rocks back and
      forward as it speeds up and brakes and leans into turns (AccelerationPitchLimit,
      ForwardAccelerationPitchFactor, LateralAccelerationRollFactor), a wheeled one bounces along at
      speed (BounceAmount), a tank rides up over what it crushes and drops off it with a kick (the crush
      is now kept as the original's "overlap"). Worked out at the end of each logic step, after the
      collisions, and blended between steps on screen; the logic never sees it (weapons fire from where
      the logic has the vehicle, as in the original). Until now ground vehicles stood level on any slope
- [x] Wheels (W3DTruckDraw, W3DTankTruckDraw): they roll with the speed (backwards in reverse; a truck's
      rear wheels faster in a powerslide), the front ones steer, each rides its own spring (the suspension
      offsets of calcPhysicsXformWheels); a tractor's cab and trailer swing into turns. Tires, cab and
      trailer are parts of their own (never merged), moved by bone controls (Control_Bone: shift, then turn)
- [x] Treads (W3DTankDraw): the tread meshes' textures roll while the tank drives and run against each
      other while it turns on the spot - each tank its own, from custom primitive data, as the original
      sets each tank's tread mappers. Every other mesh with a sliding texture (LinearOffsetTextureMapperClass
      on stage 0: the nuclear silo's red glows and the like) now slides too (W3D_UV_HLSL; rebuild the
      materials with `Tools/gen_setup.ps1 -Only w3d`)
- [x] Dirt (particle systems now have the original's velocity, burst-count and size multipliers and its
      trigger): a tank's tread debris left and right, thrown higher and thicker the faster it goes; a
      truck's dust (bigger with speed), dirt spray while it gets going, powerslide spray in turns with its
      TruckPowerslideSound, and the dust and TruckLandingSound of coming down after a jump. All of it
      stops when the vehicle is hidden or out of sight
- [x] Tried on Tournament Desert: a Crusader facing down a 15 degree slope settles at 15.3 degrees nose
      down, a Humvee across a 13 degree slope rolls 17.8 degrees (the original turns the ground's normal
      into angles that roughly; screenshot 146); driving, dust behind the Humvee and debris round the
      Crusader (145); traced (`[vehdraw f..]` once a second): the Crusader turning on the spot (left and
      right treads the opposite ways, 0.74 / 0.26), then all three treads rolling together; the Humvee's
      wheels turning, steering 5.6 degrees into its turn, its rear wheels faster in the powerslide,
      RocketBuggyPowerslide heard. A hard three-computer game ran 41000 frames without an error, its vehicles drawn with all of this.
- [ ] Not done yet: sliding on the second texture stage (108 meshes, glows laid over a base texture - the
      second stage isn't drawn yet), the GRID mapper (the EMP sphere), the body of THRUST locomotors
      (calcPhysicsXformThrust); nothing throws a ground vehicle into the air here, so the landing dust and
      sound haven't been seen; the civilian trucks' cabs and trailers weren't tried

## User's notes (Desktop\notes.txt, 2026-10-23 - before M8b)
The user played Twilight Flame and listed what was wrong; fixed first, with what other generals lacked.
- [x] Icons (China dozer / barracks / war factory / airfield / nuke silo, ranks; USA rally point, attack
      move, guard air, Ranger): the localized texture folder (Data\English\Art\Textures, EnglishZH.big)
      is looked in first - Zero Hour's interface pages live there, the base game's older pages with a
      different layout are under Art\Textures. Checked: every command set's picture resolves; screenshots
      of the China dozer, war factory and rank bars. Buttons for things with `Buildable = No` (the cut
      Biohazard Tech) are hidden, as the original does (getCommandAvailability COMMAND_HIDDEN)
- [x] Cancel a building under construction (Command_CancelConstruction on the under-construction panel:
      full refund); the reactor's overcharge button; the rally point button; star flashing stops once the
      promotions panel has been opened; computer players start with the chosen starting cash (the script
      "Give AI money to cover bug" no longer adds to it)
- [x] Ambient sounds that never stopped (birds, crows, insects, wind): a looping event with a Delay waits
      a new random delay and picks a new file between plays (AudioEventRTS::generateFilename)
- [x] The stutter at the end of every sound (heard on the under-attack siren): when a sound had nothing
      left, our generator returned no samples but left its buffer full, and USoundWaveProcedural played
      that buffer again. Recording: the siren's tail is now followed by the ambient bed only
- [x] Particle cannon: fired from the right-hand tray; it stays selected after a shortcut fire and is
      steered with a click; its connector beams come from the right bones (bShownLast); the spot fired at
      (and the targets of other powers with ViewObjectDuration/Range) is revealed for a while. Damage and
      area are the original's exactly (100 a pulse, 40 pulses, radius 44, the trail remnants' 15 every
      0.25 s) - not changed without the user's say
- [x] Strategy center battle plans (BattlePlanUpdate): bombardment (the gun comes out and fires; its
      PRIMARY NONE weapon is still chosen by the turret's idle scan, as in the original), hold the line
      (army 0.9 damage, the center 2x health), search and destroy (army 1.2x sight, the center 2x sight and
      stealth detection); doors per plan, unpack/pack timing, sounds, message, units paralyzed 5 s on a
      change; the bonuses go to new and captured units too. Tested: all three plans in turn, gun firing,
      15 units paralyzed, 3000/3000 health under hold the line
- [x] MOAB: a power button that lists sciences takes the picture and text of the best owned one's
      promotion button (DaisyCutter -> MOAB, A-10 levels); the SCRIPT_ONLY MOAB promotion is no longer in
      the rank window
- [x] Comanche: PreferredAgainst (cannon vs infantry, missiles vs vehicles) and "readiness trumps
      criteria" in weapon choice; and (not the original, asked for) a helicopter whose preferred weapon is
      reloading fires another that is ready and in reach - the cannon works on vehicles between missile
      volleys. Flares: launched once the airfield's countermeasures upgrade is done (tested)
- [x] Helix napalm bomb: the bomb falls from the helicopter and goes off on the ground (tested)
- [x] GLA Stinger Site: its soldiers (SpawnBehavior that keeps its units: at the SpawnPoint bones, held
      there, replaced 30 s after dying, die with the site; HiveStructureBody passes small arms / sniper /
      poison damage to the nearest soldier; an attack order for the site is theirs)
- [x] Supply drop zone: a countdown and progress bar when selected (CB_CONTEXT_OCL_TIMER, with the sell
      button); crates no longer land short - each glides as fast as it has to to touch down on its own
      spot of a small spiral on the pad (not the original: they bunched up or fell short); a parachute's
      rider hangs below the canopy (PARA_MAN / PARA_ATTCH), the canopy hidden until it opens
- [x] Vehicles leave a factory straight out of its door (aiFollowExitProductionPath: no path search from
      inside the building, the factory no obstacle)
- [x] Buildings turn while being placed: hold the left button on the spot and drag (PlaceEventTranslator's
      anchor); test step `target,x,y,degrees`
- [x] Group moves "like in modern games" (not the original's two fixed lanes 15 units either side of the
      way): as many lanes as the group needs, spread across the free width at each corner (ground or bridge
      deck). Twilight Flame's middle bridge, 12 Crusaders: four abreast across the whole deck (was two on
      one half), longest wait 89 frames (was 317)
- [x] A-10: the cannon pass before the missiles was there but silent - its fire sound plays now
- [x] China Satellite Hack I / II (SpyVisionUpdate by upgrade): the enemy's command centers' sight for good,
      everything's for 20 s every 4 minutes; off while the internet center is disabled
- [x] Idle-worker ("free dozer") button: lit for idle dozers again (the check wanted a supply truck's state)
- [x] Point defense lasers (PointDefenseLaserUpdate - the Air Force General's jets, the Paladin): missiles
      coming in are shot down (tested: a Paladin downs a tank hunter's missile every second)
- [x] Units born veteran (VeterancyGainCreate, a hijacker taking a vehicle's rank): no promotion sound
- [x] GLA buttons that did nothing: hijack and make-car-bomb (the vehicle clicked is entered), the
      saboteur (Sabotage...CrateCollide: power cut, factory / internet center shut down, powers reset, cash
      stolen, fakes blown up; tested on a power plant)
- [ ] Spectre gunship "goes outside the circle": its orbit follows the original (about 185 units round
      the target, the data asks for 250 - wider than the 200 attack area); flying 130 units up, it shows
      on screen about 170 units further away than the ground under it. Left as is until the user says how
      it should look
- [ ] Not done yet for other generals: the Tank General's tank paradrop (SPECIAL_TANK_PARADROP), the
      ambulance / hazmat cleanup (SPECIAL_CLEANUP_AREA), the GLA angry mob (MobNexusContain and the
      spawner's budding exit). (Units getting stuck: the movement section of 2026-10-27 below.)

## Done (MP1: network games - orders, replays, the host's server, coming back to a match)
The user's wishes (2026-10-23): a modern client-server game; a player who drops out can come back; free
hosting on their own PC or by one of the players, joined by address over the internet; zero mismatches;
a dropped player's army is commanded by its human allies only (never the computer); everybody on the same
build. Design: lockstep through a server (Net/GenNet.h) - every game works the whole match out itself from
the same settings and orders; the server is the clock and the record.
- [x] Orders: every `Issue*` builds an FGenCommand (Logic/GenCommand.h: type, player, units, target, spot,
      numbers, name), applied at the start of a logic frame by `ExecuteCommand` (GenOrders.cpp) - only to
      what the giving player may command (`CanPlayerCommand`: its own; a dropped-out human ally's too).
      The orders the logic gives itself (an AI selling a captured building) go through `QueueSimCommand`
      (not recorded or sent). The test switches that changed the game by hand (`money`, `kill`, `grant`,
      `skill`, `aiskill`, `hunt`, `damage`, `vet`, `destroy`) are orders too (`Cheat`, ExecCheat)
- [x] Match settings (Logic/GenMatch.h FGenMatchSettings): map, cash, seed, slots, the test switches that
      change the simulation (-GenNoAI, -GenDifficulty, -GenNoShroud's player, -GenPowersReady) and the
      console variables that do (gen.LogicFPS, PathCellsPerStep, PathCliffMargin, PathWeight, MissileFlight)
- [x] Checksum after every frame (`ComputeChecksum`: frame, next id, the random numbers, every player's
      money/ranks/sciences, every object's place, heading, health, target); `-GenStateDump=F[,F]` (and a
      mismatch) writes the whole state as text (`Saved/Logs/StateDump_<tag>_f<F>.txt`, `-GenDumpTag=`)
- [x] Replays: every game records itself (settings, the objects the start put down, orders, checksums) to
      `Saved/Replays/LastReplay.genrep` (every 2 minutes, at the score screen, on leaving; `-GenRecord=` names
      it, `-GenChecksumLog=` adds a frame-per-line text of the checksums). `-GenReplay=file` plays one back
      (`-GenReplayViewer=N` as another player) and logs the first frame that differs
- [x] Determinism measured (2026-10-23): a 40-minute game of 7 hard computer players on Twilight Flame
      (72,099 frames) replays identical at x8 on the game thread, as player 4 (first 55,800 frames), and at
      x2 with the simulation's own thread (first 10,800); network games identical frame for frame between
      the players (below). Not measured yet: two different CPUs (Intel / AMD) - a friend running a replay
- [x] The server (Net/GenNetServer.cpp, `-GenServer [-GenPort=8088]`, the game's program without screen or
      sound): lobby (FGenSetupData - the skirmish setup's rules; each player changes its own place, the host
      the rest), refuses another build / other game data, starts the match (Resolve), waits for every map
      to load, then closes 30 frames a second with the orders that came in; keeps every order (rejoin), compares
      the games' checksums (a mismatch is logged and told to everybody), tells the simulation when a player
      drops out or comes back (`Session` orders "drop" / "return"), and closes 10 minutes after the last
      player left (writing `Saved/Replays/Server_<date>.genrep`). Opens its port on the router by UPnP
      (Net/GenUPnP.cpp; `-GenNoUPnP` not) and tells the players the public address
- [x] The game's side (Net/GenNetClient.cpp, Net/GenNetSubsystem.cpp): connects (a name or an address,
      `host[:port]`), gets the lobby, sends orders and checksums, buffers the server's frames for the
      simulation (IGenOrderSource: a frame is worked out only when the server has closed it); a connection
      that breaks is made again with the seat token for 15 minutes; a game that was closed comes back with
      `-GenRejoin` or the menu's REJOIN (the seat is kept in `Saved/Net/Seat.txt`) and catches up from the
      start (about 125x real time early in a match). In a network game: x1 speed only, no pause, no restart
- [x] The menu: HOST ONLINE (starts the server program in the background and joins it), an address box and
      JOIN, REJOIN; the network lobby is the skirmish screen showing the host's setup (other players' places
      read-only, START GAME for the host); in a game, the network's news at the top of the screen
- [x] Allies: while a human is away, its human allies select and command its army (the selection, the drag
      box, the command bar, control groups use `CanPlayerCommand`); they don't sell its buildings
- [x] Release: packaged (Development, with logs) into `E:\Games\My Generals`; `GamePaths.txt` beside the
      program names the Zero Hour / Generals folders when the registry doesn't (Steam, EA app)
- [ ] Not done yet: the server working the match out itself as a referee, and full save / load of the
      game's state - what would repair a game that came apart (sent the referee's state) and make coming back
      instant; battle groups in network games; chat in the game; the original's network screens (with M8b);
      a different CPU tested; a test over the internet with a friend

## Done (M8d: options, main menu, hotkeys - and the user's notes of 2026-10-25)
The user asked for settings (resolution, frame rate, quality; hotkeys; scroll and mouse speed; the mouse
buttons swapped) and a well designed menu, modern rather than the original's 2003 look (M8b stays open for
the original's window files). Built on one foundation, then seven parts in parallel and merged.
Taken here from galapchevd/Generals-private on 2026-10-27: only the options, menus and hotkeys - not that
branch's stealth while firing, forgiving picks (exact picking stays, 2026-10-13), moves onto unreachable
ground, USA power balance, add-ons, Macedonia, online browser or control bar.
- [x] The player's settings: `UGenUserSettings` (Game/GenSettings.h, GameUserSettingsClassName) - the
      engine's own (resolution, window mode, frame rate limit, VSync, scalability) and the game's: controls,
      keys, volumes, video extras. Saved in GameUserSettings.ini (a test run's under Saved/Config/WindowsEditor)
- [x] Every key the game reads goes through one table of commands (Game/GenInput.h, the original's
      CommandMap.ini commands, each on a key the player may change); Esc always opens the menu
- [x] The options screen (Game/GenOptions.cpp) over the main menu or the in-game menu: Video, Controls,
      Hotkeys, Audio pages (a file each), Apply / Cancel / Reset to defaults, Esc cancels. New shell widgets:
      tabs, slider, key button and key capture, scrolling area, tooltip
- [x] Video (GenOptionsVideo.cpp): fullscreen / borderless / windowed, the display's resolutions, VSync,
      frame rate limit (default none with VSync, so the display's refresh rate paces it: 240 on the user's
      240 Hz screen, 60 on the laptop that ran hot uncapped - was 60 fps until 2026-10-27, DefaultsVersion 2
      moves a stored 60 to none), preset Low..Epic or Custom over eight
      scalability groups, resolution scale 50-100% (TSR), ray-traced lighting (off by default: r.RayTracing was
      on but GenMapActor's post-process had Lumen off - it only cost; on, it switches the map's post-process to
      Lumen GI and reflections; not yet looked at in game), a frame rate read-out, and a 15 s "keep these
      settings?" countdown after a display change. The game's defaults are put in once (DefaultsVersion): the
      engine's first start otherwise took Epic without VSync from the console variables. No display change in
      the editor or a test run (saved only)
- [x] Controls (GenOptionsControls.cpp, GenPlayerController's UpdateCamera / WorldLeftClick / RightClick):
      edge, key, drag, zoom and rotate speeds (25-300%), edge scrolling (and in a window), inverted drag, the
      mouse kept in the window, an experimental pointer speed; the Modern button layout - left selects (the
      ground deselects), right gives the order a classic left click would (Ctrl / Alt as ever), right drag
      still scrolls; placing and aiming are left / right in both. Test step `mouse,scheme|left|right|camera,...`
- [x] Hotkeys (GenBarHotkeys.cpp, GenOptionsHotkeys.cpp): the command bar's buttons by their keys - Classic
      (the '&' letter of the button's name in the CSF, as the original; '&' is no longer dropped from other
      text, "C&C" reads right now), Grid (ControlBar.wnd's 2 x 7 cells: QWERTYU / ASDFGHJ) or Custom per
      button; a bar key is the button's while it is shown, with exactly its modifiers (Ctrl + a number stays a
      group). The key is shown in the button's corner and its tooltip. The page rebinds every command, offers
      a swap on a clash, and lists an army's buttons (or the selection's) for custom keys. Test step `key,<chord>`
- [x] The menus (GenShell.cpp, GenShellStyle.h): a new look for every widget and screen (layered panels,
      gold for the main action, eased hover); a main menu at the start (skirmish, online, options, exit); the
      host / join controls on their own online screen; BACK and Esc between screens; the in-game menu with
      options and quit to desktop (asked first). A -GenTestMenu script starts on the skirmish setup unless it
      uses Main. ids
- [x] Volumes (GenOptionsAudio.cpp, GenAudioManager): master, music, sound, voices, speech - heard while
      dragged, undone by Cancel; culling still goes by the game's own volumes
- Workflow: Tools/gen_build.ps1 builds a checkout; gen_lock.ps1 lets one build or test run at a time across
  worktrees. A build in one worktree rewrites the engine's UnrealEditor.modules BuildId: another worktree's
  DLL then doesn't load until it is built again

## Done (the user's report of 2026-10-27: damaged helicopters never landed at the airfield)
"If I send a damaged helicopter to the airfield, it just hovers over it" - and a captured Chinese base's
Helix should be mended at an American airfield too.
- [x] Comanche (JetAIUpdate): the flight home (JetOrHeliReturnForLandingState, an AIInternalMoveToState)
      ends over the spot at the helicopter's own height; only the way straight down or up
      (HeliTakeoffOrLandingState) needs 3 units in all three axes - it had both, so the flight home never
      ended. That descent gives the locomotor an explicit goal (onPathDistToGoal 0), which brakes and slides
      it onto the point; with the distance passed it circled the approach point instead
- [x] Where it comes down: doLandingCommand's findPositionAround of the airfield (ten bounding radii) -
      `UGenGameLogic::FindFreeGroundAround`: rings 5 apart from a random angle, passable ground with nothing
      within 5 - beside the airfield, a different spot each time, not all on the HeliPark01 bone
- [x] canGetRepairedAt for aircraft: in the air, damaged, at an ally's FS_AIRFIELD of any side; a click on
      the airfield gives the GetRepaired cursor and order. Jets land as told to (privateGetRepaired);
      Chinook and Helix (ChinookAIUpdate::privateGetRepaired): open ground within a hundred radii, land
      there, mended by the airfield while down (setHealee), up again once whole or once the airfield is gone;
      any order ends the stay (aiDoCommand's setAirfieldForHealing(INVALID_ID))
- [x] Tested on Tournament Desert: two Comanches told to land (one at 88 / 220) came down on either side of
      the airfield, the healthy one rose at once, the other after 398 frames at 220; a Helix (120 / 300) and a
      Chinook (150 / 300) at an American airfield, by the `fix` step and by a left click; replay of the scene
      against a Chinese computer player: every frame the same (9849 frames).
      Recipes: `-Objects "AmericaAirfield@1170:520" -TestQueue "AmericaVehicleComanche,AmericaVehicleComanche"
      -TestPowers "35,money,5000;2600,damage,AmericaVehicleComanche,40" -TestJets "2500,move,1170,900;2700,land"`
      (pause f3100 with `-Cam "1150,600,0,300"`); `-Objects "AmericaAirfield@1170:520,ChinaVehicleHelix@1100:950"
      -TestSelect ChinaVehicleHelix -TestPowers "300,damage,ChinaVehicleHelix,40;400,leftclick,1170,500"`
      (pause f850); grep `[jet f`, `[jetpos f`, `[repair f`

## Done (the user's report of 2026-10-27: vehicles bump into each other and get stuck)
"If they move towards each other they could get stuck... if the vehicles are standing very close and I
move one, it tries to turn but the other vehicle is blocking it." Two causes were the port's, the rest the
original's stop-and-wait design; the user chose to replace that with modern steering (option 2 of three:
faithful repair / steering on the original's paths / flow fields - AoE IV's and SupCom 2's - left for later,
it would replace only the route, not the steering).
- [x] Port bugs: the locomotor's *blocked branch was missing (Locomotor::locoUpdate_moveTowardsPosition: a
      blocked unit stops and turns on the spot whatever its locomotor) - a wheeled vehicle turns only as fast
      as it rolls, so one held to speed 0 could never turn out from behind what blocked it (Humvee behind a
      parked Crusader: 12 s then the order dropped). And the port's own give-up (4 s / 12 s without speed)
      didn't reset when the way cleared and counted a braking unit's slide as no progress: a unit that had
      stepped aside dropped its own order. Now: ground covered or turned through counts, 6 s caught on the
      ground or 20 s waiting on units, `[steer f..] ... gives up its move` in the trace
- [x] Steering (Logic/GenSteering.cpp, beyond the original): every frame a moving vehicle looks 1.5 s ahead
      and tries nine ways (as far round as its locomotor turns in that time - treads slow to turn, wheels
      turn as they roll) at two paces, the pace of the unit ahead, the way it held last frame and standing;
      it takes the cheapest of turning off the path, going slower and running into somebody (an oncoming
      unit counts half - it steers too; traffic going its way only when close; a tie keeps right). Soldiers
      steer round vehicles; soldiers among soldiers, docking lanes and factory doors keep the original's rules
- [x] Idle allies in the way make way (Pathfinder::moveAllies / getMoveAwayFromPath: the nearest free spot
      clear of the mover's way, on the side they are on) and go back to where they stood; a unit that was
      going somewhere goes on. Parked group mates (same group order, `MoveGroup`) and parked allies in the
      last 6 cells of a move are eased past at half pace instead, never sent away; what is held to its place
      (docking, building, packing, an ability, a factory door) is never pushed or eased into
- [x] Push pass (UGenGameLogic::UpdateCollisions): what overlaps is pushed apart, half the overlap a frame,
      15 units/s at most, never onto ground the unit can't stand on - a soldier gives all of it to a vehicle,
      an idle ally 3/4 to the unit driving into it, an enemy standing its ground none
- [x] Deadlocks: each waiting on the other - the lower priority unit makes way; still wedged 2 s on (or held
      up by a unit that doesn't steer): the original's cheat, it drives through for 1.5 s (`drives through`
      in the trace). A turn far round keeps its direction (no rocking between two ways across the back);
      a vehicle doesn't pivot into a neighbour. Path round parked units keeps a cell clear for vehicles
- [x] `gen.Steering` (default 1; a match setting - replays are version 2 now) gives the original's stop and
      wait back, for A/B tests
- [x] Tested (Tournament Desert unless said; frames until the last one arrives, steering off -> on): Humvee
      behind a parked Crusader gave up -> f211 (the Crusader makes way and goes back); Humvee beside one,
      ordered into it -> f211; Humvee and Ambulance head on 4.5 s jam, the Ambulance dropped its order ->
      they pass keeping right, f181; six Humvees east through six Crusaders west: f752 / f928 with 3 still
      going -> f570 / f840; Twilight Flame bridge, 12 Crusaders: longest wait 582 frames, one never arrived ->
      18 frames, all by f1552 (at x2); AI games against a hard China (seeds 11, 4, 23, 5.6 min): 16 give-ups
      -> 0, supply deliveries unchanged (148-153); replays of 8-player Twilight Flame games: every frame the
      same (28965 frames). Soldiers through parked tanks: as before (their pace is their route's)
- [ ] Not done: the destination of a big group still takes a few seconds to settle (late arrivals ease in
      among the parked ones); soldiers among soldiers keep the original's pass-through

## Next
(Performance: no profiling between milestones - one review at the very end, the user's word of
2026-10-12. Things to look at then are noted where they are made: FindGroundPath asks for the
clear width round every cell it opens - a long group move is the dearest single search now. A
wheeled vehicle moving in view poses its model every drawn frame for its wheels (UpdateVehicleDraw), and
every ground vehicle's body is stepped every logic frame (UpdateChassis), parked ones too. Steering
(`Obj.Steer`): every moving ground unit asks the partition about a 1.5 s look ahead every logic frame, and
one with somebody in its way predicts up to ~22 ways against each neighbour.)
- [x] The powers that were greyed out (M4c, M4i): all in
- [x] What units couldn't do (the "not done yet" lines of M4c, M4e, M4f, M4i, M5b, M6b): what
      ObjectCreationUpgrade makes - M4j; crates - M4k; clearing a garrison, the shot at a driver,
      hijackers and car bombs - M4l; clearing mines - M4m; demo traps and disguises - M4n
- [x] M6 UI: picking, cursors, hover and groups - M6c; force attack and waypoint mode - M6d; the
      radius decals - M6e; the control bar from the game's own layouts - M6f
- [x] M2 rendering completeness: the sky box, cloud shadows and the light map, bridges' decks - M2c
- [x] M7 audio leftovers: the superweapons' adviser and countdowns - M7b; task complete, fear, turrets,
      the maps' ambient sounds - M7c; the voices and sounds still missing - M7d. Left: volume settings
      (with the options, M8), surround output, the trucks' sounds (with the vehicles' wheels and treads)
- [x] The vehicles' wheels, treads and dust, and their bodies on the ground - M2d
- [ ] M8 leftovers: see M8a. Next: M8b, the shell drawn from the original's window files - a small
      window manager over Core/GenWnd (the gadgets: push buttons, check boxes, combo boxes, list boxes,
      sliders, text entry, static text, with WindowTransitions.ini's transitions) and the menus' callbacks
      ported onto it: MainMenu.wnd (with its still backdrop, as the original shows when the shell map is
      off), SkirmishGameOptionsMenu / SkirmishMapSelectMenu, MultiplayerLoadScreen, ScoreScreen, QuitMenu,
      OptionsMenu (sound volumes, resolution, scroll speed), MessageBox. Then M8c: the shell map behind
      the main menu (Maps\ShellMapMD: 24 teams and camera moves - it needs the map script engine)

## Workflow notes
- C++ builds need the editor closed (Live Coding blocks external builds).
- `Tools/gen_setup.ps1 [-Only water|roads|shroud|w3d]` runs `ue_setup.py` (below) and prints what matters.
- Roads and bridges: the log says how many pieces of each kind a map has ("Roads: ...") and where each
  bridge is ("Bridge '...'", "Pathfinder: bridge layer ..."); with `-GenTrace`, `[bridge f..]` lines say
  when a unit drives onto a bridge and leaves it. A crossing test:
  `gen_test.ps1 -Map "Twilight Flame" -Cam "2380,2750,0,450" -NoAI -Objects AmericaTankCrusader -ObjectsAt "2384,2480" -TestMove "2376,3020" -Trace`
  (over the bridge; from "2200,2750" to "2560,2750" goes under it).
- Movement / steering (2026-10-27): with `-Trace`, `[steer f..]` lines say who makes way for whom (and
  where it goes back to), who paths round parked units, who drives through a wedge and who gives up a
  move. `-Extra "-GenSteerTrace=<id>"` logs that one unit's steering choice and locomotor every frame
  (want angle, what straight on runs into and when, the way and pace taken; aim, speed, wait, velocity,
  turning, braking) - object ids repeat with the same setup and seed, so find the id in a first run.
  `-Extra "-GenStateDump=F1,F2 -GenDumpTag=x"` writes every object's move / path / blocked / stuck at those
  frames. A/B against the original's stop and wait: `-Extra "-ini:Engine:[ConsoleVariables]:gen.Steering=0"`.
  Jam scenes on Tournament Desert (`-NoAI -Trace -Cam "1005,780,0,300"`): Humvee behind a parked Crusader
  `-Objects "AmericaVehicleHumvee@1005:760:0,AmericaTankCrusader@1035:760:0" -TestPowers "60,move,1205,760,AmericaVehicleHumvee"`;
  head on (AmericaVehicleMedic is a second wheeled type for `move,x,y,Type`)
  `-Objects "AmericaVehicleHumvee@950:760:0,AmericaVehicleMedic@1060:760:180" -TestPowers "60,move,1150,760,AmericaVehicleHumvee;60,move,850,760,AmericaVehicleMedic"`;
  two groups crossing: six Humvees at x 860 (y 700 + 24 i, angle 0) and six Crusaders at x 1150 (y 700 + 26 i,
  angle 180), `60,move,1250,760,AmericaVehicleHumvee;60,move,760,760,AmericaTankCrusader`. AI soak: `-Enemy
  FactionChina -Difficulty Hard -Speed 8 -Delay 75 -Trace -Seed 11 -Extra "-GenTestUnits"` and count
  `gives up its move` (0 now) and `delivers`.
- Capturing and tech buildings: `-GenTestPowers` steps `capture,x,y` (every local unit that has the
  ability captures the building nearest that spot; the log says how many could) and `cost,Template`
  (logs what the local player pays for it now). `-GenTrace` adds `[ability f..]` (begins / captures),
  `[capture f..]` (a building going from one player to another), `[cost f..]` (price changes),
  `[ocl f..]` (pad and drop zone deliveries), `[crate f..]` (a crate picked up). A capture test:
  `gen_test.ps1 -Map "Tournament Desert" -NoAI -Speed 8 -Trace -Objects AmericaBarracks -ObjectsAt "520,900" -TestQueue "AmericaInfantryRanger,AmericaInfantryRanger" -TestPowers "35,money,5000;40,upgrade,Upgrade_InfantryCaptureBuilding;1700,capture,350,980"`
  (the derrick at 350,980 is taken at about f3000; the other one is at 1765,1877). The upgrade step
  needs a building of ours that offers it. Through the command bar instead: `-TestSelect AmericaInfantryRanger`
  with a Ranger placed by `-Objects`, then `1450,click,Command_AmericaRangerCaptureBuilding;1455,target,350,980`
  (`target` aims a power that wants an object at the one nearest the spot; the button is greyed for
  the 15 s the ability takes to charge once the upgrade is in).
- Hackers and unit abilities: `-GenTestPowers` steps `hack` (our hackers hack the internet),
  `ability,CommandButton[,x,y]` (every unit of ours with that button uses its ability on whatever it
  may be used on nearest that spot; without a spot on nothing - Burton's detonator),
  `weapon,CommandButton,x,y` (the same for a FIRE_WEAPON button), `hunt,CommandButton` (our units
  hunt with the button as the computer player's teams do) and `status,Type` (anybody's objects of
  that type: owner, health, HACKED / DISABLED / TRAPPED / HACKING; and our money). `-GenTrace` adds
  `[hack f..]` and `[charge f..]` lines and more `[ability f..]` ones. A power is not ready for its
  reload time after the unit appears (the cash hack: 2 s), so don't fire it at frame 40. Examples:
  `gen_test.ps1 -Faction FactionChina -NoAI -Objects ChinaInfantryHacker -ObjectsAt "1005,760" -Enemies AmericaBarracks -EnemyAt "1005,1000" -Cam "1005,900,90,220" -TestPowers "100,ability,Command_ChinaInfantryHackerDisableBuilding,1005,1000;500,status,AmericaBarracks" -Speed 8 -Trace`
  (pause at f880 to see the stream), and with `-Objects AmericaInfantryColonelBurton -Enemies ChinaPowerPlant -Enemy FactionChina`:
  `"100,ability,Command_ColonelBurtonRemoteDemoCharge,1005,1000;700,ability,Command_ColonelBurtonDetonateCharges"`.
  The hackers' stream shows a different slice of its texture every frame, as the original's does: in
  a single frame it can be all but invisible.
- Building, producing, selling: `-GenTestPowers` steps `build,Building,x,y` (an idle dozer of ours
  puts it up there; the log says whether the spot is legal and the thing can be made), `queue,Unit`
  (at the first building of ours that makes it) and `sell,Building`. `-GenTrace` adds `[door f..]`
  (a door opening for a finished unit, the unit leaving, the door closing), `[build f..]` (ground
  levelled, a building being sold / sold) and `[power f..]` (a powered thing losing or regaining
  power). The run of M4g, a picture per `-PauseFrame`:
  `gen_test.ps1 -Map "Tournament Desert" -NoAI -Faction FactionAmerica -Speed 8 -Cam "1005,770,0,230" -Objects "AmericaSupplyCenter,AmericaPowerPlant" -ObjectsAt "780,600" -TestPowers "40,money,20000;45,build,AmericaWarFactory,1005,790;900,queue,AmericaTankCrusader;1500,sell,AmericaWarFactory" -Trace -Extra "-GenNoTestUnits" -PauseFrame 400`
  (150: scaffolding rising; 400-700: the building coming up; 840: finished; 1275: door open; 1600:
  sinking). `R.StateBusy#` in a `-GenPerf` report counts the objects with a transition under way: it
  should stay small (a number that only grows is a transition that never ends).
- Where the time goes: `-GenPerf` logs three `[perf f..]` lines every 30 s of game time (every 10 s
  at normal speed): frames a second, frame times and how many ran over 33 / 50 / 100 ms, object
  counts; the engine's game thread / render thread / GPU times, draw calls, memory; then every timer
  (`GEN_PERF_SCOPE("Name")` in Core/GenPerf.h; names starting "R." are per drawn frame, the others
  per logic step; names ending "#" are counts). `Tools/perf_sum.py <log> [every]` prints one line
  per report. `-GenBots=N` gives a test run N computer players (sides in turn, start positions
  2..N+1); `-GenTestPowers` steps `speed,N` (the game's speed from there on) and `busy` (the camera
  goes where most units are). The run this was measured with:
  `gen_test.ps1 -Map "Twilight Flame" -Difficulty Hard -Speed 8 -Seed 3 -Delay 215 -TestPowers "24000,busy;24001,speed,1" -Extra "-GenBots=7 -GenPerf -GenTestUnits"`
  (two runs with the same seed are not the same game: compare costs, not frames). If the game
  crashes, `gen_test.ps1` still waits out its delay - look for "Assertion failed" in the log.
  A fourth line, `script conditions`, has what the computer players' conditions cost by kind. The
  step `exec,<console command>` runs a console command at that frame: `24400,exec,stat dumpave
  -num=240 -ms=0.1` writes the engine's own averages per thread to the log (`LogStats`), which is
  where costs outside our timers show. `-Objects "Template*N"` puts a block of N on the map (a
  crowd: `-Objects "AmericaInfantryRanger*120,AmericaInfantryMissileDefender*120" -Cam
  "1005,800,0,330" -TestPowers "60,move,1005,1000" -Speed 1 -Delay 45`). To try a change both ways
  in one run: `gen.PoseBudget`, `gen.CullTransforms`, `gen.PathWeight`, `gen.SimThread` (0: logic
  steps inside the frame, as before; `-GenNoSimThread` does the same from the start). Steps also run
  inside the frame at game speeds above x2, so a `-Speed 8` soak tests that path and its `speed,1`
  part the thread. The `pacing` line of a report is the one to read for a 240 Hz screen: the share
  of frames over 4.2 / 5 / 8.3 ms, what a step takes from asked to done, how far behind the screen
  runs, in how many frames a late step held it, and how long the game thread waited at the gate;
  the line after it names where the step went longest without a safe point (keep it near 1 ms) and
  times the engine's garbage collections. `perf_sum.py` prints these as `>4.2ms .. thr wall lag
  holds gate`. `-Cam "x,y,0,900"` before a `busy` step measures the view zoomed all the way out.
  Models and garbage: a report's `models shown` and `parts shown, the models with most` lines count
  the parts on screen; `-GenNoMerge` (or `gen.MergeParts 0`) draws every mesh by itself again, for
  comparing pictures (`-PauseFrame` on an AI game with the same seed gives the same base both ways);
  `-GenPlans` logs for each model which meshes are one and why the others aren't (moves / blended /
  shown or hidden by name). `gen.AssetMemory` logs what models, animations and textures take.
  `gen.ObjectCensus` logs the classes with the most engine objects: asked before and after an
  `exec,obj gc` step it shows what the garbage was made of, and `exec,log LogGarbage Verbose` before
  it what each phase of the collection took (`exec,gc.TimeBetweenPurgingPendingKillObjects 12` makes
  the engine collect by itself during a test).
- Placing test objects: in `-Objects`, `Template@x:y` or `Template@x:y:angle` puts that one (or the
  middle of a `*N` block) there instead of in the row; `-Extra "-GenObjectAngle=deg -GenObjectSpacing=d"`
  turn and space the members of a block (a fence: `"ChainLinkFence01*12@985:935"` with angle 0 and
  spacing 27). Fences, crushing and groups, as tried for M3h:
  `gen_test.ps1 -NoAI -Cam "985,930,0,200" -Objects "AmericaInfantryRanger,AmericaTankCrusader,ChainLinkFence01*12@985:935" -ObjectsAt "1005,900" -TestPowers "20,move,985,975" -Delay 6 -Trace -Extra "-GenNoTestUnits -GenObjectAngle=0 -GenObjectSpacing=27"`
  (the tank through, the Ranger round; the flattened pieces are gone ten seconds later);
  `-Objects "AmericaTankCrusader@1005:900:90,neutral:CarCompact@1005:940,neutral:CarCompact@1005:985:0,neutral:CarCompact@1021.6:1030:83" -TestPowers "20,move,1005,1075"`
  (front, whole and back crushed); `-Objects "AmericaTankCrusader*6@1005:900,AmericaInfantryRanger*5@1005:860" -TestPowers "25,move,1700,2330,SELECTABLE" -Speed 6 -Delay 22 -Cam "1690,2310,0,260"`
  (columns across the map); `"10,formation,CAN_ATTACK;25,move,1005,1300,CAN_ATTACK"` (a formation);
  `-Objects "AmericaPowerPlant@1000:1000,AmericaPowerPlant@1070:1000,AmericaTankCrusader@1035:940:90" -TestPowers "20,move,1035,1070,CAN_ATTACK"`
  (a gap a cell wide: round it). `-GenTrace` adds `[group f..]` lines: whether a way for the group was
  found and with how many corners, every unit's lane, file and rank, formations made and undone, a
  group gathering in. The load log's `Pathfinder:` line counts the cells beside cliffs and the pinched ones.
- Missiles and superweapons: `-Extra "-GenPowersReady"` makes every power ready at once (its science
  is still needed); without it the clocks are real - the cannon is ready at f7200, the storm at
  f9000, the missile at f10800, so fire them at f7300 / f9100 / f10900 with `-Speed 8 -Delay 58`.
  The cannon and the silo take 10 power each (four American power plants for both). `-GenTrace`
  adds `[missile f..]` (leaves along / goes off / dies), `[nuke f..]` (the silo's doors, lift-off,
  the blasts), `[cannon f..]` (its stages, every eighth damage pulse, the beam being sent on) and
  `[power f..] ... attacks`; `-Extra "-dpcvars=gen.MissileTrace=1"` logs every missile's state,
  position, velocity, nose and thrust each half second. As tried for M4h:
  `gen_test.ps1 -NoAI -Faction FactionGLA -Cam "1005,930,0,300" -Objects "GLAScudStorm@1005:900" -Enemies "ChinaTankBattleMaster,ChinaTankBattleMaster,ChinaPowerPlant" -EnemyAt "1005,1500" -Enemy FactionChina -TestPowers "60,power,SuperweaponScudStorm,1005,1500" -PauseFrame 232 -Trace -Extra "-GenNoTestUnits -GenPowersReady"`
  (the launch; `-Cam "1005,1500,0,330" -PauseFrame 372` for the impacts);
  `-Faction FactionChina -Objects "ChinaNuclearMissileLauncher@1005:900,ChinaPowerPlant@860:820" -TestPowers "60,power,SuperweaponNeutronMissile,1005,1500"`
  (lift-off at f96, the blast at f245 over the target);
  `-Faction FactionAmerica -Objects "AmericaParticleCannonUplink@1005:900,AmericaPowerPlant@840:820,AmericaPowerPlant@840:960" -TestPowers "60,power,SuperweaponParticleUplinkCannon,1005,1500;200,power,SuperweaponParticleUplinkCannon,1140,1500"`
  (f130 at the building, f330 at the target: the second step sends the beam on). Missile units:
  `-Objects "AmericaVehicleTomahawk,GLAVehicleScudLauncher" -ObjectsAt "1005,900" -Enemies "ChinaTankBattleMaster,ChinaTankBattleMaster" -EnemyAt "1005,1180" -Enemy FactionChina -TestAttack`.
- Mines: `-GenTrace` adds `[mine f..]` lines (a field laid and how many, every mine going off and how
  many it has left, a mine found under a new building). A general's power needs its rank first:
  `skill,5000` before `science,...`. As tried for M4i:
  `gen_test.ps1 -NoAI -Faction FactionChina -Speed 4 -Objects "neutral:GermanPub01@1005:1230" -Enemies "AmericaInfantryRanger,AmericaInfantryRanger,AmericaTankCrusader" -EnemyAt "1005,960" -Enemy FactionAmerica -TestPowers "30,skill,5000;35,science,SCIENCE_ClusterMines;60,power,SuperweaponClusterMines,1005,1100;900,enemyenter,AmericaInfantryRanger,GermanPub01,1005,1230;1300,status,AmericaInfantryRanger;1301,status,ChinaClusterMine" -Trace -Extra "-GenNoTestUnits -GenPowersReady"`
  (the Rangers walk into the field); `-Faction FactionChina -Speed 8 -Cam "1005,600,0,260" -TestPowers "30,money,5000;40,upgrade,Upgrade_ChinaMines;1210,sell,ChinaCommandCenter" -Delay 9`
  (the ring is there from f640; a shot before f1210 shows its lights - they blink, so a single frame
  has about half of them lit); the enemy's view and building on one:
  `-Faction FactionAmerica -Enemy FactionChina -Enemies "ChinaStandardMine" -EnemyAt "1005,800" -Cam "1005,800,0,200" -TestPowers "30,money,5000;40,build,AmericaPowerPlant,1005,800"`.
  A model that shows nothing may be a model of bones alone: `Tools/w3d.py <Model>` lists what it has.
- Add-ons, propaganda and drones: `-GenTrace` adds `... rides on ...` to the `[contain f..]` lines,
  `[propaganda f..]` (how many units a tower has, whenever that changes) and `[drone f..]` (tied to its
  master, starts / stops mending, has lost its master). The step `upgrade,Name` queues an object upgrade at
  the first of our objects that can take it, so three Overlords and three steps give one add-on each. As
  tried for M4j:
  `gen_test.ps1 -NoAI -Faction FactionChina -Enemy FactionAmerica -Speed 4 -Cam "1005,860,0,170" -Objects "ChinaTankOverlord@945:860,ChinaTankOverlord@1005:860,ChinaTankOverlord@1065:860,ChinaInfantryTankHunter@1005:800,ChinaInfantryTankHunter@1015:800,ChinaInfantryTankHunter@995:800" -Enemies "AmericaVehicleChinook,AmericaVehicleChinook" -EnemyAt "1005,1000" -TestPowers "30,money,9000;40,upgrade,Upgrade_ChinaOverlordGattlingCannon;45,upgrade,Upgrade_ChinaOverlordBattleBunker;50,upgrade,Upgrade_ChinaOverlordPropagandaTower;520,enter,ChinaInfantryTankHunter,ChinaTankOverlord,1005,860;760,evacuate,ChinaTankOverlord,1005,860;900,enter,ChinaInfantryTankHunter,ChinaTankOverlord,1005,860;1100,kill,ChinaTankOverlord" -Trace -PauseFrame 1260 -Extra "-GenNoTestUnits"`
  (the same with `ChinaVehicleHelix` and `Upgrade_ChinaHelix...`; a speaker tower needs a `ChinaPowerPlant`
  among the objects), and
  `-Faction FactionAmerica -Objects "AmericaVehicleHumvee@945:860,AmericaVehicleHumvee@1005:860,AmericaVehicleHumvee@1065:860" -TestPowers "30,money,9000;40,upgrade,Upgrade_AmericaBattleDrone;45,upgrade,Upgrade_AmericaScoutDrone;50,upgrade,Upgrade_AmericaHellfireDrone;300,damage,AmericaVehicleHumvee,50" -PauseFrame 520`
  (`damage` hurts the first object of the type only: the one with the battle drone). Something flying
  shows further up the screen than the ground under it: aim the camera some 1.3 times its height north.
  `ini_grep.py` wants a file pattern (`data\ini\object\*.ini`), not a folder - a folder finds nothing
  and says nothing; `ini_obj.py` prints the first 400 lines of an object.
- The gunship and the sneak attack: `-GenTrace` adds `[gunship f..]` (sent for, in its orbit, every
  howitzer shell, told where to fire, leaving, gone; and every three seconds where it is, how fast and on
  which locomotor); the step `steer,x,y` moves our gunships' reticle. As tried for M4i:
  `gen_test.ps1 -NoAI -Faction FactionAmerica -Enemy FactionChina -Speed 4 -Cam "950,1420,0,330" -Enemies "ChinaTankBattleMaster,ChinaTankBattleMaster,ChinaTankBattleMaster,ChinaPowerPlant,ChinaInfantryRedguard,ChinaInfantryRedguard" -EnemyAt "1005,1500" -TestPowers "30,skill,9000;35,science,SCIENCE_SpectreGunshipSolo;60,power,SuperweaponSpectreGunship,1005,1500;330,steer,1100,1560" -Trace -PauseFrame 452 -Delay 9 -Extra "-GenNoTestUnits -GenPowersReady"`
  (something flying 130 up shows about 170 further from the camera than the ground under it). The sneak
  attack is placed like a building: `click,Command_SneakAttackFromShortcut` and then `target,x,y` (the log
  says whether the tunnel may go there; `build,GLASneakAttackTunnelNetwork,x,y` asks the same of any spot -
  the middle of Tournament Desert is too steep, (1005, 1400) is fine):
  `-Faction FactionGLA -Speed 2 -Cam "1005,1400,0,170" -Objects "GLATunnelNetwork@1005:800,GLAInfantryRebel@985:860,GLAInfantryRebel@1005:860,GLAInfantryRebel@1025:860" -TestPowers "30,skill,9000;35,science,SCIENCE_SneakAttack;60,enter,GLAInfantryRebel,GLATunnelNetwork,1005,800;300,click,Command_SneakAttackFromShortcut;305,target,1005,1400;540,evacuate,GLASneakAttackTunnelNetwork,1005,1400" -PauseFrame 400`
  (f400: digging up; without the pause, the Rebels come out at the new tunnel). A regression run without
  `-Shot` never ends by itself: the harness stops it after fifteen minutes.
- Selecting and cursors: `-GenTestPowers` steps `pick,x,y[,select|hover]` (what the cursor would be on with
  that spot of the ground under it - the pick itself, by way of the screen; `select` takes it as a click
  would, `hover` leaves the cursor there: the thing shows its health bar and name, the log names the
  cursor and its picture is drawn into the screenshot, since a test run has no mouse), `pickscan,Type`
  (picks along two lines through the nearest such object, a quarter of its radius a step: `#` on it, `o`
  on something else, `.` on nothing; and what a pick costs) and `selkey,what[,n]` (`create` / `select` /
  `add` / `view` with a group's number, `match`, `matchmap`, `all`, `aircraft`, `double`). `-Window` runs
  the test in a real window instead of offscreen: only there are the platform's cursors made (the log's
  "Cursors: X.ani ... made into the platform's" lines; offscreen Unreal has a null application and says
  "no platform cursor here"). An offscreen run's view is about 888 x 500 whatever `-ResX` says - the
  pixel positions in the log are in that. `gen.SelectFlashHold 30` holds the white flash of a selection
  for a picture; `gen.PickSlack`, `gen.PickBoxes`, `gen.Cursors`, `gen.CursorScale` are the switches.
  The control bar (M6f): `Tools/wnd_dump.py Window\ControlBar.wnd` prints a layout's windows
  (rectangles in 800 x 600, status, first images, texts); `-Trace` adds `[bar f..] right panel ...`
  (queue or portrait, which image, how many upgrade cameos). The bar's pictures come from the
  mapped images, so a missing one shows as an empty place, not an error.
  Force attack and waypoints (M6d): `forceclick,x,y` / `waypointclick,x,y` / `leftclick,x,y` click
  there with Ctrl / Alt (Alt then stays held, so the routes are drawn) / nothing held, for whatever is
  selected; the log says the action, how many units and the cursor. Example:
  `-NoAI -Objects "AmericaTankCrusader@1005:760,AmericaPowerPlant@1180:760" -TestPowers "30,pick,1005,760,select;40,forceclick,1180,760;300,forceclick,1005,1150" -Trace`
  (a second unit off the screen is not taken by `selkey,match`: use `matchmap`).
  As tried for M6c:
  `gen_test.ps1 -NoAI -Faction FactionAmerica -Enemy FactionChina -Cam "1040,740,0,170" -Objects "AmericaTankCrusader@1080:720:45,AmericaInfantryRanger@930:720" -Enemies "ChinaTankBattleMaster" -EnemyAt "1150,790" -TestPowers "60,pick,1080,720,select;65,pick,1150,790,hover;66,pickscan,AmericaTankCrusader" -Trace -Delay 5 -Extra "-GenNoTestUnits"`
  (an object placed with `Template*N` stands round the spot, not on it: a pick at the spot finds nothing).
  `Tools/ani_dump.py [out_dir]` lists what the cursor files are made of (and writes a sheet of each one's
  frames); `Tools/w3d_chunks.py <Model>` prints a model file's raw chunks. A new input of the model
  materials (ObjectTint) is made by `gen_setup.ps1 -Only w3d`.
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
  `destroy,Type,x,y` (anybody's object of that type nearest the spot) / `move,x,y[,Kind or Type]` (everything of
  ours that moves, or only that kind - `INFANTRY`, `CAN_ATTACK` leaves the dozer out - as one group order) /
  `formation[,Kind or Type]` (Ctrl+F for them) /
  `grant,Upgrade` (ours at once) / `kill,Type[,Player]` (all of that type - by the name asked for or the variation's own, e.g.
  `GLAVehicleTechnicalChassisTwo` - die as if shot: pilots, hulks) /
  `vet,Type,Level,Player` (another player's) / `combatdrop,x,y[,Type]` (our Chinooks drop there, into the
  object of that type nearest the spot) / `enemyenter,Unit,Type,x,y` (the other players' units go in) /
  `fog,Type` or `fog,*` (logs what the local player sees of those objects) / `radar,x,y` (a left click on
  the radar at that world spot) / `event` (Space: jump to the last radar event) /
  `enter,Unit,Type[,x,y]` (our units of that type, or `*`, get into the container of that type nearest the
  camera target or x,y) / `evacuate,Type[,x,y]` / `sound,Event[,x,y]` / `voice,Key,Type` (that unit's
  own sound for an INI key such as VoiceMove) / `eva,Name` / `music,Track`; `-GenTrace`
  adds `[upgrade f..]`, `[science f..]`, `[power f..]`, `[rally f..]`, `[repair f..]`, `[contain f..]`, `[vet f..]`, `[flare f..]`, `[pilot f..]`, `[drop f..]`, `[ocl f..]`, `[fire f..] ... catches fire`, `[topple f..]`, `[collapse f..]`, `[victory f..]`, `[fog f..]`, `[radar f..]`, `[stealth f..]`, `[audio f..]` lines and rank / points / weapons to the status lines),
  `-GenDifficulty=Easy|Normal|Hard` and `-GenNoAI` for the computer player (`-GenTrace` adds
  `[ai f..]` lines: what it builds, the teams it starts, a status line per second). `-GenObject=` also places finished buildings, e.g. a supply center next to a pile; `neutral:Template` makes it nobody's (a civilian building to occupy).
  Player input is ignored while `-GenShot` is active, so the real mouse can't scroll the test camera.
  `-GenNoShroud` switches the fog of war off for the local player; `gen_test.ps1` passes it unless `-Fog`
  is given, so test runs see the whole map as before.
- The fog of war material M_Shroud is made by `Tools/ue_setup.py`; `GEN_SETUP_ONLY=shroud` in the
  environment rebuilds just that one, `GEN_SETUP_ONLY=w3d` the model and particle materials. (The
  lighting parameter collection is left alone once it exists: making its parameter list anew gives the
  parameters new ids and every material not rebuilt in the same run stops compiling.)
- The menu in test runs: a run with test switches skips the menu (as before). `gen_test.ps1 -Menu
  "sec,click,Id;..."` (`-GenTestMenu=`) shows it and clicks interface elements by id at those seconds
  since the program started - ids are listed at the top of `Source/Generals1/Game/GenShell.cpp`
  (`Slot1.State=Hard Army`, `Slot0.Side=USA`, `SelectMap`, `Map=Tournament Island`, `MapAccept`, `Start2`,
  `Cash=$20000`, `TestUnits`, `Play`, `Menu.Open`, `Menu.Exit`, `Score.Continue` ...). Such a run starts from the
  default setup and doesn't touch the saved one. `-GenTestPowers` frames count from the start of the
  game, so a game started from the menu can be played on by the usual steps. When the score screen
  comes up the log has a `[score]` line per player. After "continue" or "restart" the level is loaded
  anew: the screenshot delay then counts from that load. `-GenNoMenu` starts a game at once without
  any other switch.
- Sound in test runs: `gen_test.ps1` always runs Unreal's offline audio device (`-DeterministicAudio`), so
  nothing is heard, and without music unless `-Music`. `-Record file.wav` (`-GenRecordAudio=`) writes what
  the mixer put out; such a run uses a fixed 30 Hz step, because that device renders 1/30 s of sound per
  tick whatever the frame rate (without it sounds run out several times too fast, which is harmless
  but makes a recording useless). `Tools/audio_check.py` reads the recording: loudness and clipping,
  left / right over time, and where a named sound sits in it, how well it matches and at what level.
  `-AudioSelfTest` decodes every file the events name; `-DumpWav "a,b"` writes sounds out decoded for
  `audio_check.py --dumps`. `-GenNoLimiter` takes the limiter off the mix (for checking levels).
  `[audio f..]` lines: every sound started (file, place, pan, volume, pitch) and every 5 s the counts of
  what was not played and why, and the loops objects are asking for.
- Logic code mirrors the original per-frame formulas; INI rates are converted with the configured logic rate.
- Determinism and network tests (MP1):
  - `Tools\replay_test.ps1 -Name tf7 -Map "Twilight Flame" -Bots 7 -Seed 3 -Delay 300 -ReplayDelay 330` records a
    game and plays it back; the log says "every frame the same" or the first frame that differs (and writes
    `Saved\Logs\StateDump_replay_f<N>.txt`; run the recording again with `-RecordExtra "-GenStateDump=<N> -GenDumpTag=orig"`
    and compare the two dumps line by line). `-SkipRecord -ReplayExtra "-GenReplayViewer=4"` plays it back as
    another player; `-ReplaySpeed 2` with the simulation's own thread. Long runs at 1280x720 (the default).
  - `Tools\net_test.ps1 -Name n4 -Players 2 -Map "Twilight Flame" -AI "Hard" -Teams "1,1" -Sides "FactionAmerica,FactionChina" -Delay 240 -Kill "2@30" -Powers "...;;..."`
    starts a server program (`-GenNoUPnP`: tests leave the router alone) and N games on this computer; the
    first sets the lobby up from `-GenMap` / `-GenNetAI` and starts; `-Kill "2@30"` kills game 2 thirty seconds
    after the server's clock starts and starts it again with `-GenRejoin`; at the end every game's checksums
    are compared frame by frame. `-NoStart` leaves them in the lobby (for screenshots). Test step
    `allymove,x,y`: what of an absent ally's the local player may command goes there. Logs: `GenServer.log`,
    `GenClient<N>.log` (and `GenClient<N>-killed.log`).
  - A game plays the network by hand with `-GenHost`, `-GenJoin=host[:port]`, `-GenRejoin`, `-GenName=`.
