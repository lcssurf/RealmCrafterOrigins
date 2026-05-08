package world

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
)

const heightmapMagic uint32 = 0x4D484352 // "RCHM"

type Heightmap struct {
	w, h     int
	cellSize float32
	data     []float32
}

func LoadHeightmap(path string) (*Heightmap, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	var magic, w, h uint32
	var cellSize float32
	if err := binary.Read(f, binary.LittleEndian, &magic); err != nil {
		return nil, err
	}
	if magic != heightmapMagic {
		return nil, fmt.Errorf("heightmap: bad magic %08X", magic)
	}
	binary.Read(f, binary.LittleEndian, &w)
	binary.Read(f, binary.LittleEndian, &h)
	binary.Read(f, binary.LittleEndian, &cellSize)

	data := make([]float32, w*h)
	if err := binary.Read(f, binary.LittleEndian, data); err != nil {
		return nil, err
	}
	return &Heightmap{w: int(w), h: int(h), cellSize: cellSize, data: data}, nil
}

func (hm *Heightmap) SampleWorld(x, z float32) float32 {
	fx := x / hm.cellSize
	fz := z / hm.cellSize
	ix := int(math.Floor(float64(fx)))
	iz := int(math.Floor(float64(fz)))
	tx := fx - float32(ix)
	tz := fz - float32(iz)
	h00 := hm.get(ix, iz)
	h10 := hm.get(ix+1, iz)
	h01 := hm.get(ix, iz+1)
	h11 := hm.get(ix+1, iz+1)
	return lerp(lerp(h00, h10, tx), lerp(h01, h11, tx), tz)
}

func (hm *Heightmap) get(x, z int) float32 {
	if x < 0 {
		x = 0
	} else if x >= hm.w {
		x = hm.w - 1
	}
	if z < 0 {
		z = 0
	} else if z >= hm.h {
		z = hm.h - 1
	}
	return hm.data[z*hm.w+x]
}

func lerp(a, b, t float32) float32 { return a + (b-a)*t }
