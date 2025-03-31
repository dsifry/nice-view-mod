# nice-view-mod with Bongo Cat, including FURIOUS MODE

This is a copy of the nice!view shield from the official ZMK firmware as a ZMK module that changes the shields to include a bongo cat animation on the main board, and allows for new artwork on the peripheral board.
As provided, it should function exactly like the current nice!view shield present in the ZMK firmware.

Bongo Cat starts off slowly when you are typing slowly, animating on each keypress and key release. But when you really get going, Bongo Cat goes into FURIOUS mode, just banging away on the keys, and then when you slow down/stop, Bongo Cat rests. 

This module is meant to be added to an existing forked customized keymap repo like [this one for the Urchin board](https://github.com/duckyb/zmk-urchin) or [this one for the Chocofi](https://github.com/beekeeb/zmk-config-corne-chocofi-with-niceview) with build actions set up to build your firmware with github actions and is of course meant for boards with the nice!view. Please check for such forkable repos for your board if you do not currently have one.

This repo is meant only to serve as a starting point for customization such as adding your own art or animations. Simply fork this repo and make the edits you would like and you can easily share your customizations and animations with others as they can easily include your module in thier firmware builds with only a few changes to thier west file and build.yaml.

For a complete example of forking and editing this repo to make a custom nice_view shield with custom animations, check out [https://github.com/GPeye/urchin-peripheral-animation](https://github.com/GPeye/urchin-peripheral-animation)

Replace the url-base and shield name below with whatever you customize in your fork.

## Usage

To use this module, first add it to your config/west.yml by adding a new entry to remotes and projects:

```yml
manifest:
  remotes:
    # zmk official
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: dsifry #new entry
      url-base: https://github.com/dsifry #new entry
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: nice-view-mod #new entry
      remote: dsifry #new entry
      revision: main #new entry
  self:
    path: config
```

This will use the latest code from the main branch. To use version 1.0, use this:

```yml
manifest:
  remotes:
    # zmk official
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: dsifry #new entry
      url-base: https://github.com/dsifry
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: nice-view-mod
      remote: dsifry 
      revision: v1.0 # new entry for v1.0
  self:
    path: config
```

Now simply swap out the default nice_view shield on the board for the custom one in your build.yaml file.

```yml
---
include:
  - board: nice_nano_v2
    shield: urchin_left nice_view_adapter  nice_view_custom #custom shield
  - board: nice_nano_v2
    shield: urchin_right nice_view_adapter nice_view_custom #custom shield
```
