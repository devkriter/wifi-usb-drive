# Minimalist enclosure for the Waveshare ESP32-S3-LCD-1.47

Two printed parts and four screws. The board's own surface-mount M2 standoffs
hold everything together, so there are no snaps, pins or glue.

Designed in SolidCraft around Waveshare's STEP model of the board
(`ref/esp32-s3-lcd-1_47_asm.stp`, from the wiki's "Structural Drawings" zip);
the drawing `ref/ESP32-S3-LCD-1.47.pdf` and the LCD datasheet gave the display
numbers.

## Files

| File | What |
|---|---|
| `esp32-s3-lcd-1.47-enclosure.solidcraft` | SolidCraft design: board model + both parts assembled, with print-oriented copies of each part placed either side (`Shell (2)`, `BackPlate (2)`) |
| `export/print-shell.stl`, `export/print-back-plate.stl` | print-oriented meshes, mm, flat face on Z = 0 |
| `export/print-layout.3mf` | both parts oriented and laid side by side |
| `export/shell.stl`, `export/back-plate.stl` | the same parts in the assembled position |
| `export/enclosure.3mf` | both parts in one 3MF |
| `export/enclosure.step` | both parts as B-rep for other CAD |
| `renders/*.png` | views with the board inside; `print-layout.png` shows the two print orientations beside the assembly |
| `ref/` | Waveshare STEP, dimension drawing, dimension image |

## Overall size

| | mm |
|---|---|
| Length (without plug) | 40.2 |
| Width | 24.2 |
| Thickness | 10.85 |
| USB-A plug sticks out | 12.8 |
| Wall | 1.6 |
| Front lip over the glass | 0.8 thick, overlaps the glass edge by 0.5–0.6 |
| Display window | 18.2 × 33.4, R1 corners (active area is 17.39 × 32.35) |
| Outer corner radius | 3.0 (front edge 0.8 chamfer, back edge R0.4) |

## How it goes together

1. **Shell** (front lip + four walls). Drop the board in through the open back,
   glass first, until the glass sits under the lip. The BOOT and RST switch
   actuators enter the two 3 mm slots in the long walls from the back edge.
2. **Back plate**. Slide it in from the USB end along the grooves in the walls.
   Its end strip closes the bottom of the USB-A opening and the back of the
   shell. A 0.55 mm channel on its inside clears the microSD slot housing
   while sliding.
3. **Four M2 countersunk screws** (M2 × 4, or M2 × 5 if your standoffs are
   through-threaded, which PEM SMTSO parts are) through the plate into the
   board's standoffs. The two patterns differ: the far pair is 13.28 mm apart
   (x = ±6.64, 1.98 mm from the far edge), the plug-end pair 16.32 mm apart
   (x = ±8.16, 2.40 mm from the plug edge), 32.00 mm between the pairs. The screws pull the board onto the plate; the plate is
   captured by the grooves; the board's glass is captured by the lip and the
   USB end wall, so nothing can slide back out.

Removing the plate (four screws, slide out) is also how you change the
microSD card: the card goes into the slot from the board's centre side and
lies entirely inside the enclosure.

## Clearances used

| Feature | Clearance |
|---|---|
| PCB outline to wall | 0.3 (sides), 0.3 (ends) |
| Glass top to lip underside | 0.15 |
| Plate tongue in groove | 0.1 per side, 0.15 in height |
| USB-A shell in its opening | 0.25 per side |
| Switch actuator slot | 3.0 wide for a 2.0 actuator |
| Display FPC bend at the USB end | 0.5 relief pocket in the end wall |

The interference check against every body of Waveshare's STEP model is clean
(the only overlaps reported are inside Waveshare's own model: plug vs PCB).

Waveshare's STEP model includes 2.54 mm pin headers; they are hidden in the
design and the renders because the board as sold has empty holes. If you
solder headers, this enclosure no longer fits.

## Printing

Use the `print-*` files: they are already oriented and laid side by side.
`enclosure.3mf` and `enclosure.step` keep the assembled positions, so in a
slicer the two parts overlap and the shell sits display-side up; they are for
CAD, not for slicing.

- Shell, front face down (`print-shell.stl`). The lip and window are the
  first layers, so that face takes the bed texture. Walls go straight up, the
  groove ceilings are 0.5 mm overhangs, the switch slots and the USB opening
  open toward the top of the print, and the front edge is a 0.8 mm 45°
  chamfer, not a fillet, so nothing overhangs the bed. No supports.
  0.2 mm layers, 4 perimeters (walls are 1.6 mm).
- Back plate, outer face down (`print-back-plate.stl`). The 90° countersinks
  become 45° overhangs, the microSD channel is a recess in the top face and
  the end strip stands up from the bed. The tongue overhangs the cover by
  0.5 mm at 0.75 mm height, which prints without support. No bridges.
- PETG or PLA, 0.4 mm nozzle. The thinnest spots are the outer corner walls
  next to the groove entry at the USB end (about 0.75 mm over 1 mm of length).

## Things you may want to change

- `Window` sketch: the window is 0.4–0.6 mm larger than the active area on
  each side. If your panel sits differently, move the window edges rather
  than enlarging it.
- `Cavity` sketch: side clearance (10.5 half-width). Tighten to 10.4 if the
  print comes out loose.
- LED window: Ø2.4 at (0, 16.0) over the WS2812 on the back; delete the
  `LED window` feature if you do not want it.
- The pin-header area is solid. If you want the header exposed, cut the back
  plate between y = -9 and 14 at x = ±8.9.

Coordinates in the model: board frame from the STEP file, Z up = display
side, Y toward the far end (USB plug at -Y), origin near the board centre.
