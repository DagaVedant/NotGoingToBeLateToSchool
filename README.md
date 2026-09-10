# NotGoingToBeLateToSchool

a 9 key alarm clock that makes you type a code to shut it up.

built on a xiao esp32c3 for BLARE. during the day it sits on my desk cycling through
weather, hackatime hours, now playing and world clocks. at 6am it tells my phone to start
playing spotify instead of screaming at me with a piezo. you get 3 snoozes, and on the 3rd
one you have to type a code on the keypad before it stops.

every alarm clock i've had is useless for the 16 hours a day you're awake, and way too easy
to snooze into oblivion. this one tries to fix both.

![notgoingtobelatetoschool](images/full.png)

the display screws down through its own two mounting holes into blind pilots in the slanted
face, and seats on the wall around the window instead of floating in it.

![display mounting](images/screw_holes.png)

## what it does

**ambient**, all day

- auto rotates through weather / hackatime hours / now playing / world clocks
- spinning globe on the clock face, same animation as the oled on my hackpad
- one small json from a relay covers all four pages, so the clock makes one https request
  a minute instead of juggling four apis itself

**alarm**, at 6am

- multiple alarms, each with its own days of the week, all editable on the device
- 3 snoozes max. the first two are normal, on the 3rd it drops into code entry and keeps
  going until you type the right code
- screen fades up from near black to full brightness over the 15 min before the alarm
- escalating piezo buzzer that speeds up the longer you ignore it

**ble**, written and compiling, never run on hardware

- pairs as a ble media remote so the alarm hits play on my phone instead of buzzing
- iphone notifications over ancs

## the board

69.5 x 97mm, 2 layers, 25 footprints. 461 tracks, 14 vias, about 1.5m of copper. drc clean,
nothing unrouted.

- keys on the top side at 19.05 pitch so the caps clear each other
- xiao and display header on the underside, so the top face is just keycaps and the buzzer
- xiao at the back with usb-c out the rear, on 2.54 sockets so it can be pulled out
- 8 pin display header at the front edge so the jumper run to the screen is short
- one diode per switch, sitting in the 3.45mm gap directly below it
- 4x m3 mounting holes, pushed out to the corners so they clear the key block

![pcb](images/pcb-3d.png)

nine switches in a 3x3 matrix, one 1n4148 per key so holding two keys down cannot ghost a
third.

![schematic](images/schematic.png)

### pins

the xiao has exactly 11 gpio and this uses every single one. that constraint shaped
basically the whole board.

| what | pins |
|---|---|
| buzzer | 1 |
| tft (sclk, sda, dc, cs) | 4 |
| matrix rows | 3 |
| matrix cols | 3 |
| **total** | **11 / 11** |

got 2 pins back by tying the display's `rst` to 3v3 and `bl` to gnd, which is what the blare
docs say to do when you run out. downside is no hardware brightness control, so the sunrise
ramp is done in software with a dimming palette instead of pwm.

| pad | gpio | net |
|---|---|---|
| d0 | 2 | ROW1 |
| d1 | 3 | TFT_SCLK |
| d2 | 4 | TFT_SDA |
| d3 | 5 | TFT_DC |
| d4 | 6 | TFT_CS |
| d5 | 7 | BUZZER |
| d6 | 21 | COL1 |
| d7 | 20 | COL2 |
| d8 | 8 | COL3 |
| d9 | 9 | ROW3 |
| d10 | 10 | ROW2 |

gpio8 and gpio9 are strapping pins and both have to read high at boot. the rows use internal
pull ups and the columns are hi-z until setup runs, so a key held down at power on can only
pull a line up through its diode, never down. no external pull ups needed.

## the case

two printed parts, 79 x 122.8 x 44mm overall.

- raked shell so the screen sits at a readable angle instead of flat on the desk
- pcb sits on 4 posts with m3x5x4 heatset inserts, 17mm of clear height under it because the
  xiao is socketed and hangs 15.24mm down. that one number sets the whole case depth
- top plate is 2mm with a 58mm square cutout for the keys. glued on, not screwed
- display drops in behind an 18.4 x 63mm window, sized to the 62.5 x 17.9mm glass so the pcb
  overlaps the opening by 5.08mm at each end and has something to sit on. two m2 screws
  through its own mounting holes, 16.26mm apart, into blind pilots in the 2.4mm wall
- usb-c out the back, buzzer holes in the top plate

## firmware

one arduino sketch, `firmware/firmware.ino`. copy `secrets.h.example` to `secrets.h` and
fill in wifi, timezone and the relay url.

board `XIAO_ESP32C3`, esp32 core 3.3.11. libraries: adafruit gfx, adafruit st7735/st7789,
adafruit busio, arduinojson 7, nimble-arduino.

set **Tools > Partition Scheme > Minimal SPIFFS**. with ble on the sketch is 1394487 bytes,
which is 106% of the default partition and will not fit. minimal spiffs puts it at 70% and
keeps ota.

## bom

from the kit:

| qty | part |
|---|---|
| 1 | seeed xiao esp32c3 |
| 9 | mx style switches |
| 9 | blank dsa keycaps |
| 9 | 1n4148 diodes |
| 1 | 2.25in st7789 tft, 284x76 |
| 1 | 3.3v piezo buzzer |
| 1 | 8 pin 2.54mm male header |
| 8 | 20cm f-f jumper wires |
| 4 | m3x5x4 heatset inserts |
| 4 | m3x8mm screws |

not from the kit: 2x m2 self tapping screws, about 5mm, for the display. plus filament.

unused from the kit: 3 switches, 3 keycaps, 3 diodes, 4 inserts and the m3x16 screws, since
the top plate is glued.

## repo

| path | what |
|---|---|
| `PCB/kicad_schematic/` | kicad project |
| `PCB/gerber/` | gerbers and drill file, loose |
| `PCB/gerbers.zip` | the same thing zipped, ready for jlcpcb |
| `PCB/board.step` | board 3d model |
| `CAD/` | case, as part steps and assemblies |
| `firmware/` | arduino sketch |
| `images/` | renders used here |

## licence

mit, see [LICENSE](LICENSE)
