# PiKVM V3 OLED status

> Retained Raspberry Pi enclosure display. The ESP32-P4 product prototype uses
> the separate SH1106 adapter documented in
> [`../firmware/esp32p4/README.md`](../firmware/esp32p4/README.md).

The development RevLink unit uses a Raspberry Pi 4 Model B Rev 1.2 inside a
PiKVM V3-style enclosure. Its front window contains the standard 0.91-inch,
128x32 SSD1306 I2C OLED. RevLink runs Debian rather than PiKVM OS, so the
display is controlled by the independent `revlink-oled.service`.

The display is optional. A missing OLED, I2C error, or failed display service
must never stop `revlink.service`, USB synchronization, or the web interface.

## Architecture

`revlink.service` writes display-safe JSON to:

```text
/run/revlink/display-status.json
```

`revlink-oled.service` reads that local runtime file and renders it on
`/dev/i2c-1` at address `0x3c`. The development enclosure mounts the panel
inverted relative to the SSD1306 default, so its unit passes `--rotate 180`.
No web credentials, network endpoint, device serial number, map contents, or
datalog contents are sent to the display service.

The OLED reports:

- Wi-Fi network association attempts by SSID;
- the successful uplink SSID;
- a live countdown until the Wi-Fi watchdog scans again;
- waiting for an AccessPort;
- USB device detected;
- connecting and reading identity;
- incremental synchronization;
- downloads and uploads;
- map and startup-image verification;
- completion and failure states.

Wi-Fi passwords are never written to the display-status file or shown on the
OLED. A Wi-Fi transition temporarily takes priority over the normal idle USB
status, then the display automatically returns to AccessPort state.

## Enable the development enclosure

Install RevLink first, then run:

```sh
sudo deploy/enable-pikvm-oled.sh
sudo reboot
```

The helper:

1. backs up the Raspberry Pi boot configuration the first time it changes it;
2. adds `dtparam=i2c_arm=on`;
3. loads `i2c-dev` at boot;
4. grants the `revlink` service account access through the `i2c` group;
5. installs and enables `revlink-oled.service`.

After reboot:

```sh
i2cdetect -y 1
systemctl status revlink-oled.service --no-pager -l
journalctl -u revlink-oled.service -n 50 --no-pager
```

The scan should show `3c`. If it does not, stop the OLED service and inspect
the ribbon cable and HAT seating before changing the configured address.

## Disable

```sh
sudo systemctl disable --now revlink-oled.service
```

Leaving I2C enabled does not affect RevLink USB operation. To restore the
original boot configuration, use the backup created beside `config.txt` and
reboot. Do not overwrite later boot changes blindly.
