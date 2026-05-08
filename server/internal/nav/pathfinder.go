package nav

import (
	"container/heap"
	"math"
)

// centroid returns the average position of a poly's three vertices.
func centroid(nm *NavMesh, polyIdx int) Vec3 {
	p := &nm.Polys[polyIdx]
	var c Vec3
	for _, vi := range p.V {
		c.X += nm.Verts[vi].X
		c.Y += nm.Verts[vi].Y
		c.Z += nm.Verts[vi].Z
	}
	c.X /= 3; c.Y /= 3; c.Z /= 3
	return c
}

func dist3(a, b Vec3) float32 {
	dx, dy, dz := a.X-b.X, a.Y-b.Y, a.Z-b.Z
	return float32(math.Sqrt(float64(dx*dx + dy*dy + dz*dz)))
}

// nearestPoly returns the index of the poly whose centroid is closest to p,
// or -1 if the navmesh is empty.
func (nm *NavMesh) NearestPoly(p Vec3) int {
	best, bestDist := -1, float32(math.MaxFloat32)
	for i := range nm.Polys {
		c := centroid(nm, i)
		if d := dist3(c, p); d < bestDist {
			best, bestDist = i, d
		}
	}
	return best
}

// --- A* priority queue ---

type aNode struct {
	poly   int
	g, f   float32
	parent int
}

type aPQ []*aNode

func (q aPQ) Len() int            { return len(q) }
func (q aPQ) Less(i, j int) bool  { return q[i].f < q[j].f }
func (q aPQ) Swap(i, j int)       { q[i], q[j] = q[j], q[i] }
func (q *aPQ) Push(x any)         { *q = append(*q, x.(*aNode)) }
func (q *aPQ) Pop() any           { old := *q; n := old[len(old)-1]; *q = old[:len(old)-1]; return n }

// FindPath returns a sequence of waypoints from `from` to `to` using A* over
// poly centroids, followed by a simple string-pull to remove unnecessary turns.
// Returns nil if no path exists.
func (nm *NavMesh) FindPath(from, to Vec3) []Vec3 {
	if len(nm.Polys) == 0 {
		return nil
	}
	startPoly := nm.NearestPoly(from)
	goalPoly  := nm.NearestPoly(to)
	if startPoly < 0 || goalPoly < 0 {
		return nil
	}
	if startPoly == goalPoly {
		return []Vec3{to}
	}

	n := len(nm.Polys)
	gScore := make([]float32, n)
	for i := range gScore { gScore[i] = math.MaxFloat32 }
	parent := make([]int, n)
	for i := range parent { parent[i] = -1 }
	inOpen := make([]bool, n)

	goalC := centroid(nm, goalPoly)

	gScore[startPoly] = 0
	pq := &aPQ{&aNode{poly: startPoly, g: 0, f: dist3(centroid(nm, startPoly), goalC)}}
	heap.Init(pq)
	inOpen[startPoly] = true

	for pq.Len() > 0 {
		cur := heap.Pop(pq).(*aNode)
		if cur.poly == goalPoly {
			// Reconstruct poly path.
			path := []int{cur.poly}
			for parent[path[len(path)-1]] >= 0 {
				path = append(path, parent[path[len(path)-1]])
			}
			// Reverse.
			for l, r := 0, len(path)-1; l < r; l, r = l+1, r-1 {
				path[l], path[r] = path[r], path[l]
			}
			return nm.stringPull(from, to, path)
		}
		poly := &nm.Polys[cur.poly]
		for _, nb32 := range poly.Neighbor {
			if nb32 < 0 { continue }
			nb := int(nb32)
			ng := cur.g + dist3(centroid(nm, cur.poly), centroid(nm, nb))
			if ng < gScore[nb] {
				gScore[nb] = ng
				parent[nb] = cur.poly
				f := ng + dist3(centroid(nm, nb), goalC)
				if !inOpen[nb] {
					heap.Push(pq, &aNode{poly: nb, g: ng, f: f})
					inOpen[nb] = true
				}
			}
		}
	}
	return nil // no path
}

// stringPull simplifies the poly-centroid path using a left/right funnel check.
// Simplified version: walks shared edges and picks midpoints, then removes
// collinear/redundant points.
func (nm *NavMesh) stringPull(from, to Vec3, polyPath []int) []Vec3 {
	if len(polyPath) == 0 {
		return nil
	}
	pts := []Vec3{from}
	for i := 1; i < len(polyPath); i++ {
		// Midpoint of the shared edge between polyPath[i-1] and polyPath[i].
		pa := &nm.Polys[polyPath[i-1]]
		pb := &nm.Polys[polyPath[i]]
		// Find the two vertices shared by pa and pb.
		var shared [2]Vec3
		found := 0
		for _, va := range pa.V {
			for _, vb := range pb.V {
				if va == vb {
					shared[found] = nm.Verts[va]
					found++
					if found == 2 { goto done }
				}
			}
		}
	done:
		if found == 2 {
			mid := Vec3{
				(shared[0].X + shared[1].X) * 0.5,
				(shared[0].Y + shared[1].Y) * 0.5,
				(shared[0].Z + shared[1].Z) * 0.5,
			}
			pts = append(pts, mid)
		}
	}
	pts = append(pts, to)
	return removeCollinear(pts)
}

// removeCollinear drops waypoints that are nearly on the line between their neighbours.
func removeCollinear(pts []Vec3) []Vec3 {
	if len(pts) <= 2 {
		return pts
	}
	const eps = 0.1
	out := []Vec3{pts[0]}
	for i := 1; i < len(pts)-1; i++ {
		a, b, c := pts[i-1], pts[i], pts[i+1]
		// Cross product magnitude of (b-a) × (c-a) on XZ plane.
		abx, abz := b.X-a.X, b.Z-a.Z
		acx, acz := c.X-a.X, c.Z-a.Z
		cross := abx*acz - abz*acx
		if math.Abs(float64(cross)) > eps {
			out = append(out, b)
		}
	}
	out = append(out, pts[len(pts)-1])
	return out
}
