#include "shared.h"
#include "lib/tui.c"

#define MAX_SCREEN_HEIGHT 300
#define MAX_SCREEN_WIDTH 800

fn str strForPlanet(PlanetType type) {
  switch(type) {
    case PlanetTypeNull:
      return "  ";
    case PlanetTypeAsteroid:
      return "🪨";
    case PlanetTypeEarth:
      return "🌎";
    case PlanetTypeGas:
      return "🪐";
    case PlanetTypeMoon:
      return "🌘";
    case PlanetTypeStation:
      return "\xF0\x9F\x9B\xB0 ";
    case PlanetType_Count:
      assert(false && "this should never happen");
      return "";
  }
}

