"""
Generate Training Camp heightmap inspired by WYD layout:
- South (z~380-512): flat training ground, slight terrain variation
- Middle (z~200-380): gentle slope upward toward city
- North (z~0-200): Armia city plateau, slightly elevated
- East/West borders: gentle hills
"""
import struct, array, math, sys

COLS = ROWS = 512
CELL = 2.0  # world units per cell
data = array.array('f', [0.0] * (COLS * ROWS))

def set_h(cx, cz, h):
    if 0 <= cx < COLS and 0 <= cz < ROWS:
        data[cz * COLS + cx] = h

def smooth_terrain():
    """3x3 box blur."""
    out = array.array('f', data)
    for z in range(1, ROWS-1):
        for x in range(1, COLS-1):
            total = 0.0
            for dz in range(-1, 2):
                for dx in range(-1, 2):
                    total += data[(z+dz)*COLS + (x+dx)]
            out[z*COLS+x] = total / 9.0
    data[:] = out

def noise(x, z, scale=1.0, amp=1.0, octaves=4):
    """Simple deterministic noise via sin/cos."""
    v = 0.0
    freq = scale
    a = amp
    for o in range(octaves):
        v += math.sin(x * freq * 0.017 + o * 2.3) * math.cos(z * freq * 0.019 + o * 1.7) * a
        v += math.sin((x + z) * freq * 0.011 + o * 3.1) * a * 0.5
        freq *= 2.0
        a *= 0.5
    return v

# Base terrain pass
for cz in range(ROWS):
    wz = cz * CELL  # world Z
    for cx in range(COLS):
        wx = cx * CELL  # world X

        # Normalized coordinates [0,1]
        nx = wx / (COLS * CELL)  # 0=west 1=east
        nz = wz / (ROWS * CELL)  # 0=north 1=south

        # Base height bands:
        # North (nz<0.2): Armia plateau ~20-25 units
        # Middle (0.2<nz<0.5): slope down ~10-20 units
        # South (nz>0.5): Training camp ~5-12 units

        if nz < 0.20:
            base = 55.0 + (0.20 - nz) * 80.0  # Armia high plateau ~55-70u
        elif nz < 0.50:
            t = (nz - 0.20) / 0.30
            base = 55.0 - t * 40.0  # steep slope from 55 down to 15
        else:
            base = 15.0 - (nz - 0.50) * 10.0  # Training Camp ~10-15u

        base = max(3.0, base)

        # Edge hills (east/west borders) — tall natural walls
        edge_e = max(0, nx - 0.80) / 0.20
        edge_w = max(0, 0.20 - nx) / 0.20
        edge_n = max(0, 0.06 - nz) / 0.06
        edge_s = max(0, nz - 0.94) / 0.06
        edge = max(edge_e, edge_w) ** 2 * 80.0
        edge += max(edge_n, edge_s) ** 2 * 60.0
        base += edge

        # Add terrain noise — more pronounced
        n = noise(cx, cz, scale=1.0, amp=6.0, octaves=5)

        # Flatten the road corridor (cx near 256, middle Z band)
        road_dist = abs(cx - 256) / 20.0
        if road_dist < 1.0 and 0.25 < nz < 0.75:
            road_flat = 1.0 - road_dist
            n *= (1.0 - road_flat * 0.8)

        # Flatten the Training Camp interior
        tc_x_dist = abs(cx - 256) / 60.0
        tc_z_dist = abs(cz - 400) / 60.0
        if tc_x_dist < 1.0 and tc_z_dist < 1.0:
            tc_flat = (1.0 - tc_x_dist) * (1.0 - tc_z_dist)
            n *= (1.0 - tc_flat * 0.9)

        # Flatten Armia city interior
        arm_x = abs(cx - 256) / 85.0
        arm_z = abs(cz - 110) / 85.0
        if arm_x < 1.0 and arm_z < 1.0:
            arm_flat = (1.0 - arm_x) * (1.0 - arm_z)
            n *= (1.0 - arm_flat * 0.85)
            base += arm_flat * 3.0  # slight extra elevation for Armia

        h = base + n
        data[cz * COLS + cx] = h

# Blur 3x to smooth transitions
for _ in range(3):
    smooth_terrain()

# Write heightmap — format: magic(4) + W(4) + H(4) + cell_size(4) + heights(W*H*4)
CELL_SIZE = 2.0
path = '../client/data/areas/Training Camp/heightmap.bin'
with open(path, 'wb') as f:
    f.write(b'RCHM')
    f.write(struct.pack('<IIf', COLS, ROWS, CELL_SIZE))
    data.tofile(f)

mn = min(data)
mx = max(data)
print(f'Heightmap written: {COLS}x{ROWS}, height range [{mn:.2f}, {mx:.2f}]')
print(f'Path: {path}')

# Bump zone_dirty so GUE auto-reloads the terrain.
import sqlite3
con = sqlite3.connect('rco.db')
cur = con.cursor()
cur.execute('CREATE TABLE IF NOT EXISTS zone_dirty '
            '(area_name TEXT PRIMARY KEY, version INTEGER DEFAULT 0)')
cur.execute("INSERT INTO zone_dirty (area_name,version) VALUES ('Training Camp',1) "
            "ON CONFLICT(area_name) DO UPDATE SET version=version+1")
con.commit()
print('GUE will auto-reload terrain in ~1s.')
