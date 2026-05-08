package nav

import (
	"encoding/binary"
	"fmt"
	"os"
)

const (
	navMeshMagic   uint32 = 0x52_4E_41_56 // "RNAV"
	navMeshVersion uint32 = 1
)

type Vec3 struct{ X, Y, Z float32 }

// NavPoly is one triangle in the navmesh.
// V[i] are indices into NavMesh.Verts.
// Neighbor[i] is the index of the adjacent poly sharing edge (V[i], V[(i+1)%3]),
// or -1 if there is no neighbor (boundary edge).
type NavPoly struct {
	V        [3]uint32
	Neighbor [3]int32
}

type NavMesh struct {
	Verts []Vec3
	Polys []NavPoly
}

// Load reads a .navmesh file from disk.
func Load(path string) (*NavMesh, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	rd := func(v any) error { return binary.Read(f, binary.LittleEndian, v) }

	var magic, version uint32
	if err := rd(&magic); err != nil {
		return nil, err
	}
	if magic != navMeshMagic {
		return nil, fmt.Errorf("navmesh: bad magic %08X", magic)
	}
	if err := rd(&version); err != nil {
		return nil, err
	}
	if version != navMeshVersion {
		return nil, fmt.Errorf("navmesh: unsupported version %d", version)
	}

	var numVerts uint32
	rd(&numVerts)
	verts := make([]Vec3, numVerts)
	for i := range verts {
		rd(&verts[i].X)
		rd(&verts[i].Y)
		rd(&verts[i].Z)
	}

	var numPolys uint32
	rd(&numPolys)
	polys := make([]NavPoly, numPolys)
	for i := range polys {
		rd(&polys[i].V[0])
		rd(&polys[i].V[1])
		rd(&polys[i].V[2])
		rd(&polys[i].Neighbor[0])
		rd(&polys[i].Neighbor[1])
		rd(&polys[i].Neighbor[2])
	}

	return &NavMesh{Verts: verts, Polys: polys}, nil
}

// Save writes the navmesh to a .navmesh file.
func (nm *NavMesh) Save(path string) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	wr := func(v any) error { return binary.Write(f, binary.LittleEndian, v) }

	wr(navMeshMagic)
	wr(navMeshVersion)
	wr(uint32(len(nm.Verts)))
	for _, v := range nm.Verts {
		wr(v.X); wr(v.Y); wr(v.Z)
	}
	wr(uint32(len(nm.Polys)))
	for _, p := range nm.Polys {
		wr(p.V[0]); wr(p.V[1]); wr(p.V[2])
		wr(p.Neighbor[0]); wr(p.Neighbor[1]); wr(p.Neighbor[2])
	}
	return nil
}
