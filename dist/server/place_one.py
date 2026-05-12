"""
place_one.py — place a single test object and auto-reload GUE.
Usage: python place_one.py <model_id> [sx] [sy] [sz] [yaw]

Example:
  python place_one.py 114 1.0 1.0 1.0 0
  python place_one.py 30 2.0 2.0 2.0 0

Places the object at the center of the map (512, terrain_height, 512).
GUE reloads automatically within ~1s.
"""
import sys, sqlite3, struct, array

AREA = 'Training Camp'
CON  = sqlite3.connect('rco.db')
CUR  = CON.cursor()

model_id = int(sys.argv[1]) if len(sys.argv) > 1 else 114
sx = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
sy = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
sz = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
yaw= float(sys.argv[5]) if len(sys.argv) > 5 else 0.0

x, z = 512.0, 512.0

# Sample terrain height
with open('../client/data/areas/Training Camp/heightmap.bin', 'rb') as f:
    f.read(4)
    cols, rows = struct.unpack('<II', f.read(8))
    f.read(4)  # cell_size
    heights = array.array('f', f.read(cols * rows * 4))

cx = min(cols-1, int(x / 2.0))
cz = min(rows-1, int(z / 2.0))
y  = heights[cz * cols + cx]

# Remove previous test objects (model_id match at center)
CUR.execute("DELETE FROM zone_scenery WHERE area_name=? AND model_id=? "
            "AND abs(x-512)<1 AND abs(z-512)<1", (AREA, model_id))

# Insert
CUR.execute(
    'INSERT INTO zone_scenery '
    '(area_name,model_id,material_id,x,y,z,pitch,yaw,roll,sx,sy,sz,'
    'collision,anim_mode,inv_size,ownable,locked) '
    'VALUES (?,?,0,?,?,?,0,?,0,?,?,?,0,0,0,0,0)',
    (AREA, model_id, x, y, z, yaw, sx, sy, sz))

# Bump zone_dirty
CUR.execute('CREATE TABLE IF NOT EXISTS zone_dirty '
            '(area_name TEXT PRIMARY KEY, version INTEGER DEFAULT 0)')
CUR.execute('INSERT INTO zone_dirty (area_name,version) VALUES (?,1) '
            'ON CONFLICT(area_name) DO UPDATE SET version=version+1', (AREA,))

CON.commit()

# Lookup model name
CUR.execute('SELECT name FROM media_models WHERE id=?', (model_id,))
row = CUR.fetchone()
name = row[0] if row else f'model_{model_id}'

print(f'Placed: {name} (id={model_id}) at (512,{y:.1f},512)')
print(f'Scale: sx={sx} sy={sy} sz={sz}  yaw={yaw}')
print('GUE will reload in ~1s.')
