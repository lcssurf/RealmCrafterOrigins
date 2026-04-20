package world

// Portal is a trigger volume that teleports a player to another area.
type Portal struct {
	// Trigger centre and radius (XZ plane only).
	X, Z   float32
	Radius float32

	// Destination.
	TargetArea                    string
	DestX, DestY, DestZ, DestYaw float32
}
