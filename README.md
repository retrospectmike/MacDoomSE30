# MacDoomSE/30
<p style="text-align: center;"><img width="318" height="199" alt="image" src="https://github.com/user-attachments/assets/d7397cb2-80ae-482d-b90b-4949c053f18f" /> <img width="318" height="199" alt="image" src="https://github.com/user-attachments/assets/dec3a9fa-9ead-4a7d-b9cd-45a75337d49f" /> </p>

<p style="text-align: center;"><img width="318" height="199" alt="image" src="https://github.com/user-attachments/assets/912629a9-fb42-455e-966b-2211e93ef164" /> <img width="318" height="199" alt="image" src="https://github.com/user-attachments/assets/f0bf7e90-7459-4059-9f11-c1b46cef0e96" /> </p>

This is a port of [linuxdoom](https://github.com/id-Software/DOOM) to MacOS System 7 with the intention of playing on the 68030-based [Macintosh SE/30](https://en.wikipedia.org/wiki/Macintosh_SE/30).

## System Requirements
- Macintosh SE/30 (68030 processor)
- System 7.5.x with [MODE-32](https://en.wikipedia.org/wiki/MODE32) to access all your RAM
- 64MB of RAM
  - Doesn't currently need it all and may adjust this later
- Enough storage to fit the game and your .wad file(s)
  - I recommend [BlueSCSI](https://github.com/BlueSCSI/BlueSCSI-v2/)
- WAD file(s)
  - doom.wad / doom1.wad / doom2.wad / Tnt.wad / Plutonia.wad  etc.
  - You'll have to provide one or more of these yourself, placed in the same dir as the application file

## Background
### Why?
I wanted to play Doom on my Macintosh SE/30 and I wondered if it was possible.
### Is it Playable?
Yes.  At time of writing, the frame rate is reasonable for playing but additional work in not only performance but also black&white dithering and lighting settings might greatly enhance the appearance.  The current b&w appearance can at times make it difficult to play and at the moment - THERE IS NO SOUNDS YET!
### How?
Though I love to tinker with these vintage Macs (see my other repos), I wanted to do something bigger and I wanted to use AI to help.  So this was a natural project to tackle and practice using AI.  I used Claude Code to help plan / build / iterate the port.  It was very helpful and did most of the coding.  Is this cheating?  That's up to you to decide.  But the result is a playable Doom on the SE/30.  Was it literally feed it the linuxdoom repo and hit Go and wait a few hours and done?  **NO!**.  I messed with it daily trying to make adjustments and find + fix bugs, intelligently implement optimizations, fight it when things didn't work, switching models to try to be efficient with usage, making suggestions on what to try, and even calling out some of its more obv mistakes in code, etc.  Took several weeks of burning up my subscription tokens daily to get to this point.  Surely, I would have taken much much longer to do it myself -- and some things like 68k assembly I likely would never have had the time to learn and do.
### Why 64MB of RAM?
It's easy to upgrade the SE/30 to 64MB or even 128MB of RAM.  Much easier than it is to upgrade the CPU.  So all optimizations were done assuming memory is no object, but processing is scarce.
### What optimizations did you execute?
The file `PERFORMANCE_HISTORY.md` is a record of changes and, when recorded, performance of the various improvements along the way.  `OPTIMIZATION_IDEAS.md` also captures details on optimizations done, failed, and not done.  
The main ones you'll notice are:
- fog to reduce distance rendering work
- flat-filled floors
- Ultra-low quality "QUAD" mode
- Some rendering optimization that currently results in warping of wall edges when they are up close.  May try to fix in the future.
### Development Environment
- [Retro68](https://github.com/autc04/Retro68) is used to cross compile on my modern Mac
- Claude Code for handling all the code implementation
- [Basilisk II](https://basilisk.cebix.net/) for initial check that changes worked
- [Snow](https://snowemu.com/) emulator for real-hardware-like performance testing without needing to transfer files all the way to real target every time.  It works great and has generally produced close-to-actual performance numbers.  It also resolves a number of emulation-only issues that Basilisk II suffers from - namely encountered with a few 68k assembly code.
- On-target SE/30 testing for actual performance evaluation and of course real-world experience testing

## Special Thanks
- TinkerDifferent.com
- The Macintosh SE/30 team
- Steve Wozniak - because why not
- Susan Kare - because she'd appreciate it
- The team behind the Snow emulator, BlueSCSI, etc.
- Users like you - who keep these machines booping and das lights blinkin'!
- Peace, love, tolerance, and meditation in whatever flavor or names that float your boat!!!

# Careware
This program is care-ware. If you enjoy it, do something nice to someone today!

# Release Notes
• 1.0b3
- Updated Options screen to enable modification of Half Line and Affine options
  - Half line OFF is highly recommended  for Full Screen to avoid really low-quality visuals
• 1.0b2
- Actual Initial public release
- Added missing wad file check
- Updated to better doom.cfg included distro config
• 1.0b1
- Initial public release
- 'Playable' on the SE/30 with Default options settings
- No sounds yet
