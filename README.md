# Space Trading

**Space Trading** is a computerized board game intended to be played together in a room with family or friends. The "game board" is displayed by the server on the big TV, and each player has their laptop running the client to control the character.

The goal is to pay off your ship's mortgage first.

# How To Play

*This project uses [GCC](https://gcc.gnu.org/) to build. Make sure you have that installed locally first.*

On every computer that you will need to play, download this repository:
```
git clone git@github.com:Tenari/space-trading.git
cd space-trading
```
and then build it:

MacOSX/unix:
```bash
./make.sh server

or

./make.sh client
```

Windows (client only):
```bash
./win_make.bat
```

The resulting executable binary file will be in the `build` directory. Run it from the unix command line:
```bash
./build/server

or

./build/client
```

Or, double-click on the windows .exe file.

---

All computers you will use to play must be connected to the same Wi-Fi network. It uses Local Area Networking (LAN) to connect them together.

1. connect a computer to your main TV/projector screen. This will be your "server." Run the server binary
2. you can press `TAB` on the server to switch between Debug and Map. Map is the main game-view.
3. have each player run their `client` application. It will prompt them for the `Server IP Address`, which should be displayed in the top right corner of the server.
4. Don't bother with secure passwords when logging in. The password is just to prevent your brother from trying to login as you.
5. Wait until each player has selected their ship and sees the "Map" tab on their screen.
6. Start playing!

### ideas

space-trader-multiplayer-boardgame-esque:

- projector shows the star-map with everyone's positions
- players all have a laptop to control their character
    - choose a starting ship
        - you have 10_000 credits, and the down-payment % of the ship total value affects the interest rate you get
- Ships:
    - attributes:
        - cargo space
        - Alcubierre Drive efficiency
        - Passenger space
        - Passenger amenities
        - smugglers holds
        - life-support system
    - Sparrow [2, 1, 0, shared-bunk, 1, 0, 1, 100k]
    - Dart [5, 8, 2, shared-bunk, 0, 1, 2, 150k]
    - Hauler-Prime Z-1 [120, 2, 0, X, 2, 1, 4, 180k]
    - NX-400 [15, 4, 8, shared-bunk, 0, 2, 3, 280k]
    - CabinRun 40-X [40, 3, 45, shared-bunk, 1, 4, 6, 380k]
    - IronBulk X7 [350, 3, 0, X, 3, 2, 8, 420k]
    - Solaris-200 [25, 6, 24, shared-bunk, 0, 3, 4, 520k]
    - Merchant's Run [180, 5, 12, shared-bunk, 1, 2, 5, 620k]
    - Genesis-Long-Haul V2 [100, 4, 80, shared-bunk, 2, 6, 10, 720k]
    - Titan-Class HC-3000 [800, 4, 0, X, 3, 3, 15, 890k]
    - Steady Hand [420, 6, 25, shared-bunk, 1, 4, 7, 1.1m]
    - Pathfinder [60, 6, 65, shared-bunk, 0, 5, 8, 1.4m]
    - Ark-Class Transport A1 [250, 5, 95, shared-bunk, 3, 8, 18, 1.6m]
    - Starlight [8, 9, 5, private-apartment, 0, 3, 2, 2.1m]
    - Leviathan's Route [900, 7, 40, shared-bunk, 2, 6, 12, 2.3m]
    - Voyager's Dream [140, 7, 100, shared-bunk, 1, 8, 12, 2.8m]
    - Apex-5000 Deluxe [150, 8, 18, private-apartment, 0, 4, 6, 3.2m]
    - Aurora Crown [18, 10, 35, private-apartment, 0, 4, 5, 3.8m]
    - TerraFreight Luxe-900 [380, 9, 30, private-apartment, 0, 5, 8, 4.5m]
    - Odyssey [320, 8, 100, shared-bunk, 2, 10, 20, 5.4m]
    - Celestial Palace [55, 9, 85, private-apartment, 0, 7, 10, 6.2m]
    - Sovereign Expanse [950, 10, 60, private-apartment, 1, 8, 20, 8.7m]
    - Nexus-Elite-7 [120, 10, 100, private-apartment, 0, 9, 14, 9.1m]
    - Infinity's Embrace [280, 10, 100, private-apartment, 1, 10, 22, 10m]
- turn-based. Everyone does their trading at the same time, and then flies to the next station
    - instead of making journeys take longer, for game-sake, longer journeys just cost more fuel.
    - "faster" drives are actually just more fuel efficient
- the prices of goods are supply/demand based on quantity produced/consumed at planets/stations
- the prices/goods available at a place are only visible to those who are in the station
- stations may build "reputation" prices for repeat suppliers/buyers.
- players can trade with each other while at the same station
- certain stations have shipyards, with mortgages available for trade-in/upgrading ships drives/storage capacity, etc.
- all systems have a safety rating, indicating likelyhood of piracy. High risk systems often have higher profitablilty
    - "safe" stations have high risk of being inspected for illegal goods
    - "criminal" stations have high risk of piracy
    - but it's always a dice roll that some event happens at all when you arrive in system.
- AI station trading agent for haggling deal-making?
- transport contracts available at stations randomly.
- in a system, you can fly out to the asteroid belt and try to buy from miners directly, but it's a gamble on even finding any of them.
- game ends when someone pays off their ship's mortgage.
- passengers/VIP transport
- playstyles:
    - Mass-Cargo hauler
    - Fast-expensive-goods transport (precious metals)
    - passenger transport
- commodities:
    - Hydrogen (Fuel)
    - Oxygen
    - Water
    - Food: (plants provide seed versions as well)
        - Wheat
        - rice
        - corn
        - potatoes
        - beef
        - chicken
        - butter
        - olive oil
        - alcohol
    - fertilizer
    - cleaning supplies
    - air filters
    - Chemicals
    - clothes
    - textiles
    - Electronics
    - Construction parts
    - 3D printer parts
    - clothing fabricator
    - hand tools
    - machine tools
    - cutlery
    - robots
    - plastics
    - hand weapons
    - batteries
    - solar panels
    - medicines
    - optical components
    - glass
    - Metals (Ore-form and Purified-form):
        - Lithium
        - Berylium
        - Magnesium
        - Aluminium
        - Silicon
        - Scandium
        - Titanium
        - Vanadium
        - Chromium
        - Manganese
        - Iron
        - Cobalt
        - Nickel
        - Copper
        - Zinc
        - Selenium
        - Zirconium
        - Molybdenum
        - Ruthenium
        - Rhodium
        - Palladium
        - Silver
        - Cadmium
        - Tin
        - Tungsten
        - Rhenium
        - Iridium
        - Platinum
        - Gold
        - Mercury
        - Lead
