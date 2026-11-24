# App Store Connect texts

What App Store Connect asks for, kept here so the words are the same each time.
The privacy policy is `docs/privacy-policy.md` (its address on GitHub is the one
to give).  The app requires an Apple TV 4K (2017 or later); say so wherever the
form has room, since the Info.plist cannot (`tvos-port.md`).

## TestFlight: Test Information

**Beta App Description**

> museld plays laserdiscs from RF captures.  A capture is the raw signal from the
> disc, made with a Domesday Duplicator or similar device; museld decodes it in
> real time and shows the picture and plays the sound.  It handles NTSC and PAL
> discs and the Japanese Hi-Vision MUSE discs, with analog, digital and AC3
> sound, closed captions and subtitle files.
>
> On the Apple TV the captures are read from a web server on your own network
> (a NAS or any machine running a web server).  A sample capture is included,
> so the app plays something without any server: it is the second line of the
> first screen.
>
> Requires an Apple TV 4K (2017 or later).

**Feedback Email**: staffan@ulfberg.se

**Beta App Review Information**: your name, phone and e-mail; *Sign-in
required*: no.

**Review Notes**

> museld decodes RF captures that users make of their own laserdiscs.  Nothing
> is downloaded from or shared with anyone, and there is no account.  The app is
> the Apple TV port of the open-source museld player for Linux, macOS and
> Windows, developed since 2021 at https://github.com/staffanu/museld, where
> the source, the documentation and the desktop releases are.
>
> To try the app: open it, choose "Play the sample capture (a Space Shuttle
> launch)" on the first screen, and the clip plays with sound.  The sample is
> inside the app: ten seconds of NASA mission report footage, a US government
> work, from a laserdisc.  Select on the remote pauses, left and right seek, up
> and down move between chapters, and the Menu button opens the settings over
> the picture.  Playing from a web server needs a capture file on the local
> network, which is why the app asks for local network access; the sample does
> not.
>
> The app requires an Apple TV 4K (2017 or later); on an Apple TV HD it shows a
> message saying so.
>
> RF capture is an established hobbyist format: the open-source ld-decode
> project (https://github.com/happycube/ld-decode) and the Domesday Duplicator
> capture hardware (https://www.domesday86.com/) are its tools, and public
> sample captures are at https://madeye.org/ld-captures/public/.

## Per build: What to Test

> Open the app and choose "Play the sample capture" on the first screen.  The
> launch should play at once, with sound (the English narration).  Then press
> Menu to look at the settings, and select to pause.  If you have captures on a
> web server, "Enter an address..." takes its address.

## The App Store listing, when it comes to that

- Name: museld.  Subtitle: Laserdisc player for RF captures.
- Category: Entertainment (or Utilities).
- Age rating: none of the listed content.
- Privacy: "Data Not Collected".
- Support URL: https://github.com/staffanu/museld.  Privacy policy URL:
  https://github.com/staffanu/museld/blob/master/docs/privacy-policy.md.
- Screenshots: 1920x1080 frames of the sample playing, the menu over the
  picture, and the browser's first screen (`--export-frame` with
  `MUSELD_EXPORT_OVERLAYS=1` on the desktop build renders the same picture).
- Copyright: Staffan Ulfberg.  The app is GPLv3, and Apple's terms are held to
  conflict with it, so before an App Store submission the author, as the sole
  copyright holder of museld's own code, should add an App Store exception to
  the license notice in the README (the libraries in the tvOS build -- SDL,
  MoltenVK, libFLAC, stb, miniaudio, cpp-httplib, Noto Sans -- are permissive).
  Not needed for TestFlight.
