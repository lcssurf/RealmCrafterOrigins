#pragma once
// Compatibility shim — Actor moved to shared/renderer/ so every viewport
// (client, GUE Media preview, GUE Zone editor) can reuse the same class.
// Keep this header so existing "#include renderer/actors/actor.h" sites
// don't need changing.
#include "rco/renderer/actor.h"
