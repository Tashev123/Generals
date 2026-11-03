#include "Map/GenRandomMapInternal.h"

// The environments of the random maps. Every name here is from the game's own data, as the official multiplayer maps of
// that kind of place use it: terrain classes by the share of a map they cover and the slopes they lie on, objects by how
// often they are placed, the lights and the map.ini of a map that looks like that (measured with Tools/gen_map.py).

namespace GenRandomMapDetail
{
	static TArray<FEnvironmentDef> MakeEnvironments()
	{
		TArray<FEnvironmentDef> Out;
		{
			// Sand and palms: El Scorcho, Desert Fury, Tournament Desert.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("desert");
			E.Label = TEXT("Desert");
			E.MapIniDonor = TEXT("Tournament Desert");
			E.LightDonors[0] = TEXT("Tournament Desert");
			E.LightDonors[1] = TEXT("El Scorcho");
			E.LightDonors[2] = TEXT("El Scorcho");
			E.LightDonors[3] = TEXT("Wasteland Warlords");
			E.Hilliness = 1.f;
			E.Forest = 0.32f;
			E.Ground = { TEXT("SandLargeType4"), TEXT("SandMediumType11"), TEXT("SandMediumType9") };
			E.Patch = TEXT("SandLargeType4Grassy");
			E.Dirt = TEXT("DirtMediumType12");
			E.Rock = TEXT("RocksType5");
			E.Cliff = TEXT("CliffLargeType11");
			E.Shore = TEXT("SandType3wet");
			E.Bed = TEXT("SandType3wavy");
			E.Base = TEXT("DirtMediumType13");
			E.Town = TEXT("SandMediumType5c");
			E.Trees = { { TEXT("TreePalm1short"), 5 }, { TEXT("TreePalm2short"), 5 }, { TEXT("TreePalm2"), 3 }, { TEXT("TreePalm03"), 3 }, { TEXT("TreePalm1"), 2 }, { TEXT("TreeDogwood2"), 1 } };
			E.Bushes = { { TEXT("DesertShrub06"), 4 }, { TEXT("Bush03"), 3 }, { TEXT("DesertShrub09"), 2 }, { TEXT("Bush01"), 2 }, { TEXT("Bush04"), 2 } };
			E.Rocks = { { TEXT("Rocks1Brown"), 3 }, { TEXT("Rocks2Brown"), 3 }, { TEXT("Rocks3Brown"), 3 }, { TEXT("RockClusterSmall01"), 1 }, { TEXT("Boulders"), 1 } };
			E.BigRocks = { { TEXT("RockClusterMedium01"), 2 }, { TEXT("RockClusterLarge01"), 1 } };
			E.Houses = { { TEXT("MogadishuHouse17"), 3 }, { TEXT("MogadishuHouse10"), 2 }, { TEXT("MogadishuHouse16"), 2 }, { TEXT("MogadishuHouse13"), 2 }, { TEXT("MogadishuHouse05"), 2 },
				{ TEXT("MogadishuHouse06"), 2 }, { TEXT("MogadishuHouse03"), 2 }, { TEXT("MogadishuHouse04"), 2 }, { TEXT("MogadishuHouse11"), 2 }, { TEXT("MogadishuHouse12"), 2 },
				{ TEXT("MogadishuHouse02"), 2 }, { TEXT("MogadishuHouse01"), 1 }, { TEXT("MogadishuHouse07"), 1 } };
			E.Shops = { { TEXT("MogadishuShop01"), 2 }, { TEXT("MogadishuShop02"), 2 }, { TEXT("MogadishuGarage"), 1 }, { TEXT("MogadishuDome"), 1 }, { TEXT("MogadishuArchedHouse"), 1 } };
			E.Large = { { TEXT("MogadishuHighrise01"), 1 }, { TEXT("MogadishuHighrise02"), 1 }, { TEXT("StanTownHall01"), 1 } };
			E.Towers = { { TEXT("GuardTower"), 2 }, { TEXT("WoodTower"), 2 }, { TEXT("StanWatchTowerTall"), 1 } };
			E.FarmBuildings = { { TEXT("Tent03"), 2 }, { TEXT("Shack"), 1 }, { TEXT("MogadishuHouse01"), 1 } };
			E.StreetProps = { { TEXT("MarketVender01"), 2 }, { TEXT("MarketVender02"), 2 }, { TEXT("MarketVender03"), 2 }, { TEXT("AsianFruitStand02"), 2 }, { TEXT("AsianFruitStand03"), 2 }, { TEXT("FoodCart02"), 1 }, { TEXT("CanvasSunShader"), 1 } };
			E.YardProps = { { TEXT("Barrel"), 3 }, { TEXT("ClothesLine"), 3 }, { TEXT("Tire02"), 2 }, { TEXT("CarScrap"), 2 }, { TEXT("MogadishuWell"), 2 }, { TEXT("Tent"), 2 }, { TEXT("Cart"), 1 } };
			E.FarmProps = { { TEXT("Tent"), 3 }, { TEXT("Barrel"), 2 }, { TEXT("Cart"), 2 }, { TEXT("FirePit"), 1 }, { TEXT("WaterTower"), 1 } };
			E.Landmarks = { TEXT("EgyptianPyramidSmall"), TEXT("EgyptianSphinx") };
			E.RoadMain = TEXT("DirtRoad3");
			E.RoadTown = TEXT("DirtRoad");
			E.RoadAvenue = TEXT("TwoLaneOld");
			E.Bridge = TEXT("KandaharBridge");
			E.AmbientDay = { TEXT("Amb_DesertNatureDayBirds"), TEXT("Amb_DesertNatureDayInsects2"), TEXT("Amb_DesertNatureWindThinLoop"), TEXT("Amb_DesertNatureWindThickLoop"), TEXT("Amb_DesertNatureDayBirds2") };
			E.AmbientNight = { TEXT("Amb_DesertNatureNightInsectLoop"), TEXT("Amb_DesertNatureWindNight"), TEXT("Amb_DesertVillageNightInsect") };
			E.AmbientWater = { TEXT("Amb_WaterLakeLapping"), TEXT("Amb_WaterLakeLappingLoop") };
		}
		{
			// Rock, dust and old towns: Cairo Commandos, Twilight Flame, Wasteland Warlords.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("mideast");
			E.Label = TEXT("Middle East");
			E.MapIniDonor = TEXT("Golden Oasis");
			E.LightDonors[0] = TEXT("Dust Devil");
			E.LightDonors[1] = TEXT("Cairo Commandos");
			E.LightDonors[2] = TEXT("Twilight Flame");
			E.LightDonors[3] = TEXT("Wasteland Warlords");
			E.Hilliness = 1.15f;
			E.Forest = 0.3f;
			E.Ground = { TEXT("SandMediumType6"), TEXT("DirtMediumType10"), TEXT("SandMediumType10") };
			E.Patch = TEXT("SandMediumType7Grassy");
			E.Dirt = TEXT("DirtMediumType11");
			E.Rock = TEXT("RocksMediumType1");
			E.Cliff = TEXT("CliffLargeType10");
			E.Shore = TEXT("SandType4");
			E.Bed = TEXT("SandMediumType5b");
			E.Base = TEXT("SandMediumType8");
			E.Town = TEXT("SandMediumType3");
			E.Trees = { { TEXT("TreePalm1"), 4 }, { TEXT("TreePalm1short"), 4 }, { TEXT("TreeCypress01"), 1 }, { TEXT("TreeBirch01"), 1 }, { TEXT("TreeFir05"), 1 }, { TEXT("TreeDogwood2"), 1 } };
			E.Bushes = { { TEXT("Bush11"), 3 }, { TEXT("DesertShrub09"), 3 }, { TEXT("Bush06"), 2 }, { TEXT("Bush04"), 1 } };
			E.Rocks = { { TEXT("Rocks1"), 3 }, { TEXT("Rocks2"), 3 }, { TEXT("Rocks3"), 3 }, { TEXT("Boulders"), 2 }, { TEXT("RockClusterSmall01"), 1 } };
			E.BigRocks = { { TEXT("RockClusterMedium01"), 2 }, { TEXT("RockClusterLarge01"), 1 } };
			E.Houses = { { TEXT("StanApartment02"), 3 }, { TEXT("StanApartment01"), 2 }, { TEXT("StanSmallRetail03"), 2 }, { TEXT("StanSmallRetail01"), 2 }, { TEXT("StanSmallRetail02"), 1 }, { TEXT("MogadishuHouse05"), 2 }, { TEXT("MogadishuHouse06"), 2 }, { TEXT("MogadishuHouse01"), 2 } };
			E.Shops = { { TEXT("StanRestaurant01"), 2 }, { TEXT("StanRestaurant02"), 2 }, { TEXT("StanConvenienceStore01"), 2 }, { TEXT("MogadishuShop01"), 2 }, { TEXT("MogadishuShop02"), 2 }, { TEXT("MogadishuDome"), 1 } };
			E.Large = { { TEXT("StanHotel01"), 2 }, { TEXT("MogadishuHighrise02"), 2 }, { TEXT("MogadishuHighrise03"), 2 }, { TEXT("StanTownHall01"), 1 } };
			E.Towers = { { TEXT("MogadishuTower05"), 3 }, { TEXT("MogadishuTower04"), 2 }, { TEXT("StanWatchTowerTall"), 1 }, { TEXT("GuardTower"), 1 } };
			E.FarmBuildings = { { TEXT("Tent03"), 2 }, { TEXT("Shack"), 2 } };
			E.StreetProps = { { TEXT("MarketVender01"), 2 }, { TEXT("MarketVender02"), 2 }, { TEXT("MarketVender03"), 2 }, { TEXT("JunkDealer"), 1 }, { TEXT("JunkDealerStand"), 1 }, { TEXT("FoodCart02"), 1 } };
			E.YardProps = { { TEXT("Barrel"), 3 }, { TEXT("BrokenWallPiece02"), 2 }, { TEXT("BrokenWallPiece04"), 2 }, { TEXT("BrokenWallPiece05"), 2 }, { TEXT("Tire02"), 2 }, { TEXT("Pump"), 1 }, { TEXT("MogadishuWell"), 1 }, { TEXT("WaterTower"), 1 } };
			E.FarmProps = { { TEXT("Tent"), 3 }, { TEXT("Barrel"), 2 }, { TEXT("Pump"), 1 }, { TEXT("Cart"), 1 } };
			E.Landmarks = { TEXT("EgyptianSphinx"), TEXT("EgyptianPyramidSmall") };
			E.RoadMain = TEXT("DirtRoadTracks2");
			E.RoadTown = TEXT("DirtRoad");
			E.RoadAvenue = TEXT("TwoLaneOld");
			E.Bridge = TEXT("KandaharBridge");
			E.AmbientDay = { TEXT("Amb_DesertNatureDayBirds"), TEXT("Amb_DesertNatureWindThinLoop"), TEXT("Amb_DesertNatureWindThickLoop"), TEXT("Amb_DesertVillageDayBirds"), TEXT("Amb_DesertNatureWindTreesLoop") };
			E.AmbientNight = { TEXT("Amb_DesertNatureWindNight"), TEXT("Amb_DesertVillageNightInsect"), TEXT("Amb_DesertVillageNightDog") };
			E.AmbientWater = { TEXT("Amb_WaterLakeLapping"), TEXT("Amb_WaterLakeLappingLoop") };
		}
		{
			// Green fields, oak woods, German towns: Heartland Shield, Tournament Plains, Leipzig Lowlands.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("europe");
			E.Label = TEXT("Europe");
			E.MapIniDonor = TEXT("Tournament Lake");
			E.LightDonors[0] = TEXT("Heartland Shield");
			E.LightDonors[1] = TEXT("Tournament Plains");
			E.LightDonors[2] = TEXT("Final Crusade");
			E.LightDonors[3] = TEXT("Rocky Rampage");
			E.Hilliness = 0.9f;
			E.Forest = 1.f;
			E.Ground = { TEXT("GrassMediumType7"), TEXT("GrassMediumType3"), TEXT("GrassMediumType13") };
			E.Patch = TEXT("GrassMediumType30");
			E.Dirt = TEXT("DirtMediumType5");
			E.Rock = TEXT("RocksType3");
			E.Cliff = TEXT("CliffMediumType5");
			E.Shore = TEXT("DirtType1");
			E.Bed = TEXT("DirtMediumType3");
			E.Base = TEXT("GrassMediumType5");
			E.Town = TEXT("CobblestoneType1");
			E.Crops = TEXT("CropsType1");
			E.Trees = { { TEXT("TreeOakFall4"), 3 }, { TEXT("TreeOakFall6"), 3 }, { TEXT("TreeOakFall1"), 3 }, { TEXT("TreeOakFall2"), 2 }, { TEXT("TreeOakFall3"), 2 }, { TEXT("TreeDogwood3"), 2 }, { TEXT("TreeDogwood5"), 2 },
				{ TEXT("TreeMaple03"), 1 }, { TEXT("TreeOak1"), 2 }, { TEXT("ArborVitae01"), 1 }, { TEXT("TreeBirch01"), 1 } };
			E.Bushes = { { TEXT("Bush01"), 3 }, { TEXT("Bush02"), 2 }, { TEXT("Bush03"), 2 }, { TEXT("Bush08"), 2 }, { TEXT("Bush07"), 1 } };
			E.Rocks = { { TEXT("Rocks1"), 3 }, { TEXT("Rocks2"), 3 }, { TEXT("Rocks3"), 3 }, { TEXT("RockClusterSmall01"), 1 } };
			E.BigRocks = { { TEXT("RockClusterMedium01"), 2 }, { TEXT("RockClusterLarge01"), 1 } };
			E.Houses = { { TEXT("GermanTownHouse1"), 4 }, { TEXT("GermanTownHouse2"), 4 }, { TEXT("GermanPub01"), 2 }, { TEXT("GermanPub02"), 2 }, { TEXT("GermanCotage01"), 2 }, { TEXT("Chalet"), 1 } };
			E.Shops = { { TEXT("GermanPub02"), 2 }, { TEXT("GermanPub01"), 2 }, { TEXT("StanSchoolhouse02"), 1 }, { TEXT("ConvienceStore"), 1 } };
			E.Large = { { TEXT("GermanTownHall"), 2 }, { TEXT("EuroHighrise"), 1 }, { TEXT("SovietGovernmentBuildingSmall"), 1 }, { TEXT("EuropeanParkingGarage"), 1 }, { TEXT("CivilianHighrise01"), 1 } };
			E.Towers = { { TEXT("WoodTower"), 2 }, { TEXT("GuardTower"), 1 } };
			E.FarmBuildings = { { TEXT("FarmHouse"), 3 }, { TEXT("BarnShed"), 3 }, { TEXT("BarnCoop"), 2 }, { TEXT("GermanWindMill01"), 2 }, { TEXT("Shack"), 1 } };
			E.StreetProps = { { TEXT("ParkBench"), 3 }, { TEXT("PlanterRound"), 2 }, { TEXT("BikeRack"), 2 }, { TEXT("PhoneBooth"), 1 }, { TEXT("TrashCanA"), 2 }, { TEXT("StreetSign"), 1 } };
			E.YardProps = { { TEXT("Cart"), 2 }, { TEXT("StatueAngel"), 1 }, { TEXT("Fountain1"), 1 }, { TEXT("Tombstones01"), 1 }, { TEXT("WineVat"), 1 }, { TEXT("BigWoodenBarrels"), 1 } };
			E.FarmProps = { { TEXT("Haystack"), 4 }, { TEXT("CropBundle"), 4 }, { TEXT("ScareCrow"), 2 }, { TEXT("Cart"), 2 }, { TEXT("FeedingTroughs"), 1 }, { TEXT("TractorCombine"), 1 } };
			E.Cars = { { TEXT("CarEuroVan1"), 2 }, { TEXT("CarSwatchEuro"), 2 }, { TEXT("CarBug1"), 2 }, { TEXT("CarBug2"), 2 }, { TEXT("CarBug3"), 1 } };
			E.Landmarks = { TEXT("StatueAngel"), TEXT("Fountain1") };
			E.StreetLamp = TEXT("StreetLamp03");
			E.RoadMain = TEXT("TwoLaneOld2");
			E.RoadTown = TEXT("Cobblestone");
			E.RoadAvenue = TEXT("TwoLaneOld2");
			E.Bridge = TEXT("EuropeanBridgeWide");
			E.AmbientDay = { TEXT("Amb_TemperateForestBirds"), TEXT("Amb_TemperateMeadowBirds"), TEXT("Amb_TemperateForestTreesLoop"), TEXT("Amb_TemperateMeadowAmbientLoop"), TEXT("Amb_TemperateMeadowBirdsLoop") };
			E.AmbientNight = { TEXT("Amb_TemperateNightOwl"), TEXT("Amb_TemperateForestTreesLoop"), TEXT("Amb_DesertNatureNightInsectLoop") };
			E.AmbientWater = { TEXT("Amb_WaterLakeLapping"), TEXT("Amb_WaterRiverLoop") };
		}
		{
			// Snow: Bitter Winter, Whiteout, Lone Eagle, Tournament Tundra.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("winter");
			E.Label = TEXT("Alpine Winter");
			E.MapIniDonor = TEXT("Bitter Winter");
			E.LightDonors[0] = TEXT("Bitter Winter");
			E.LightDonors[1] = TEXT("Bitter Winter");
			E.LightDonors[2] = TEXT("Lone Eagle");
			E.LightDonors[3] = TEXT("Tournament Tundra");
			E.bSnowy = true;
			E.Hilliness = 1.25f;
			E.Forest = 0.85f;
			E.Ground = { TEXT("SnowLargeType1"), TEXT("SnowType6"), TEXT("SnowType5") };
			E.Patch = TEXT("SnowType7b");
			E.Dirt = TEXT("DirtSnowType1");
			E.Rock = TEXT("RocksMediumType1Snow");
			E.Cliff = TEXT("CliffMediumType6snow");
			E.Shore = TEXT("SnowType1");
			E.Bed = TEXT("SnowType3");
			E.Base = TEXT("DirtMediumType15Snow");
			E.Town = TEXT("ConcreteMediumType1Snow");
			E.Trees = { { TEXT("TreeSpruceSnow"), 4 }, { TEXT("TreePine5snow2"), 4 }, { TEXT("TreePine3snow"), 3 }, { TEXT("TreePineSnow"), 3 }, { TEXT("TreeOak02Snow"), 2 }, { TEXT("TreeSpruce05Snow"), 1 }, { TEXT("TreeSpruce04Snow"), 1 }, { TEXT("TreeFir09Snow"), 1 } };
			E.Bushes = { { TEXT("Bush14Snow"), 3 }, { TEXT("Bush13Snow"), 2 }, { TEXT("Bush12Snow"), 2 } };
			E.Rocks = { { TEXT("RockClusterSmallSnow01"), 3 }, { TEXT("Rocks8"), 2 }, { TEXT("Rocks9"), 2 } };
			E.BigRocks = { { TEXT("RockClusterMediumSnow01"), 2 }, { TEXT("RockClusterLargeSnow01"), 1 } };
			E.Houses = { { TEXT("GermanTownHouse1"), 4 }, { TEXT("GermanTownHouse2"), 4 }, { TEXT("GermanPub01"), 2 }, { TEXT("GermanPub02"), 2 }, { TEXT("Chalet"), 3 }, { TEXT("GermanCotage01"), 1 } };
			E.Shops = { { TEXT("GermanPub02"), 2 }, { TEXT("GermanPub01"), 2 }, { TEXT("StanSovietOffice01"), 1 } };
			E.Large = { { TEXT("GermanTownHall"), 3 }, { TEXT("SovietBuilding"), 1 }, { TEXT("StanSovietOffice02"), 1 }, { TEXT("EuroHighrise"), 1 } };
			E.Towers = { { TEXT("WoodTower"), 3 }, { TEXT("GuardTower"), 1 } };
			E.FarmBuildings = { { TEXT("BarnShed"), 2 }, { TEXT("BarnCoop"), 2 }, { TEXT("Chalet"), 2 }, { TEXT("Shack"), 1 } };
			E.StreetProps = { { TEXT("BusStop"), 2 }, { TEXT("PhoneBoothYellow"), 1 }, { TEXT("TrashCanA"), 2 }, { TEXT("BikeRack"), 2 }, { TEXT("StreetSign"), 1 } };
			E.YardProps = { { TEXT("Lumber"), 2 }, { TEXT("Tombs"), 1 }, { TEXT("Tombstones01"), 1 }, { TEXT("BigWoodenBarrels"), 1 }, { TEXT("OldStreetLamppost"), 1 } };
			E.FarmProps = { { TEXT("Lumber"), 2 }, { TEXT("BigWoodenBarrels"), 1 }, { TEXT("Cart"), 1 } };
			E.Cars = { { TEXT("CarSport01"), 1 }, { TEXT("TruckWork"), 1 } };
			E.Landmarks = { TEXT("Tombs") };
			E.StreetLamp = TEXT("StreetLamp03");
			E.RoadMain = TEXT("GravelRoadSnow");
			E.RoadTown = TEXT("TwoLaneOld2Snow");
			E.RoadAvenue = TEXT("TwoLaneOld2Snow");
			E.Bridge = TEXT("EuropeanBridgeWideSNOW");
			E.AmbientDay = { TEXT("Amb_WinterNatureWind"), TEXT("Amb_WinterNatureWindLoop"), TEXT("Amb_WinterNatureWindCold"), TEXT("Amb_WinterNatureCrows"), TEXT("Amb_WinterNatureCrows2") };
			E.AmbientNight = { TEXT("Amb_WinterNatureWindLoop"), TEXT("Amb_WinterNatureNightWolves"), TEXT("Amb_WinterNatureWindCold"), TEXT("Amb_TemperateNightOwl") };
			E.AmbientWater = { TEXT("Amb_WaterLakeLapping") };
		}
		{
			// Bamboo, blossom, lanterns, Asian towns: Manic Aggression, Iron Dragon, Victory Valley.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("china");
			E.Label = TEXT("China");
			E.MapIniDonor = TEXT("Victory Valley");
			E.LightDonors[0] = TEXT("Manic Aggression");
			E.LightDonors[1] = TEXT("Iron Dragon");
			E.LightDonors[2] = TEXT("Mountain Guns");
			E.LightDonors[3] = TEXT("Dark Night");
			E.Hilliness = 1.f;
			E.Forest = 0.75f;
			E.Ground = { TEXT("GrassMediumType10"), TEXT("GrassMediumType8"), TEXT("GrassMediumType30") };
			E.Patch = TEXT("GrassMediumType31");
			E.Dirt = TEXT("DirtType1");
			E.Rock = TEXT("RocksType3");
			E.Cliff = TEXT("CliffMediumType23");
			E.Shore = TEXT("SandMediumType3");
			E.Bed = TEXT("DirtMediumType5");
			E.Base = TEXT("DirtType2");
			E.Town = TEXT("CobblestoneType1");
			E.Crops = TEXT("CropsType1");
			E.Trees = { { TEXT("TreeCherryBlossom01"), 3 }, { TEXT("TreeCherryBlossom02"), 2 }, { TEXT("TreeBamboo01"), 4 }, { TEXT("TreeBamboo02"), 3 }, { TEXT("TreeBamboo03"), 2 }, { TEXT("TreeBirch03"), 2 }, { TEXT("TreeWillow01"), 2 }, { TEXT("ArborVitae01"), 2 } };
			E.Bushes = { { TEXT("Bush01"), 3 }, { TEXT("Bush02"), 2 }, { TEXT("Bush11"), 2 }, { TEXT("Hedge1"), 1 } };
			E.Rocks = { { TEXT("Rocks1"), 3 }, { TEXT("Rocks2"), 3 }, { TEXT("Rocks3"), 2 }, { TEXT("RocksG05"), 1 }, { TEXT("RockClusterSmall01"), 1 } };
			E.BigRocks = { { TEXT("RockClusterMedium01"), 2 }, { TEXT("RockClusterLarge01"), 1 } };
			E.Houses = { { TEXT("AsianApartmentUnit"), 3 }, { TEXT("AsianApartment01"), 2 }, { TEXT("AsiaApartment02"), 2 }, { TEXT("AsianRetailStore02"), 2 }, { TEXT("ChinaRetail02"), 2 }, { TEXT("ChinaRetail01"), 1 }, { TEXT("AsianArcade"), 1 } };
			E.Shops = { { TEXT("AsianRetailStore01"), 3 }, { TEXT("AsianRetailStore02"), 3 }, { TEXT("ChinaRetail03"), 2 }, { TEXT("AsianArcade"), 2 }, { TEXT("AsianElectricRetail"), 2 } };
			E.Large = { { TEXT("AsianHotel01"), 3 }, { TEXT("AsianOffice01"), 3 }, { TEXT("AsianOffice02"), 1 }, { TEXT("AsianBank"), 1 }, { TEXT("AsianNightClub"), 1 }, { TEXT("AsianApartmentSingle01"), 2 } };
			E.Towers = { { TEXT("WoodTower"), 2 }, { TEXT("GuardTower"), 1 } };
			E.FarmBuildings = { { TEXT("AsiaTownHall01"), 1 }, { TEXT("Shack"), 3 }, { TEXT("BarnCoop"), 1 } };
			E.StreetProps = { { TEXT("AsianFruitStand02"), 3 }, { TEXT("AsianFruitStand03"), 3 }, { TEXT("AsianTrashDumpster"), 2 }, { TEXT("GardenPathLantern01"), 2 }, { TEXT("DecorativeBanners"), 2 } };
			E.YardProps = { { TEXT("GardenPathLantern01"), 3 }, { TEXT("AncientSoldierStatue01"), 1 }, { TEXT("AncientGazebo"), 1 }, { TEXT("DecorativeBanners"), 2 }, { TEXT("GardenWell"), 1 } };
			E.FarmProps = { { TEXT("CropBundle"), 4 }, { TEXT("Haystack"), 2 }, { TEXT("ScareCrow"), 1 }, { TEXT("Cart"), 2 } };
			E.Cars = { { TEXT("CarAsian1"), 3 }, { TEXT("CarAsian2"), 3 }, { TEXT("CarBug2"), 1 } };
			E.Landmarks = { TEXT("TempleOfHeavenGateHouse"), TEXT("AncientGiantBell"), TEXT("AncientSoldierStatue02") };
			E.StreetLamp = TEXT("AsianStreetLight");
			E.RoadMain = TEXT("TwoLaneDark");
			E.RoadTown = TEXT("Cobblestone");
			E.RoadAvenue = TEXT("FourLaneDark");
			E.Bridge = TEXT("EuropeanBridgeTwoLane");
			E.AmbientDay = { TEXT("Amb_TemperateForestBirds"), TEXT("Amb_UrbanParkBirds"), TEXT("Amb_TemperateMeadowBirdsLoop"), TEXT("Amb_TemperateForestTreesLoop"), TEXT("Amb_MountainBirds") };
			E.AmbientNight = { TEXT("Amb_TemperateNightOwl"), TEXT("Amb_DesertNatureNightInsectLoop"), TEXT("Amb_TemperateForestTreesLoop") };
			E.AmbientWater = { TEXT("Amb_WaterLakeLapping"), TEXT("Amb_WaterRiverLoop") };
		}
		{
			// Small towns, gas stations, dogwood and pine: Homeland Alliance, North America, Red Rock.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("usa");
			E.Label = TEXT("North America");
			E.MapIniDonor = TEXT("Tournament Lake");
			E.LightDonors[0] = TEXT("NorthAmerica");
			E.LightDonors[1] = TEXT("Red Rock");
			E.LightDonors[2] = TEXT("Bear Town Beatdown");
			E.LightDonors[3] = TEXT("Tournament City");
			E.Hilliness = 0.85f;
			E.Forest = 1.f;
			E.Ground = { TEXT("GrassMediumType31"), TEXT("GrassMediumType30"), TEXT("GrassMediumType11") };
			E.Patch = TEXT("GrassMediumType27olive");
			E.Dirt = TEXT("DirtType3");
			E.Rock = TEXT("RocksType6");
			E.Cliff = TEXT("CliffMediumType15b");
			E.Shore = TEXT("SandMediumType5");
			E.Bed = TEXT("DirtMediumType3");
			E.Base = TEXT("DirtMediumType5");
			E.Town = TEXT("ConcreteType4");
			E.Crops = TEXT("CropsType1");
			E.Trees = { { TEXT("TreeDogwood7"), 4 }, { TEXT("TreeDogwood3"), 3 }, { TEXT("TreePine2"), 3 }, { TEXT("TreeMaple1"), 2 }, { TEXT("TreeOak01"), 2 }, { TEXT("TreeSpruce2"), 2 }, { TEXT("TreeMaple04"), 2 }, { TEXT("TreeDogwood1"), 2 } };
			E.Bushes = { { TEXT("Bush11"), 3 }, { TEXT("Bush07"), 2 }, { TEXT("Bush09"), 1 }, { TEXT("Bush06"), 2 } };
			E.Rocks = { { TEXT("Rocks1"), 3 }, { TEXT("Rocks2"), 3 }, { TEXT("Rocks3"), 3 }, { TEXT("RocksG02"), 1 } };
			E.BigRocks = { { TEXT("RockClusterMedium01"), 2 }, { TEXT("RockClusterLarge01"), 1 } };
			E.Houses = { { TEXT("StanSmallRetail03"), 3 }, { TEXT("StanSmallRetail01"), 3 }, { TEXT("StanApartment02"), 3 }, { TEXT("MogadishuHouse01"), 2 }, { TEXT("GermanCotage01"), 1 }, { TEXT("FarmHouse"), 1 } };
			E.Shops = { { TEXT("ConvienceStore"), 3 }, { TEXT("StanConvenienceStore01"), 2 }, { TEXT("StanRestaurant01"), 2 }, { TEXT("StanRestaurant02"), 2 }, { TEXT("AmericanFastFood"), 2 }, { TEXT("GasStation"), 2 }, { TEXT("AmericanGasStation"), 1 } };
			E.Large = { { TEXT("StanHotel01"), 2 }, { TEXT("CivilianHighrise01"), 3 }, { TEXT("CivilianHighrise02"), 3 }, { TEXT("EuropeanParkingGarage"), 1 }, { TEXT("IndustrialBuilding01"), 1 }, { TEXT("IndustrialBuilding02"), 1 } };
			E.Towers = { { TEXT("WoodTower"), 2 }, { TEXT("WaterTower"), 1 } };
			E.FarmBuildings = { { TEXT("FarmHouse"), 3 }, { TEXT("BarnShed"), 3 }, { TEXT("BarnCoop"), 2 }, { TEXT("Shack"), 1 } };
			E.StreetProps = { { TEXT("MailBox"), 3 }, { TEXT("PhoneBooth"), 2 }, { TEXT("BusStop"), 2 }, { TEXT("StreetSign"), 2 }, { TEXT("ParkingMeter"), 1 }, { TEXT("FireHydrantRed"), 2 }, { TEXT("Dumpster"), 1 } };
			E.YardProps = { { TEXT("TrashDumpster"), 2 }, { TEXT("PlaygroundSet"), 1 }, { TEXT("VendingMachine"), 1 }, { TEXT("Billboard01"), 1 }, { TEXT("Billboard02"), 1 }, { TEXT("OldWoodenTelephonePoll"), 1 } };
			E.FarmProps = { { TEXT("Haystack"), 4 }, { TEXT("TractorCombine"), 1 }, { TEXT("Tractor"), 1 }, { TEXT("FeedingTroughs"), 2 }, { TEXT("ScareCrow"), 1 } };
			E.Cars = { { TEXT("CarCompact"), 3 }, { TEXT("CarCamaro"), 2 }, { TEXT("CarSUV01"), 2 }, { TEXT("CarLuxury01"), 1 }, { TEXT("CarSedan01"), 2 }, { TEXT("CarSport01"), 1 } };
			E.Landmarks = { TEXT("PlaygroundSet"), TEXT("Fountain1") };
			E.StreetLamp = TEXT("StreetLamp02");
			E.RoadMain = TEXT("TwoLaneDarkDotted");
			E.RoadTown = TEXT("TwoLaneDark");
			E.RoadAvenue = TEXT("FourLaneDark");
			E.Bridge = TEXT("ConcreteWide");
			E.AmbientDay = { TEXT("Amb_TemperateMeadowBirds"), TEXT("Amb_TemperateForestBirds"), TEXT("Amb_TemperateTownDayAmbientLoop"), TEXT("Amb_UrbanParkBirds"), TEXT("Amb_TemperateMeadowBirdsLoop") };
			E.AmbientNight = { TEXT("Amb_TemperateNightOwl"), TEXT("Amb_DesertNatureNightInsectLoop"), TEXT("Amb_DesertVillageNightDog") };
			E.AmbientWater = { TEXT("Amb_WaterLakeLapping"), TEXT("Amb_WaterStreamLoop") };
		}
		{
			// Palms, huts and beaches: Eastern Everglades, Tournament Island, Dark Mountain.
			FEnvironmentDef& E = Out.AddDefaulted_GetRef();
			E.Key = TEXT("tropics");
			E.Label = TEXT("Tropics");
			E.MapIniDonor = TEXT("Dark Mountain");
			E.LightDonors[0] = TEXT("Tournament Island");
			E.LightDonors[1] = TEXT("Flash Fire");
			E.LightDonors[2] = TEXT("Bombardment Beach");
			E.LightDonors[3] = TEXT("Eastern Everglades");
			E.Hilliness = 0.8f;
			E.Forest = 1.2f;
			E.Ground = { TEXT("GrassMediumType8"), TEXT("GrassMediumType10"), TEXT("GrassMediumType7") };
			E.Patch = TEXT("SandMediumType7GrassyLight");
			E.Dirt = TEXT("DirtType1");
			E.Rock = TEXT("RocksType5");
			E.Cliff = TEXT("CliffMediumType23");
			E.Shore = TEXT("SandLargeType3Light");
			E.Bed = TEXT("SandMediumType3");
			E.Base = TEXT("SandMediumType5grassy");
			E.Town = TEXT("DirtMediumType5");
			E.Trees = { { TEXT("TreePalm04"), 4 }, { TEXT("TreePalm03"), 4 }, { TEXT("TreePalm1short"), 4 }, { TEXT("TreePalm2short"), 2 }, { TEXT("TreePalm1"), 2 }, { TEXT("TreePalm2"), 2 }, { TEXT("TreeDogwood1"), 2 }, { TEXT("TreeOak01"), 1 } };
			E.Bushes = { { TEXT("VinesGreen"), 2 }, { TEXT("Bush11"), 3 }, { TEXT("Bush06"), 3 }, { TEXT("Bush05"), 1 } };
			E.Rocks = { { TEXT("Rocks6"), 3 }, { TEXT("Rocks5"), 2 }, { TEXT("Rocks7"), 2 }, { TEXT("RockClusterSmall01"), 1 } };
			E.BigRocks = { { TEXT("RockClusterMedium01"), 2 }, { TEXT("RockClusterLarge01"), 1 } };
			E.Houses = { { TEXT("GrassHut01"), 3 }, { TEXT("GrassHut02"), 3 }, { TEXT("GrassHut03"), 4 }, { TEXT("GrassHut04"), 3 }, { TEXT("RiverHouse01"), 2 }, { TEXT("RiverHouse02"), 1 }, { TEXT("Shack"), 1 } };
			E.Shops = { { TEXT("GrassHut04"), 2 }, { TEXT("RiverHouse02"), 2 }, { TEXT("QuonsetHut01"), 1 }, { TEXT("MogadishuShop01"), 1 } };
			E.Large = { { TEXT("MogadishuHighrise01"), 1 }, { TEXT("AsianHotel01"), 1 }, { TEXT("StanHotel01"), 1 } };
			E.Towers = { { TEXT("WoodTower"), 3 }, { TEXT("GuardTower"), 1 } };
			E.FarmBuildings = { { TEXT("GrassHut01"), 2 }, { TEXT("GrassHut03"), 2 }, { TEXT("Shack"), 1 } };
			E.StreetProps = { { TEXT("Torches"), 4 }, { TEXT("BigWoodenBarrels"), 2 }, { TEXT("Cart"), 1 } };
			E.YardProps = { { TEXT("Torches"), 3 }, { TEXT("FirePit"), 2 }, { TEXT("BigWoodenBarrels"), 2 }, { TEXT("WaterTower02"), 1 }, { TEXT("WoodBeams01"), 1 } };
			E.FarmProps = { { TEXT("Torches"), 2 }, { TEXT("FirePit"), 1 }, { TEXT("Cart"), 1 } };
			E.Landmarks = { TEXT("FirePit") };
			E.RoadMain = TEXT("DirtRoad5");
			E.RoadTown = TEXT("DirtPath1");
			E.RoadAvenue = TEXT("DirtRoad5");
			E.Bridge = TEXT("WoodenSectional");
			E.AmbientDay = { TEXT("Amb_JungleDayDenseAmbientLoop"), TEXT("Amb_JungleDaySparseAmbientLoop"), TEXT("Amb_JungleDayDenseAmbientBirds"), TEXT("Amb_TemperateForestTrees") };
			E.AmbientNight = { TEXT("Amb_JungleNightAmbientLoop"), TEXT("Amb_JungleNightAmbientLoop2"), TEXT("Amb_DesertNatureNightInsectLoop") };
			E.AmbientWater = { TEXT("Amb_WaterOceanWaves"), TEXT("Amb_WaterLakeLapping") };
		}
		return Out;
	}

	const TArray<FEnvironmentDef>& GetEnvironmentDefs()
	{
		static const TArray<FEnvironmentDef> Defs = MakeEnvironments();
		return Defs;
	}
}
