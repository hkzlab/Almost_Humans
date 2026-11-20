# Almost Humans

![The Humans logo](pics/the_humans_title.png)

## Introduction 

'Almost Humans' is a reverse engineered port of the DOS CD version of "The Humans" by Imagitec Design, 1992.

The game is a mix between platform and puzzle, with some distant hints of "Lemmings". You can read about it on [Wikipedia](https://en.wikipedia.org/wiki/The_Humans_(video_game)).

![Screenshot of the SDL3 port, winter level](pics/almost_humans_sdl3.png)

You can still buy it on [GOG](https://www.gog.com/en/game/the_humans_bundle) for a few €/$/whatever. 

## 🤖 Disclaimer 🤖

Work on this port was started as an experiment to check the usability of a cheap open-weight LLM in my reverse-engineering workflows (See the section "[Tools](#tools)" below).

I usually don't release my experiments nor LLM written code, but given that

- this ended up quite playable
- I don't like wasting effort
- I'm having fun cleaning it up
- I plan to keep improving it

I decided to release it for everyone to use.

But I respect and understand the choice of people wishing to steer clear of code that has ties to LLM, so I want to be clear with this:

- **Reverse engineering** of the original DOS executable was **aided by an LLM**
- **Some of the code was written by an LLM**, some by me, most was re-worked and cleaned up by me
- The architecture of the port is mine (and took some hints from codebases I worked on in the past: ScummVM, Reminiscence, RAW...)

## SDL3 port 

**TODO**

### Playing

**TODO**

### Building

**TODO**

### TODO

- Full test playthrough
- Improve extended viewport mode (background graphics?)
- Additional video modes (e.g. no aspect ratio correction)
- Controller support
- Refactor the video plane model (see core's [video.c](src/core/video.c)) to remove the static 256KiB used to model video planes and decide who owns the logic around it

## Credits

I directly took code out of these projects:

- [DOSBox staging](https://github.com/dosbox-staging/dosbox-staging): Took a good chunk of the speaker emulation from them
- [Nuked-OPL3-fast](https://github.com/tgies/Nuked-OPL3-fast): The AdLib emulation uses this library

## Tools

- [Ghidra](https://github.com/NationalSecurityAgency/ghidra): The reverse engineering suite
- [GhidrAssistMCP](https://github.com/symgraph/GhidrAssistMCP): MCP server I used to allow access to an LLM
- [DOSBox Staging](https://github.com/dosbox-staging/dosbox-staging): Self-built version with debugger to manually single-step through pieces of code
- [dosbox-automation](https://github.com/dosbox-automation/dosbox-automation/) combined with [dosbox-mcp](https://github.com/dosbox-automation/dosbox-mcp) to allow the LLM access to the live game and its memory
- [VSCodium](https://github.com/VSCodium/vscodium): My editor of choice
- [DeepSeek v4.1 Flash](https://platform.deepseek.com)
- [opencode](https://github.com/anomalyco/opencode/)
