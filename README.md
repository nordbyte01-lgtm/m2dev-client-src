# Client Source Repository

This repository contains the source code necessary to compile the game client executable.

## How to build

> cmake -S . -B build
>
> cmake --build build

---

## 📋 Changelog

### 🐛 Bug Fixes
* **Amun's freeze on drag window**: Fixed a bug where the client window would freeze while we are dragging it around.
* **Shaman Mounted Combat:** Fixed a bug that wrongly calculated Shaman characters mounted hits that didn't collide with the target to cause damage when attack speed was had an extremely high value.
* **Invisibility VFX Logic:** Fixed an issue where skill visual effects remained visible to the character while under the `AFFECT_INVISIBLE` state.
* **Buff Effects Visibility Recovery:** Fixed an issue where buff skill visual effects remained invisible if the skill was cast while the character was under the `AFFECT_INVISIBLE` state.
* **Casting Speed Cooldowns:** Fixed an issue where Casting Speed was not properly calculated in skill cooldowns. The system now supports real-time calculation updates whenever the bonus value changes.
