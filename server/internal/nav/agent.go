package nav

import "math"

// Agent follows a path computed by NavMesh.FindPath.
// Tick() advances position toward the current waypoint and returns the new position.
type Agent struct {
	Waypoints   []Vec3
	waypointIdx int
	Speed       float32 // world units per second
	Arrived     bool
}

const arrivalRadius = float32(0.3)

// RequestPath computes a new path from `from` to `to` and resets the agent.
// Returns false if no path was found (agent stays put).
func (a *Agent) RequestPath(nm *NavMesh, from, to Vec3) bool {
	path := nm.FindPath(from, to)
	if path == nil {
		return false
	}
	a.Waypoints   = path
	a.waypointIdx = 0
	a.Arrived     = false
	return true
}

// Tick advances the agent along its path by dt seconds.
// Returns the new position; if no path is set returns current.
func (a *Agent) Tick(dt float32, current Vec3) Vec3 {
	if a.Arrived || len(a.Waypoints) == 0 {
		a.Arrived = true
		return current
	}

	wp := a.Waypoints[a.waypointIdx]
	dx := wp.X - current.X
	dz := wp.Z - current.Z
	d2 := dx*dx + dz*dz

	if d2 <= arrivalRadius*arrivalRadius {
		a.waypointIdx++
		if a.waypointIdx >= len(a.Waypoints) {
			a.Arrived = true
			return Vec3{wp.X, wp.Y, wp.Z}
		}
		wp = a.Waypoints[a.waypointIdx]
		dx = wp.X - current.X
		dz = wp.Z - current.Z
		d2 = dx*dx + dz*dz
	}

	dist := float32(math.Sqrt(float64(d2)))
	step := a.Speed * dt
	if step > dist { step = dist }

	nx := current.X + (dx/dist)*step
	nz := current.Z + (dz/dist)*step
	// Y is interpolated linearly toward waypoint Y.
	ny := current.Y + (wp.Y-current.Y)*(step/dist)
	return Vec3{nx, ny, nz}
}

// Reset clears the path and marks the agent as not arrived.
func (a *Agent) Reset() {
	a.Waypoints   = nil
	a.waypointIdx = 0
	a.Arrived     = false
}

// HasArrived returns true when the agent has reached the last waypoint.
func (a *Agent) HasArrived() bool { return a.Arrived }
