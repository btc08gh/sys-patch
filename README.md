# sys-patch

A script-like system module that patches fs, es and ldr on boot.

---

## Config

sys-patch features a simple config. This can be manually editied or updated using the overlay.

the config file can be found in `/config/sys-patch/config.ini`, if the file does not exist, the file will be created when sys-patch is run.

```ini
[options]
patch_sysmmc=1 ; 1=(default) patch sysmmc, 0=don't patch sysmmc
patch_emummc=1 ; 1=(default) patch emummc, 0=don't patch emummc
logging=1      ; 1=(default) output /config/sys-patch/log.inim 0=no log
```

---

## Overlay

the overlay can be used to change the config options and to see what patches are applied (if any).

- Unpatched means the patch wasn't applied (likely not found).
- Patched (green) means it was patched by sys-patch.
- Patched (yellow) means it was already patched, likely by sigpatches or a custom atmosphere build.

<p float="left">
  <img src="https://i.imgur.com/IlTkkYM.jpg" width="400" />
  <img src="https://i.imgur.com/T4K5u5f.jpg" width="400" />
</p>

---

## Building

### prerequisites
- install devkitpro

```sh
git clone --recurse-submodules https://github.com/ITotalJustice/sys-patch.git
cd sys-patch
make
```

---

## What is being patched?

Here's a quick run down of what's being patched

- fs
- es
- ldr

fs and es need new patches after every new fw version.

ldr on the other hand needs new patches after every new atmosphere release. this is due to ldr service being reimplemented by atmosphere. in fw 10.0.0, a new check was added to ofw which we needed to patch out. As atmosphere closely follows what ofw does, it also added this check. This is why a new patch is needed per atmosphere update.

---

## How does it work?

it uses a collection of patterns to find the piece of code to patch. alternatively, it could just use offsets, however this would mean this tool would have to be updated after every new fw update, that's not ideal.

the patches are applied at boot, then, the sysmod stops running. the memory footpint of the sysmod is very very small, only using 16kib in total. the size of the binary itself is only ~50kib! this doesnt really mean much, but im pretty proud of it :)

---

## Does this mean i should stop downloading / using sigpatches?

No, i would personally recommend continuing to use sigpatches. Reason being is that should this tool ever break, i likely wont be quick to fix it.

---

## If i am using sigpatches already, is there any point in using this as well?

Yes, in 2 niche cases.

1. A new ldr patch needs to be created after every atmosphere update. Sometimes, a new silent atmosphere update is released. This tool will always patch ldr without having to update patches.

2. Building atmosphere from src will require you to generate a new ldr patch for that custom built atmosphere. This is easy enough due to the public scripts / tools that exist out there, however this will always be able to

Also, if you forget to update your patches when you update fw / atmosphere, this sysmod should be able to patch everything just fine! so it's nice to have as a fallback.

---

## My request

This repo is mainly a proof of concept. I would love for someone to build upon this, make it into something bigger / better.

here are a few ideas that i have:
- option to load new patterns from file
- make homebrew frontend that can update this sysmod, apply patches, all without having to reboot

---

## Credits / Thanks

software is built on the shoulders of giants. this tool wouldn't be possible wthout these people:

- DarkMatterCore
- MrDude
- BornToHonk (farni)
- TeJay
- ArchBox
- Shchmue (lockpick)
- Jakibaki (sys-netcheat)
- SciresM (Atmosphere, hactool, etc)
- Switchbrew (libnx, switch-examples)
- DevkitPro (toolchain)
- [minIni](https://github.com/compuphase/minIni)
- [libtesla](https://github.com/WerWolv/libtesla)
