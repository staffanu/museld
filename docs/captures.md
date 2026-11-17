# Captures

Where the laserdisc captures that museld is developed and tested against live, and how a
machine gets the working set of clips.

The captures are kept in one place, Staffan's NAS, mounted at `/Volumes/video/LD-Captures`
on the Macs at home and at `/nas/video/LD-Captures` on multivac (which also serves
`public/` as `https://madeye.org/ld-captures/public/`).  **The NAS is reachable from the
home network only**: `tools/captures-sync.sh` and every path below that starts with
`/Volumes/video` or `/nas/video` mean nothing elsewhere.  Away from home, and on any other
machine, the regression suite fetches its clips from the public web address by itself
(`tests/regression/regress.py`, see `docs/regression-testing.md`), and the curated clips
are whatever `captures-sync.sh` last pulled into `~/data/LD-Captures/clips`.

## Layout on the NAS

- `NTSC/`, `PAL/`, `MUSE/` -- whole sides only, one directory per disc, one file per side
  (`<disc>-side<n>.<player>.<standard>-rf.<rate>.ldf`, or the capture tool's own name
  where it was received that way); `CATALOGUE.md` at the top of the share says for each
  which player and ADC, and what the capture is good for.  A disc captured on several
  players or at several rates has a subdirectory per capture.  A player's decoded MUSE
  baseband is a side too (`...muse-baseband.16.2MHz.ldf`, beside the RF of the same disc).
- `excerpts/<standard>/<Disc>/` -- the partial captures, seconds to a couple of minutes,
  kept apart so the whole sides can be browsed on their own.  They are a holding area:
  once the stretches a capture was kept for are in `clips/` or the regression suite, it
  can go.
- `clips/` -- the curated collection: short cuts of the stretches worked on and returned
  to, `.ldf` of a few seconds to a minute, each described in `clips/MANIFEST.toml` (source
  capture, position, length, format, what it shows).  This is the set every machine mirrors.
- `public/museld-testdata/` -- the regression suite's clips and goldens, served on the web;
  the suite fetches from there, so they stay where they are.  The manifest lists them as
  pointers, not copies.
- `derived/` -- not captures, but worth keeping: ld-decode's outputs of the Abyss
  capture, the old ldaudio experiment files, a rendered demo.  What belongs to one disc
  stays beside its capture (the CD rip that is the Jurassic Park CD capture's truth, the
  subtitle files of the MUSE discs).

`CATALOGUE.md` at the top of the share says for each
  which player and ADC, and what the capture is good for.  A disc captured on several
  players or at several rates has a subdirectory per capture (`EmpireStrikesBack/d515/`,
  `.../v4400/`).  A single-file capture gets a directory of its own too, so the rule
  stays one.  The MUSE discs sit under `MUSE/`.
- `clips/` -- the curated collection: short cuts of the stretches worked on and returned
  to, `.ldf` of a few seconds to a minute, each described in `clips/MANIFEST.toml` (source
  capture, position, length, format, what it shows).  This is the set every machine mirrors.
- `public/museld-testdata/` -- the regression suite's clips and goldens, served on the web;
  the suite fetches from there, so they stay where they are.  The manifest lists them as
  pointers, not copies.
- `derived/` -- not captures, but worth keeping: ld-decode's outputs of the Abyss
  capture, the old ldaudio experiment files, a rendered demo.  What belongs to one disc
  stays beside its capture (the CD rip that is the Jurassic Park CD capture's truth, the
  subtitle files of the MUSE discs).

`CATALOGUE.md` at the top of the share is the index of all of it, and
`clips/MANIFEST.toml` of the clips.

## The working set on a machine

```
tools/captures-sync.sh            # pulls LD-Captures/clips to ~/data/LD-Captures/clips
tools/captures-sync.sh --dry-run  # shows what it would copy
```

It is rsync over the mounted share, so it needs the mount: at home, or on multivac (where
it uses the local path).  A full capture is never mirrored; cut a clip from it, add it to
the manifest on the NAS, and let the sync bring it over.

Cutting a clip (ldconv, byte-exact, with the seek and length in seconds):

```
ldconv -s 22s -n 50s /Volumes/video/LD-Captures/MUSE/TopGun/top-gun.side1.x9.muse-rf.60MHz.ldf \
       /Volumes/video/LD-Captures/clips/muse-top-gun-x9-wobble-22s-50s.ldf
```
