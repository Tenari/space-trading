#include "base/all.h"
#include "string_chunk.h"

#ifndef GAMESHARED_H
#define GAMESHARED_H

///// #define some game-tunable constants
#define GAME_CONSTANT_ONE (1)
#define GAME_CONSTANT_TWO (2)

typedef enum EntityFeature {
  FeatureWalksAround,
  FeatureCanFight,
  EntityFeature_Count
} EntityFeature;

typedef enum EntityType {
  EntityNull,
  EntityStarSystem,
  EntityCharacter,
  EntityType_Count,
} EntityType;
static const char* ENTITY_STRINGS[] = {
  "NULL",
  "StarSystem",
  "Character",
};

typedef enum ShipType {
  ShipSparrow,
  ShipDart,
  ShipHaulerPrimeZ1,
  ShipNX400,
  ShipType_Count,
} ShipType;
static const char* SHIP_TYPE_STRINGS[] = {
  "Sparrow",
  "Dart",
  "Hauler-Prime Z-1",
  "NX-400",
};
typedef struct ShipTemplate {
  ShipType type;
  u8 drive_efficiency;
  u8 life_support_efficiency;
  u16 vacuum_cargo_slots;
  u16 climate_cargo_slots;
  u16 passenger_berths;
  u16 passenger_amenities_flags;
  u16 smugglers_hold_cu_m;
  u32 base_cost;
} ShipTemplate;
global ShipTemplate SHIPS[] = {
  {
    .type = ShipSparrow, .drive_efficiency = 1, .life_support_efficiency = 2,
    .vacuum_cargo_slots = 5, .climate_cargo_slots = 0, .passenger_berths = 0,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 1, .base_cost = 100000,
  },
  {
    .type = ShipDart,    .drive_efficiency = 12, .life_support_efficiency = 4,
    .vacuum_cargo_slots = 9, .climate_cargo_slots = 1, .passenger_berths = 1,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 0, .base_cost = 150000,
  },
  {
    .type = ShipHaulerPrimeZ1, .drive_efficiency = 3, .life_support_efficiency = 3,
    .vacuum_cargo_slots = 25, .climate_cargo_slots = 5, .passenger_berths = 0,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 0, .base_cost = 250000,
  },
  {
    .type = ShipNX400, .drive_efficiency = 6, .life_support_efficiency = 8,
    .vacuum_cargo_slots = 5, .climate_cargo_slots = 5, .passenger_berths = 5,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 0, .base_cost = 350000,
  },
};

typedef struct PlayerShip {
  ShipType type;
  u8 drive_efficiency;
  u8 life_support_efficiency;
  u16 vacuum_cargo_slots;
  u16 climate_cargo_slots;
  u16 passenger_berths;
  u16 passenger_amenities_flags;
  u16 smugglers_hold_cu_m;
  u32 base_cost;
  u32 remaining_mortgage;
  f32 interest_rate;
  u32 cu_m_fuel; // we are just saying you can buy as much fuel as you want
  u32 cu_m_o2; // we are just saying you can buy as much o2 as you want
  u64 id;
} PlayerShip;

typedef enum CommodityType {
  CommodityNull,
  CommodityHydrogenFuel,
  CommodityOxygen,
  CommodityWater,
  CommodityWheat,
  CommodityRice,
  CommodityCorn,
  CommodityPotatoes,
  CommodityBeef,
  CommodityChicken,
  CommodityButter,
  CommodityOliveOil,
  CommodityLiquor,
  CommodityBeer,
  CommodityFertilizer,
  CommodityCleaningSupplies,
  CommodityAirFilters,
  CommodityIndustrialChemicals,
  CommodityClothes,
  CommodityRawTextiles,
  CommodityElectronics,
  CommodityConstructionParts,
  Commodity3dPrinterParts,
  CommodityHandTools,
  CommodityCutlery,
  CommodityRobots,
  CommodityPlastics,
  CommodityHandWeapons,
  CommodityBatteries,
  CommoditySolarPanels,
  CommodityMedicines,
  CommodityOpticalComponents,
  CommodityGlass,
  CommodityLithium,
  CommodityBerylium,
  CommodityMagnesium,
  CommodityAluminium,
  CommoditySilicon,
  CommodityScandium,
  CommodityTitanium,
  CommodityVanadium,
  CommodityChromium,
  CommodityManganese,
  CommodityIron,
  CommodityCobalt,
  CommodityNickel,
  CommodityCopper,
  CommodityZinc,
  CommoditySelenium,
  CommodityZirconium,
  CommodityMolybdenum,
  CommodityRuthenium,
  CommodityRhodium,
  CommodityPalladium,
  CommoditySilver,
  CommodityCadmium,
  CommodityTin,
  CommodityTungsten,
  CommodityRhenium,
  CommodityIridium,
  CommodityPlatinum,
  CommodityGold,
  CommodityMercury,
  CommodityLead,
  Commodity_Count,
} CommodityType;
static const char* COMMODITY_STRINGS[] = {
  "NULL",
  "Hydrogen Fuel",
  "Oxygen",
  "Water",
  "Wheat",
  "Rice",
  "Corn",
  "Potatoes",
  "Beef",
  "Chicken",
  "Butter",
  "Olive Oil",
  "Liquor",
  "Beer",
  "Fertilizer",
  "Cleaning Supplies",
  "Air Filters",
  "Industrial Chemicals",
  "Clothes",
  "Raw Textiles",
  "Electronics",
  "Construction Parts",
  "3d Printer Parts",
  "Hand Tools",
  "Cutlery",
  "Robots",
  "Plastics",
  "Hand Weapons",
  "Batteries",
  "Solar Panels",
  "Medicines",
  "Optical Components",
  "Glass",
};

typedef struct Commodity {
  CommodityType type;
  str name;
  u32 base_price_per_kg;
  u32 kg_per_container;
  u32 m3_per_kg;
} Commodity;

typedef struct StarSystem {
  StringChunkList name;
} StarSystem;

typedef struct Planet {
  StringChunkList name;
  bool habitable;
  u32 population;
  u32 commodities[Commodity_Count];
} Planet;

typedef struct {
  i32 x;
  i32 y;
  i32 z;
} XYZ;

typedef enum Direction {
  DirectionInvalid,
  North,
  South,
  East,
  West,
  Up,
  Down,
  Direction_Count
} Direction;

typedef enum CommandType {
  CommandInvalid,
  CommandKeepAlive,
  CommandLogin,
  CommandCreateCharacter,
  CommandType_Count,
} CommandType;
static const char* command_type_strings[] = {
  "Invalid",
  "KeepAlive",
  "Login",
  "CreateCharacter",
};

#define ENTITY_HEADER_MESSAGE_SIZE (8+8+1+1+1)
#define ENTITY_MESSAGE_SIZE (ENTITY_HEADER_MESSAGE_SIZE+2+2+2+2+8+1+1)
typedef enum Message {
  MessageInvalid,
  MessageCharacterId,
  MessageBadPw,
  MessageNewAccountCreated,
  Message_Count,
} Message;
static const char* MESSAGE_STRINGS[] = {
  "Invalid",
  "CharacterId",
  "BadPw",
  "NewAccountCreated",
};

#endif //GAMESHARED_H
