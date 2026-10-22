# Copilot credit meter (ESP32 + SSD1306)

Shows your GitHub Copilot credit usage on a 128x64 OLED: percentage used (one decimal place),
credits used (one decimal place) and limit, a progress bar, and time until the monthly reset.

## Hardware

| SSD1306 (I2C) | ESP32-WROOM-32 |
|---------------|----------------|
| VCC           | 3V3            |
| GND           | GND            |
| SDA           | GPIO 21        |
| SCL           | GPIO 22        |

If your display is SH1106 or a different size, change the `U8G2_...` constructor at the top of `src/main.cpp`.

## How the data is fetched

The firmware experimentally calls GitHub's internal Copilot endpoint:

    GET https://api.github.com/copilot_internal/user

It reads the quota category selected during setup:

- **Personal:** `quota_snapshots.chat.credits_used`
- **Business / Enterprise:** `quota_snapshots.premium_interactions.credits_used`

Choose the account type whose quota contains your usage. This is an undocumented endpoint used
by Copilot clients, not a supported public API; its behavior and response fields can change or
stop working. It requires a Copilot session token, not a regular GitHub personal access token.
Session tokens can expire, and the endpoint may not be available for every account.

Enter the corresponding credit limit shown for your account in Copilot. The progress percentage
is calculated from the selected quota's `credits_used` and this configured limit. Reset time is
currently computed as 00:00 UTC on the first of each month.

## Copilot token

The setup form expects a Copilot session token. It does not exchange a GitHub PAT for one.
The firmware does not log the token, but saves it in the ESP32's normal, unencrypted NVS flash.
Treat it as a secret: do not share it, and erase it from the device if it is no longer needed.
Because this is an internal endpoint, a token or endpoint change may prevent readings from
working; use a supported GitHub API where possible.

## Build and flash

    pio run -t upload
    pio device monitor

## First-time setup

With no saved settings the device starts an access point `CopilotMeter-Setup`. The WPA2 password
is random each boot and shown on the OLED. Connect, open http://192.168.4.1, and enter the Wi-Fi
SSID/password, Copilot session token, account type (Personal or Business / Enterprise), and
monthly credit limit. The device saves the settings and reboots.

To wipe settings: let the device start normally, then press and hold BOOT for 3 seconds.
The OLED shows a reset prompt; release the button to cancel. Do not hold BOOT while powering on,
because that can put the ESP32 into its ROM download mode instead of starting the firmware.

## Credential storage

The Wi-Fi password and Copilot session token are saved in the ESP32's normal (unencrypted) NVS flash.
HTTPS to GitHub is verified against the Mozilla CA bundle, and the setup hotspot only runs when no
credentials are saved.

## Files

- `platformio.ini`, `src/main.cpp`
