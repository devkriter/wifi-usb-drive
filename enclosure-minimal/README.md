# Minimal tray enclosure: Waveshare ESP32-S3-LCD-1.47

A one-piece tray with no lid. The board drops in face up, the LCD sits flush
with the rim (0.2 mm below it), and four M2 screws from the back pull the
board's SMT standoffs down onto the floor.

- Outside size: 23.3 x 40.0 x 9.25 mm, not counting the USB plug
  (the reference design is about 25.4 x 42.3 x 17 mm).
- Wall 1.2 mm, floor 1.5 mm, 0.3 mm clearance around the PCB.
- Made for a board **without pin headers**.

## Files
- `esp32-s3-lcd147-tray.stl`: the file to print.
- `esp32-s3-lcd147-tray.step`: for editing in other CAD tools.
- `esp32-s3-lcd147-tray.solidcraft`: the SolidCraft design, with the board model
  (`ref/esp32-s3-lcd-1_47_asm.stp`, from Waveshare) in place.
- `esp32-s3-lcd147-tray-closed.*`: the same tray with a solid back (no TF window).
- `ref/`: Waveshare's drawing and STEP model, plus the bulky reference design.

## Printing
- Print with the floor on the bed, as exported. You don't need supports: every
  opening is open at the top, and the countersinks are 45°.
- 0.2 mm layers. PLA or PETG both work.

## Hardware
- 4x M2 x 4 countersunk screws (DIN 965 / ISO 7046). They thread into the
  board's M2 SMT standoffs (2.5 mm tall), so no nuts are needed. The heads sit
  flush in the floor, and the tips stop at the underside of the PCB. M2 x 3
  also works. Don't use screws longer than 4 mm, or the tips can press into the
  back of the LCD.

## Openings
- USB end: a U-notch for the plug. The floor runs out under the plug.
- Both sides: a 4 mm notch for BOOT and RST. The plungers sit about 0.3 mm
  inside the wall, so you press them with a fingernail.
- Back: a 12.4 x 22 mm window over the TF socket. The socket hangs 0.4 mm below
  the standoffs, and the window lets you push the card in or out without taking
  the case off.
- Closed version: instead of the window, a 0.5 mm pocket on the inside of the
  floor (12.4 x 16.8 mm) makes room for the socket and the card's path. The
  floor there is 1.0 mm thick and the outside is flat. To swap the card, take
  out the 4 screws.
