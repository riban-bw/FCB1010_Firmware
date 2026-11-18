# Installing

[Back to the README](../README.md)

## BEWARE: This replaces the stock firmware completely

The stock presets, banks, direct-select modes and the Behringer SysEx editor do not work with this firmware. Put the original chip back to restore stock behaviour. This firmware keeps its settings in the same memory chip as the stock presets: its shared settings and footswitch bank 1 are away from where the stock firmware keeps them, but every other bank you save overwrites part of them. If you want the stock presets back later, save them with a SysEx dump from the stock firmware before you change chips, or be prepared to set them up again.

## Flashing to an EPROM or EEPROM

The program memory is a 28-pin chip in a socket. The EPROM used in my FCB1010 is an HT27C512-70, which is 64KB. Some units have the smaller 27C256 (32KB) or 27C128 (16KB). The firmware is the same for each; only the image file and the replacement chip differ.

| Chip in the socket | Size | Image to write | Replacement chips |
|---|---|---|---|
| 27C512 (for example HT27C512) | 64KB | `build/fcb1010_27c512.bin` | A 27C512-pinout part: W27C512 (electrically erasable, so it can be reprogrammed in the programmer), or a one-time programmable AT27C512R |
| 27C256 | 32KB | `build/fcb1010_27c256.bin` | A 27C256-pinout part such as an AT27C256R, or an AT28C256 EEPROM on an adapter (see step 3) |
| 27C128 | 16KB | `build/fcb1010_27c128.bin` | A 27C128-pinout part, or a larger chip of the socket's pinout with its image |

The 27C512 image is the 32KB firmware twice, so it runs whether the socket drives the chip's top address line (pin 1, A15) high or low. All three images are built by `make` and offered by the GitHub build.

1. **Identify the stock chip.** Open the case (16 screws on the underside). The program memory is the 28-pin socketed chip, often under a label or hot glue. Read the part number and find it in the table above; go by what is printed on yours. I recommend taking a photo of the original physical installation to have a record of what it looked like before you started to make changes.
2. **Back up the original.** Read the stock chip with your programmer and save the file. Put a fresh label on it and keep it. This may allow you to flash the stock firmware to your new, target (E)EPROM to validate the flashing process. It is unlikely that the original chip is rewritable.
3. **Choose a replacement with the same pinout as the socket.** The 27C512 and 27C256 differ on pin 1 (A15 on the 27C512, Vpp on the 27C256), so use a part of the same family as the stock chip. A parallel EEPROM such as the AT28C256 differs from both on pins 1 and 27: pin 27 is /WE on the EEPROM but address line A14 on the EPROMs, so a plain swap can cause spurious write pulses and corrupt the code. If you want an AT28C256 in a 27C256 socket, use a 28C256-to-27C256 adapter board (for example [this PCBWay design](https://www.pcbway.com/project/shareproject/28C256_to_27C256.html), which has jumpers for both pinouts). I haven't tested any of these in an FCB1010.
4. **Program the chip.** With a programmer such as a TL866 and the open-source `minipro` tool, the commands look like this (the device name must match your chip exactly; list the names with `minipro -l | grep -i 27c512`):

   ```
   minipro -p "W27C512@DIP28" -w build/fcb1010_27c512.bin   # 27C512 socket
   minipro -p "W27C512@DIP28" -m build/fcb1010_27c512.bin   # verify
   minipro -p "AT27C256R@DIP28" -w build/fcb1010_27c256.bin # 27C256 socket
   ```

   `minipro` erases an electrically erasable chip such as the W27C512 before writing it. A used UV-erasable EPROM (one with a window) must be erased under UV light first.
5. **Install it with the pedal unplugged from power.** Match pin 1 to the notch on the socket, the same as the old chip was.
6. **Power on.** "riban" should scroll across the display, then it should show `0`. Press UP and release it, and the display should show `1`.

To lower the risk, program a spare chip first and keep the original in a safe place. The firmware has not yet run on a real FCB1010 (see [Testing](testing.md)).

## Using a One ROM

A [One ROM](https://onerom.org) (the 28-pin "Fire" board) can take the place of the chip. It is programmed over USB with the `onerom` command-line tool, so no programmer is needed:

```
onerom program --slot file=build/fcb1010_27c512.bin,type=27C512
```

**The pedal's reset is too short for it.** The One ROM takes longer to start than the pedal's processor, so when both are powered by the pedal the processor runs before the One ROM serves the firmware. Without code to run, it switches itself into power-down within about 50 microseconds and stays there until it is switched off. The pedal stays dark and does not respond. It works if the One ROM's USB socket is connected to a computer before the pedal is switched on.

The fix is a longer reset. The processor's reset comes from a TL7705 supervisor chip whose timing capacitor (on its CT pin, in series with a 1kΩ resistor to ground) is 10nF in my unit, which gives about 130 microseconds. The reset time is about 13ms per µF (1.3 × 10⁴ × CT), and can be half or double that from chip to chip. I replaced the 10nF with a 4.7µF electrolytic (about 60ms; + to the CT pin side, − to ground, rated 6.3V or more), and the pedal now starts every time without USB. Anything up to 10µF (TI's recommended maximum) should work. The bigger capacitor takes longer to discharge, so leave the pedal off for a second or two before switching it on again.
