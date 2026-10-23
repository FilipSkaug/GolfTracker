# GolfTracker
bruke et roterende display med esp32 til å enkelt tracke golfrundene mine

Score tracking on the Elecrow CrowPanel 1.28" HMI ESP32 Rotary Display (ESP32-S3).
Firmware lives in [Hardware/](Hardware/) (PlatformIO).

## Flash it

```sh
cd Hardware
pio run -t upload
```

## On the course

| Screen | Turn | Press | Hold |
|---|---|---|---|
| Home | Resume round / Select course / My rounds / Phone sync | open | – |
| My rounds | scroll rounds (newest first) | open scorecard | back |
| Scorecard | previous / next round | back to list | home |
| Select course | scroll all courses | pick | back |
| Holes | 18 holes / Front 9 / Back 9 (9-hole course: 9 / 18 = twice round) | start | back |
| Hole | strokes (starts at par) | save hole → next hole | menu: previous hole, finish early, discard |
| Round saved | – | phone sync | home |
| Phone sync | – | stop Wi-Fi, home | – |

The round is saved to flash after every hole, so a reset or flat battery never loses it — pick
**Resume round** on the home screen.

## Built-in courses

Installed once on first boot ([default_courses.h](Hardware/src/default_courses.h)); edit or delete
them from the phone like any other course.

| Course | Holes | Par | Par source |
|---|---|---|---|
| Byneset Nord | 18 | 72 | bynesetgolf.no |
| Byneset Sør | 9 | 32 | GolfPass |
| Trondheim GK Sommerseter | 18 | 72 | Offcourse, Hole19 |
| Asker Golfklubb | 18 | 70 | askergolf.no |
| Bærum Golfklubb | 18 | 71 | bmgk.no |
| Saurines de la Torre | 18 | 72 | AllSquare (GolfPass differs on holes 1, 9, 18) |
| New Sierra Golf | 18 | 72 | Golf4Holland, newsierragolf.com |
| La Serena Golf | 18 | 72 | GolfPass, Golf4Holland |
| Lo Romero Golf | 18 | 72 | Golfify, GolfPass |

## Phone sync (courses + scores)

1. Choose **Phone sync** (or press on the "Round saved" screen).
2. On the iPhone, join Wi-Fi **GolfTracker** (password `golf1234`).
3. Open **http://golf.local** (or http://192.168.4.1) in Safari.

- **Courses** tab: search for a course and import its pars, or add one manually (name, 9/18 holes,
  par per hole). Courses are stored on the display.
- **Rounds** tab: scorecards for every round, *Copy scores* for pasting, *Delete*.
- **Settings** tab: GolfCourseAPI key and optional home Wi-Fi.

### Course search

Course data comes from [GolfCourseAPI](https://golfcourseapi.com) (30,000+ courses). Sign up for a
free key (35 requests/day; one import = search + details = 2 requests) and paste it under Settings.
The phone calls the API directly, so it needs internet. The easiest way to get that is to add your
home Wi-Fi under Settings: during sync the display joins it too, and you open `golf.local` without
leaving your normal network.

Tip: in Safari, Share → *Add to Home Screen* gives the page an app icon.

## GolfBox auto-upload

GolfBox has no public API, so a GitHub Action ([.github/workflows/golfbox.yml](.github/workflows/golfbox.yml),
script [golfbox/submit.py](golfbox/submit.py)) logs in to golfbox.no with a headless browser and
fills in **Score → Innlever score**: date, course/tee, marker (searched by *Medlemsnavn*) and the
strokes per hole, then presses **Lagre**. Your marker still approves the round in GolfBox.

Flow: finish the round → press on "Round saved" → Phone sync joins your hotspot/home Wi-Fi →
the display sends unsent rounds to GitHub (sync screen shows `GolfBox: 1 sent`) → the Action runs.
Failed runs show up in the repo's **Actions** tab (GitHub emails you).

### One-time setup

1. **Secrets** (Settings → Secrets and variables → Actions → *Secrets*): `GOLFBOX_USERNAME`,
   `GOLFBOX_PASSWORD`, and `GOLFBOX_MARKER` = the marker's name as GolfBox shows it.
2. **Variable** (same page → *Variables*): `GOLFBOX_DRY_RUN` = `true` while testing.
3. **Test run**: Actions → *GolfBox upload* → *Run workflow*, paste a round such as
   `{"course":"Byneset Nord","par":[5,3,4,4,4,3,5,3,4,5,4,4,3,4,5,4,4,4],"strokes":[5,3,4,4,4,3,5,3,4,5,4,4,3,4,5,4,4,4]}`
   with *dry run* ticked, then read the log. When it looks right, set `GOLFBOX_DRY_RUN` = `false`.
4. **Token for the display**: GitHub → Settings → Developer settings → Fine-grained tokens → new
   token for this repository only, permission *Contents: Read and write*. Paste it on the phone page
   under Settings → *GolfBox auto-upload*.
5. **Course mapping**: [golfbox/courses.json](golfbox/courses.json) maps display course names to
   GolfBox's Klubb / Bane / Utslagssted texts. Byneset Nord is taken from GolfBox; the others are
   guesses. If one is wrong, the run fails and its log lists the options GolfBox offers.

Public repo: secrets are always hidden, but run logs are public and show each round's course,
strokes and date. Screenshots (which show your GolfBox profile) are only uploaded when the repo is
private, so in a public repo you debug from the text log.

Notes: the display has no clock, so the play date is the day the round is sent and the tee time
defaults to ~4 hours earlier. Automated logins may be against GolfBox's terms, and a site change
can break the script.

## Tuning

See [Hardware/src/config.h](Hardware/src/config.h): encoder direction/sensitivity, Wi-Fi name and
password, screen dim timeout, LED brightness.
