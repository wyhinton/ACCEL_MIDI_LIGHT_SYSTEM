# Wi-Fi Login Bridge – PC Receiver

Firmware for the ESP32-S3 board that plugs into the target (headless,
auto-login) Windows PC. Pair of `../WIFI_LOGIN_MAC_SENDER`.

See [../WIFI_LOGIN_MAC_SENDER/README.md](../WIFI_LOGIN_MAC_SENDER/README.md)
for the full architecture, protocol reference, and usage instructions.

This board enumerates on Windows as a composite USB device: a serial port
plus a HID keyboard. When it receives Wi-Fi credentials over ESP-NOW, it
**types a self-contained bootstrap command into Windows** (`Win+R` ->
`powershell -EncodedCommand ...` -> `Enter`) that joins the network and
writes the result back over its own serial port. Nothing needs to be
pre-installed on the PC — that's the whole point, since this board is
assumed to be the only access to it. No action needed on the Windows side;
just flash this board, plug it in, and send credentials from the Mac.

If you *do* get one-time physical/remote access to the PC before it's
deployed, `scripts/windows_wifi_provision.ps1` is a more robust
alternative — drop it in the Startup folder once and it runs as a
persistent listener, no HID typing required on future requests:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/windows_wifi_provision.ps1
```
