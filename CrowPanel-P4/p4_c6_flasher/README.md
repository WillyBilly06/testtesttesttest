# P4-as-C6 Flasher

Tiny ESP-IDF app that turns the **CrowPanel-P4 into a USB-to-UART bridge**
so you can flash the on-board ESP32-C6 directly through the P4's USB-C
port. No external USB-TTL adapter needed.

This is a **dev tool** — not the normal P4 firmware. Flash it when you
want to flash/monitor the C6, then flash `factory_project` back when
you're done.

## Wiring (already done on the CrowPanel pogo jig)

| P4 pin | Direction | C6 pin / signal     |
|--------|-----------|---------------------|
| GPIO47 | RX  ←     | C6 UART0 TX (GPIO16)|
| GPIO48 | TX  →     | C6 UART0 RX (GPIO17)|
| GPIO33 | OUT →     | C6 BOOT (GPIO9), active LOW |
| GPIO32 | OUT →     | C6 EN  (CHIP_PU),  active LOW |
| GND    |           | GND                  |

Auto-reset mapping (matches CP2102/CH340, which is what `esptool`
expects):

| Host signal | P4 pin behaviour | C6 effect           |
|-------------|------------------|---------------------|
| DTR asserted | BOOT → LOW      | enter download mode |
| RTS asserted | EN   → LOW      | reset chip          |
| neither      | both HIGH       | normal run          |

## Build & flash the bridge onto the P4

From this directory, with ESP-IDF v5.5 environment activated:

```powershell
idf.py set-target esp32p4
idf.py build
# Replace COM7 with whatever the P4 currently shows up as.
idf.py -p COM7 flash
```

Once the P4 reboots, a **new** COM port appears (the P4's native USB-CDC
provided by TinyUSB). That's the one you talk to with esptool.

## Flash the C6 firmware

```powershell
# Replace COMx with the new port (will be different from the one above).
python -m esptool --chip esp32c6 -p COMx -b 460800 ^
    write_flash 0x0 ..\c6_firmware\build\crowpanel_c6_firmware.bin
```

Or to flash the full set (bootloader + partition + app), with the
artifacts already built by the `c6_firmware` project:

```powershell
python -m esptool --chip esp32c6 -p COMx -b 460800 ^
    write_flash @..\c6_firmware\build\flash_args
```

## Monitor the C6

Any serial monitor on the new COM port at 115200 8N1 will show the C6's
boot log:

```powershell
python -m esp_idf_monitor -p COMx -b 115200 --target esp32c6
```

## Returning to normal operation

Reflash the factory firmware:

```powershell
cd ..\factory_project
idf.py -p COM7 flash
```

## How it works

`main/main.c`:
- Sets up **UART1** at GPIO47/48 with 4 KB RX/TX buffers.
- Drives **GPIO33 (BOOT)** and **GPIO32 (EN)** as outputs, idle HIGH.
- Installs **TinyUSB** with a CDC-ACM interface.
- USB → C6: CDC RX callback drains all received bytes and `uart_write_bytes`
  them out.
- C6 → USB: a small task pumps `uart_read_bytes` into
  `tinyusb_cdcacm_write_queue` + flush.
- DTR / RTS line-state changes are mapped to BOOT / EN levels with the
  same polarity esptool expects.
- Baud rate changes from the host (CDC line-coding event) are applied to
  UART1 with `uart_set_baudrate`. esptool drives this up to 460800 or
  921600 during flashing.

## Notes / gotchas

- **GPIO32 conflict**: in normal P4 firmware, GPIO32 is also used as
  `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE`. That's the same physical
  pin tied to C6 EN — both purposes are compatible with the level
  manipulation the bridge does, but you should **not run the bridge and
  the factory firmware at the same time** (you can't anyway — they're
  separate apps).
- The P4 has *two* USB peripherals (USB-OTG and USB-Serial-JTAG). The
  CrowPanel's USB-C is wired to **USB-OTG**. The bridge uses TinyUSB on
  USB-OTG; the IDF default console stays on UART0 so the two never
  collide.
- If the new COM port doesn't appear: check Device Manager. On Windows
  10/11 the P4 enumerates as a USB-CDC-ACM device (no driver needed).
- If the C6 doesn't enter download mode, your DTR/RTS polarity is
  inverted (some terminal apps drive them as "asserted = HIGH"). esptool
  uses the standard CP2102 polarity that this bridge follows.
