#include "rco/physics/collision_world.h"

// CollisionWorld (collision_world.h) is a pure aggregate of reference
// members — no methods to implement yet. This translation unit exists so
// the header is compiled standalone at least once (catches missing-include
// / self-containment mistakes) and so rco_physics's source list isn't
// empty, ready for Phase 1+ (capsule sweep) to add real code here.
