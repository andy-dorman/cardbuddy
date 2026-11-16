# cardbuddy

[fm1buddy](https://github.com/konsumer/fm1buddy) for the [M5 Cardputer (ADV)](https://docs.m5stack.com/en/core/Cardputer-Adv): swap, back up and recover the M-VAVE FM-1's firmware, and manage its sounds, samples and songs, with a keyboard instead of a touchscreen. It's an app like any other for [M5 Launcher](https://bmorcelli.github.io/Launcher/), so the Cardputer can run other things when you don't need it.

It runs fm1buddy's own core (`lib/fm1core` there), so it does the same things the same way, and **the SD card is the same**: move a card between an fm1buddy and a cardbuddy and both see the same firmwares, backups, sounds, setups and SLOOP files.

| On the SD card | What                                                                      |
| -------------- | ------------------------------------------------------------------------- |
| `/fm1`         | firmwares (`.fwsc`)                                                       |
| `/backups`     | the FM-1's whole flash                                                    |
| `/syx`         | sounds (`.syx`, DX7 banks)                                                |
| `/setups`      | setups, fomajam's kits                                                    |
| `/sloop`       | SLOOP backups, `songs/`, `samples/`                                       |
| `/recordings`  | WAV recordings                                                            |
| `/midimap.txt` | MIDI mapping rules                                                        |
| `/fm1buddy`    | the Hub's list, install records, logs (`cardbuddy-log.txt` is this one's) |

## Using it

- `;` `.` move up and down, `,` `/` page (left and right), `enter` opens, `` ` `` goes back (`fn` + these while typing)
- In a firmware list, just type to find one; `tab` switches between the SD card's and the Hub's

## Where it's at

Working: the firmware lists (SD card and the Hub, with search), a firmware's details, downloading from the Hub over Wi-Fi, Wi-Fi setup, themes, battery.

Next: the USB host (the FM-1 on the USB-C port: installs, backups, recovery, sounds, SLOOP, MIDI), from fm1buddy's `src/s3`. The Cardputer has no PSRAM, which fm1buddy's board code leans on, so that part needs care. Then what the Cardputer is good at: the keyboard as a piano, tilt as MIDI CCs, previewing samples and recordings on its speaker, sampling from its mic.

## Building

With [PlatformIO](https://platformio.org), and fm1buddy beside this repo (`../fm1buddy`, for its core):

```sh
git clone https://github.com/konsumer/fm1buddy.git ../fm1buddy
pio run -t upload
```

`npm run fmt` formats it.

## License

GPL-3.0-only, as fm1buddy.
