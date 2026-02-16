#include "base/all.h"
#include "string_chunk.h"

#ifndef GAMESHARED_H
#define GAMESHARED_H

///// #define some game-tunable constants
#define STARTING_DOWN_PAYMENT (10000)
#define SHIP_DETAIL_COUNT (8)
#define STAR_SYSTEM_COUNT (40)
#define MAP_WIDTH (40)
#define MAP_HEIGHT (10)
#define MAX_PLANETS (3)

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
global str SHIP_TYPE_STRINGS[] = {
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

fn f32 calcInterestRate(u32 cost, u32 down_payment) {
  f32 mortgage_rate = 10.0 *(1.0 - (3.0*(f32)down_payment / ((f32)cost/3.0)));
  return mortgage_rate;
}

typedef enum {
  FieldTypeU8,
  FieldTypeU16,
  FieldTypeU32,
  FieldTypeFloat,
  FieldTypeString,
  FieldTypeEnum,
  FieldType_Count,
} FieldType;

typedef struct FieldDescriptor {
  str name;
  FieldType type;
  size_t offset;
  int width;  // column width for display
  str* enum_vals;
} FieldDescriptor;

// Define field metadata using offsetof
#ifndef offsetof
#define offsetof(st, m) ((size_t)&(((st*)0)->m))
#endif

// Field descriptors for Employee
global FieldDescriptor SHIP_FIELDS[SHIP_DETAIL_COUNT] = {
  { "Type", FieldTypeEnum, offsetof(ShipTemplate, type), 16, (str*)&SHIP_TYPE_STRINGS },
  { "Cost", FieldTypeU32, offsetof(ShipTemplate, base_cost), 8 },
  { "DriveEff", FieldTypeU8, offsetof(ShipTemplate, drive_efficiency), 8 },
  { "LifeSupp", FieldTypeU8, offsetof(ShipTemplate, life_support_efficiency), 8 },
  { "V.Cargo", FieldTypeU16, offsetof(ShipTemplate, vacuum_cargo_slots), 7 },
  { "C.Cargo", FieldTypeU16, offsetof(ShipTemplate, climate_cargo_slots), 7 },
  { "Passengers", FieldTypeU16, offsetof(ShipTemplate, passenger_berths), 10 },
  { "SmugglersHold", FieldTypeU16, offsetof(ShipTemplate, smugglers_hold_cu_m), 13 },
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

typedef enum EquipmentType {
  EquipmentTypeAquaponicsSystem,
  EquipmentType3DPrinter,
  EquipmentTypeAutoTailor,
  EquipmentType_Count,
} EquipmentType;

typedef enum CommodityType {
  CommodityHydrogenFuel,
  CommodityOxygen,
  CommodityWater,
  CommodityFertilizer,
  CommodityRawTextiles,
  CommodityLowGradeOre,
  CommodityHighGradeOre,
  CommodityPlastics,
  CommodityGrain,
  CommodityMeat,
  CommoditySpices,
  CommodityElectronics,
  CommodityGlass,
  CommodityHandTools,
  CommoditySemiConductors,
  CommodityCommonMetals,
  CommodityRareMetals,
  CommodityAlcohol,
  CommodityClothes,
  CommodityPersonalSundries,
  Commodity_Count,
} CommodityType;

static const char* COMMODITY_STRINGS[] = {
  "Hydrogen Fuel",
  "Oxygen",
  "Water",
  "Fertilizer",
  "Raw Textiles",
  "Low grade Ore",
  "High grade Ore",
  "Plastics",
  "Grain",
  "Meat",
  "Spices",
  "Electronics",
  "Glass",
  "Hand Tools",
  "Semi-conductors (Silicon, Arsenic, Boron)",
  "Common Metals (Iron, Nickel, Zinc)",
  "Rare Metals (Titanium, Chromium)",
  "Alcohol",
  "Clothes",
  "Personal Sundries (Cutlery, Storage, Buckles, Toys, Furnishings)",
};

typedef struct Commodity {
  CommodityType type;
  str name;
  u32 base_price_per_kg;
  u32 kg_per_container;
  u32 m3_per_kg;
} Commodity;

typedef enum PlanetType {
  PlanetTypeNull,
  PlanetTypeEarth,
  PlanetTypeGas,
  PlanetTypeMoon,
  PlanetTypeAsteroid,
  PlanetTypeStation,
  PlanetType_Count,
} PlanetType;

typedef struct Planet {
  PlanetType type;
  u32 commodities[Commodity_Count];
  u32 production[Commodity_Count];
} Planet;

typedef struct StarSystem {
  u32 x;
  u32 y;
  u32 crime;
  str name;
  Planet planets[MAX_PLANETS];
} StarSystem;

global str STAR_NAMES[STAR_SYSTEM_COUNT] = {
  "Achernar",
  "Aldebaran",
  "Mining Colony 17",
  "Antares",
  "Arcturus",
  "Barnard's Star",
  "Betelgeuse",
  "Canopus",
  "Capella",
  "Castor",
  "Centauri Prime",
  "Deneb",
  "Epsilon Eridani",
  "Fomalhaut",
  "Gliese 581",
  "Hadar",
  "Izar",
  "Kepler-186",
  "Lacaille 8760",
  "Lalande 21185",
  "Mintaka",
  "Mirach",
  "Polaris",
  "Pollux",
  "Procyon",
  "Proxima",
  "Regulus",
  "Rigel",
  "Ross 154",
  "Sirius",
  "Spica",
  "Tau Ceti",
  "Trappist-1",
  "Vega",
  "Wolf 359",
  "Zeta Reticuli",
  "Alnitak",
  "Bellatrix",
  "Denebola",
  "Eltanin",
};

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
  MessageStarPositions,
  MessageSystemCommodities,
  Message_Count,
} Message;
static const char* MESSAGE_STRINGS[] = {
  "Invalid",
  "CharacterId",
  "BadPw",
  "NewAccountCreated",
  "StarPositions",
  "SystemCommodities",
};

#endif //GAMESHARED_H
