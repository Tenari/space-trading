#include "base/all.h"
#include "string_chunk.h"

#ifndef GAMESHARED_H
#define GAMESHARED_H

///// #define some game-tunable constants
#define STARTING_DOWN_PAYMENT (10000)
#define SHIP_DETAIL_COUNT (8)
#define STAR_SYSTEM_COUNT (16)
#define MAP_WIDTH (32)
#define MAP_HEIGHT (12)
#define MAX_PLANETS (3)
#define MAX_PASSENGER_JOB_OFFERS (16)
#define MAX_PASSENGER_JOB_PEOPLE (4)
#define MAX_PASSENGER_JOB_PRICE (10000)
#define MAX_PASSENGER_BERTHS (8)

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
  u32 cu_m_fuel;
  u32 cu_m_o2;
} ShipTemplate;

global ShipTemplate SHIPS[] = {
  {
    .type = ShipSparrow, .drive_efficiency = 1, .life_support_efficiency = 1,
    .vacuum_cargo_slots = 5, .climate_cargo_slots = 0, .passenger_berths = 0,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 1, .base_cost = 100000,
    .cu_m_fuel = 2500, .cu_m_o2 = 500,
  },
  {
    .type = ShipDart,    .drive_efficiency = 7, .life_support_efficiency = 3,
    .vacuum_cargo_slots = 6, .climate_cargo_slots = 0, .passenger_berths = 1,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 0, .base_cost = 150000,
    .cu_m_fuel = 2000, .cu_m_o2 = 1500,
  },
  {
    .type = ShipHaulerPrimeZ1, .drive_efficiency = 5, .life_support_efficiency = 2,
    .vacuum_cargo_slots = 30, .climate_cargo_slots = 0, .passenger_berths = 0,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 0, .base_cost = 275000,
    .cu_m_fuel = 2500, .cu_m_o2 = 800,
  },
  {
    .type = ShipNX400, .drive_efficiency = 6, .life_support_efficiency = 5,
    .vacuum_cargo_slots = 3, .climate_cargo_slots = 0, .passenger_berths = 3,
    .passenger_amenities_flags = 0,
    .smugglers_hold_cu_m = 0, .base_cost = 250000,
    .cu_m_fuel = 2000, .cu_m_o2 = 3000,
  },
};

global FieldDescriptor SHIP_FIELDS[SHIP_DETAIL_COUNT] = {
  { "Type", FieldTypeEnum, offsetof(ShipTemplate, type), 16, (str*)&SHIP_TYPE_STRINGS },
  { "Cost", FieldTypeU32, offsetof(ShipTemplate, base_cost), 8 },
  { "Cargo", FieldTypeU16, offsetof(ShipTemplate, vacuum_cargo_slots), 7 },
  { "PassengerBerths", FieldTypeU16, offsetof(ShipTemplate, passenger_berths), 16 },
  { "DriveEff", FieldTypeU8, offsetof(ShipTemplate, drive_efficiency), 8 },
  { "LifeSupp", FieldTypeU8, offsetof(ShipTemplate, life_support_efficiency), 8 },
  { "FuelTank", FieldTypeU32, offsetof(ShipTemplate, cu_m_fuel), 8 },
  { "O2 Tank", FieldTypeU32, offsetof(ShipTemplate, cu_m_o2), 8 },
};

typedef enum CommodityType {
  CommodityHydrogenFuel,
  CommodityOxygen,
  CommodityWater,
  CommodityFood,
  CommodityRawTextiles,
  CommodityOre,
  CommodityPlastics,
  CommoditySemiConductors,
  CommodityMetals,
  CommodityGlass,
  CommodityHandTools,
  CommodityElectronics,
  Commodity_Count,
} CommodityType;

global str COMMODITY_STRINGS[Commodity_Count] = {
  "Hydrogen Fuel",
  "Oxygen",
  "Water",
  "Food",
  "Raw Textiles",
  "Ore",
  "Plastics",
  "Semi-conductors",
  "Metals",
  "Glass",
  "Hand Tools",
  "Electronics",
};

typedef enum StorageUnit {
  StorageUnitKg,
  StorageUnitContainer,
  StorageUnitAtmoContainer,
  StorageUnit_Count,
} StorageUnit;

global str STORAGE_UNIT_STRINGS[StorageUnit_Count] = {
  "kg",
  "cont",
  "atmo"
};

typedef struct Commodity {
  CommodityType type;
  StorageUnit unit;
  str name;
  u32 price;
  u32 qty;
  u32 consumption;
} Commodity;

global Commodity COMMODITIES[Commodity_Count] = {
  { .type = CommodityHydrogenFuel,  .unit = StorageUnitKg,
    .name = "Hydrogen Fuel",
    .price = 10, .qty = 10000, .consumption = 100,
  },
  { .type = CommodityOxygen,  .unit = StorageUnitKg,
    .name = "Oxygen",
    .price = 5, .qty = 5000, .consumption = 80,
  },
  { .type = CommodityWater,  .unit = StorageUnitContainer,
    .name = "Water",
    .price = 100, .qty = 100, .consumption = 10,
  },
  { .type = CommodityFood,  .unit = StorageUnitContainer,
    .name = "Food",
    .price = 200, .qty = 70, .consumption = 10,
  },
  { .type = CommodityRawTextiles,  .unit = StorageUnitContainer,
    .name = "Raw Textiles",
    .price = 500, .qty = 40, .consumption = 3,
  },
  { .type = CommodityOre,  .unit = StorageUnitContainer,
    .name = "Ore",
    .price = 1000, .qty = 100, .consumption = 8,
  },
  { .type = CommodityPlastics,  .unit = StorageUnitContainer,
    .name = "Plastics",
    .price = 2000, .qty = 30, .consumption = 5,
  },
  { .type = CommoditySemiConductors,  .unit = StorageUnitContainer,
    .name = "Semi-conductors",
    .price = 4000, .qty = 40, .consumption = 3,
  },
  { .type = CommodityMetals,  .unit = StorageUnitContainer,
    .name = "Metals",
    .price = 5000, .qty = 40, .consumption = 3,
  },
  { .type = CommodityGlass,  .unit = StorageUnitContainer,
    .name = "Glass",
    .price = 9200, .qty = 50, .consumption = 5,
  },
  { .type = CommodityHandTools,  .unit = StorageUnitContainer,
    .name = "Hand Tools",
    .price = 12000, .qty = 9, .consumption = 2,
  },
  { .type = CommodityElectronics,  .unit = StorageUnitContainer,
    .name = "Electronics",
    .price = 21000, .qty = 20, .consumption = 3,
  },
};

typedef struct MarketCommodity {
  CommodityType type;
  StorageUnit unit;
  u32 bid;
  u32 ask;
  u32 qty;
  u32 owned;
} MarketCommodity;

global FieldDescriptor MARKET_COMMODITY_FIELDS[Commodity_Count] = {
  { "Commodity", FieldTypeEnum, offsetof(MarketCommodity, type), 44, (str*)&COMMODITY_STRINGS },
  { "Unit", FieldTypeEnum, offsetof(MarketCommodity, unit), 6, (str*)&STORAGE_UNIT_STRINGS },
  { "#", FieldTypeU32, offsetof(MarketCommodity, qty), 6 },
  { "Bid", FieldTypeU32, offsetof(MarketCommodity, bid), 6 },
  { "Ask", FieldTypeU32, offsetof(MarketCommodity, ask), 6 },
  { "Owned", FieldTypeU32, offsetof(MarketCommodity, owned), 6 },
};

typedef struct Passenger {
  u8 goal_system_idx;
  u8 people;
  u8 turns_remaining;
  u32 reward;
} Passenger;

typedef struct PassengerJobOffer {
  u8 goal_system_idx;
  u8 people; // 1 - 4, affects how much oxygen they consume
  u8 time_limit; // # of turns until they leave your ship without paying
  u32 offer;
} PassengerJobOffer;

typedef struct DisplayPassengerJobOffer {
  str destination;
  u8 people;
  u8 time_limit;
  u8 goal_system_idx;
  u32 offer;
} DisplayPassengerJobOffer;

global FieldDescriptor PASSENGER_JOB_OFFER_FIELDS[] = {
  { "Destination", FieldTypeString, offsetof(DisplayPassengerJobOffer, destination), 44 },
  { "People", FieldTypeU8, offsetof(DisplayPassengerJobOffer, people), 6 },
  { "Turns", FieldTypeU8, offsetof(DisplayPassengerJobOffer, time_limit), 6 },
  { "Offer", FieldTypeU32, offsetof(DisplayPassengerJobOffer, offer), 6 },
};

typedef struct PlayerShip {
  ShipType type;
  bool ready_to_depart;
  u8 system_idx;
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
  u32 credits;
  u64 id;
  u32 commodities[Commodity_Count];
  Passenger passengers[MAX_PASSENGER_BERTHS];
} PlayerShip;

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
  bool changed;
  u32 x;
  u32 y;
  u32 crime;
  u32 idx;
  str name;
  Planet planets[MAX_PLANETS];
  PassengerJobOffer offers[MAX_PASSENGER_JOB_OFFERS];
} StarSystem;

global str STAR_NAMES[STAR_SYSTEM_COUNT] = {
  "Vega",
  "Aldebaran",
  "Mining Colony 17",
  "Antares",
  "Mintaka",
  "Barnard's Star",
  "Betelgeuse",
  "Tau Ceti",
  "Capella",
  "Castor",
  "Centauri Prime",
  "Deneb",
  "Epsilon Eridani",
  "Fomalhaut",
  "Gliese 581",
  "Hadar",
/*  "Izar",
  "Achernar",
  "Kepler-186",
  "Lacaille 8760",
  "Lalande 21185",
  "Arcturus",
  "Canopus",
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
  "Trappist-1",
  "Wolf 359",
  "Zeta Reticuli",
  "Alnitak",
  "Bellatrix",
  "Denebola",
  "Eltanin",
*/
};

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
  CommandTransact,
  CommandReadyStatus,
  CommandSetDestination,
  CommandPayMortgage,
  CommandAcceptPassengerJob,
  CommandType_Count,
} CommandType;

static const char* command_type_strings[CommandType_Count] = {
  "Invalid",
  "KeepAlive",
  "Login",
  "CreateCharacter",
  "ReadyStatus",
  "SetDestination",
  "PayMortgage",
  "AcceptPassengerJob",
};

typedef enum Message {
  MessageInvalid,
  MessageCharacterId,
  MessageBadPw,
  MessageNewAccountCreated,
  MessageStarPositions,
  MessageSystemCommodities,
  MessagePlayerDetails,
  MessageTransactionResult,
  MessageTurnTick,
  MessageGameOver,
  MessagePayoffResult,
  MessageSystemPassengers,
  MessageJobAcceptResult,
  MessageJobComplete,
  Message_Count,
} Message;

static const char* MESSAGE_STRINGS[] = {
  "Invalid",
  "CharacterId",
  "BadPw",
  "NewAccountCreated",
  "StarPositions",
  "SystemCommodities",
  "PlayerDetails",
  "TransactionResult",
  "TurnTick",
  "GameOver",
  "PayoffResult",
  "SystemPassengers",
  "JobAcceptResult",
};

///// shared helper functions
fn u32 fuelCostForTravel(u32 drive_efficiency, Pos2 current, Pos2 dest) {
  u32 x_distance = Max(current.x, dest.x) - Min(current.x, dest.x);
  u32 y_distance = Max(current.y, dest.y) - Min(current.y, dest.y);
  u32 fuel_per_dist = 100 - drive_efficiency*5;
  return (x_distance + y_distance) * fuel_per_dist;
}

fn u32 usedVacuumCargoSlots(PlayerShip ship) {
  u32 used_cargo = 0;
  for (u32 i = 0; i < Commodity_Count; i++) {
    if (COMMODITIES[i].unit == StorageUnitContainer) {
      used_cargo += ship.commodities[i];
    }
  }
  return used_cargo;
}

fn f32 priceForCommodity(CommodityType type, u32 quantity, bool bid) {
  f32 result = 0.0;
  Commodity details = COMMODITIES[type];
  f32 max = details.price * 3;
  f32 min = (f32)details.price / 3.0;
  if (quantity == 0) {
    return max;
  }
  result = ((f32)details.price) * ((f32)details.qty / (f32)quantity); // the bid/ask spread
  if (max < result) {
    result = max;
  }
  if (min > result) {
    result = min;
  }
  // the bid/ask spread
  if (bid) {
    result = result * 0.97;
  } else {
    result = result * 1.03;
  }
  return result;
}

fn f32 calcInterestRate(u32 cost, u32 down_payment) {
  return 3.0;
  //f32 mortgage_rate = 10.0 *(1.0 - (3.0*(f32)down_payment / ((f32)cost/3.0)));
  //return mortgage_rate;
}

fn bool passengerJobEq(PassengerJobOffer a, PassengerJobOffer b) {
  return a.goal_system_idx == b.goal_system_idx &&
    a.offer == b.offer &&
    a.people == b.people &&
    a.time_limit == b.time_limit;
}

fn u32 shipTotalPassengers(PlayerShip ship) {
  u32 count = 0;
  for (u32 i = 0; i < MAX_PASSENGER_BERTHS; i++) {
    if (ship.passengers[i].people > 0) {
      count += ship.passengers[i].people;
    }
  }
  return count;
}

fn u32 shipUsedPassengerBerths(PlayerShip ship) {
  u32 used = 0;
  for (u32 i = 0; i < MAX_PASSENGER_BERTHS; i++) {
    if (ship.passengers[i].people > 0) {
      used += 1;
    }
  }
  assert(used <= ship.passenger_berths);
  return used;
}

fn u32 shipAvailablePassengerBerths(PlayerShip ship) {
  return (u32)ship.passenger_berths - shipUsedPassengerBerths(ship);
}

fn u32 oxyCostForTravel(PlayerShip* ship, Pos2 current, Pos2 dest) {
  u32 x_distance = Max(current.x, dest.x) - Min(current.x, dest.x);
  u32 y_distance = Max(current.y, dest.y) - Min(current.y, dest.y);
  u32 oxy_per_person_per_dist = 15 - ship->life_support_efficiency;
  // +1 because the pilot uses oxy as well
  return (x_distance + y_distance) * oxy_per_person_per_dist * (shipTotalPassengers(*ship) + 1);
}

#endif //GAMESHARED_H
