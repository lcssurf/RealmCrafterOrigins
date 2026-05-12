"""
Training Camp + Armia — WYD-inspired layout
Map: 512x512 cells @ cell=2 = 1024x1024 world units

Layout (world Z):
  z=820-980  Training Camp sul — área de spawn, muralha externa
  z=600-820  Zona de treinamento — campo aberto, obstáculos
  z=380-600  Estrada principal — transição, ruínas, lanternas
  z=80-380   Armia — cidade completa com muralha, distritos, castelo, catedral

Wall rotation fix:
  Paredes correndo no eixo X (leste-oeste): yaw=90
  Paredes correndo no eixo Z (norte-sul):   yaw=0
"""
import sqlite3, sys, math, struct, array
sys.stdout.reconfigure(encoding='utf-8')

con = sqlite3.connect('rco.db')
cur = con.cursor()
cur.execute("DELETE FROM zone_scenery WHERE area_name='Training Camp'")

AREA = 'Training Camp'
objects = []

SCALES = {
    114: (0.0675, 0.0,  0),   # wall_clean
    113: (0.0675, 0.0,  0),   # wall_broken
    30:  (0.015,  0.0,  0),   # castle_wall_LOD01
    105: (0.0075, 5.4,  0),   # tower_LOD01       pivot center
    110: (0.0075, 5.4,  0),   # twotowers         pivot center
    64:  (0.005,  7.2,  0),   # keep              pivot center
    80:  (0.004,  3.6,  0),   # portcullis        pivot center
    22:  (0.006,  5.4, 90),   # bridgegathouse    pitch=90
    59:  (0.02,   0.0,  0),   # houses_single
    52:  (0.02,   2.0,  0),   # houses_double     pivot above
    54:  (0.02,   0.0,  0),   # houses_fourwindow
    56:  (0.02,   2.7,  0),   # houses_overhang   pivot center
    12:  (0.01,   0.9,  0),   # barrel_closed     pivot center
    15:  (0.01,   0.9,  0),   # barrel_open
    9:   (0.01,   0.9,  0),   # barrel_broken
    84:  (0.01,   0.0,  0),   # sack01
    29:  (0.01,   0.0,  0),   # cart
    33:  (0.01,   0.0,  0),   # chest
    75:  (0.01,   0.0,  0),   # logs_open
    72:  (0.01,   0.0,  0),   # logs_covered
    36:  (0.01,   0.0,  0),   # chopping_block
    27:  (0.01,   0.0,  0),   # bucket
    100: (0.01,   0.0,  0),   # straw_bail
    119: (0.2,    0.0,  0),   # windmill
    108: (0.025,  9.0,  0),   # trebuchet         pivot center
    # Cathedral (correct from Unreal)
    129: (1.0, 0.0, 0),   # SM_Arch
    130: (1.0, 0.0, 0),   # SM_Arch_big
    140: (1.0, 0.0, 0),   # SM_Cathedral_Wall
    167: (1.0, 0.0, 0),   # SM_Wall_foundation
    142: (1.0, 0.0, 0),   # SM_Entrance
    137: (1.0, 0.0, 0),   # SM_Cathedral_roof
    165: (1.0, 0.0, 0),   # SM_Tower_top
    149: (1.0, 0.0, 0),   # SM_Pillar_cantonne
    162: (1.0, 0.0, 0),   # SM_Stairs
    141: (1.0, 0.0, 0),   # SM_Door
    166: (1.0, 0.0, 0),   # SM_Wall_2
    148: (1.0, 0.0, 0),   # SM_Parapet
    150: (1.0, 0.0, 0),   # SM_Pillar_square
    152: (1.0, 0.0, 0),   # SM_Pinnacle
    133: (1.0, 0.0, 0),   # SM_Arch_wall
    138: (1.0, 0.0, 0),   # SM_Cathedral_roof_spire
    154: (1.0, 0.0, 0),   # SM_Roof_Nave
    163: (1.0, 0.0, 0),   # SM_Stairs_corner
}

for mid, (s, yo, pitch) in SCALES.items():
    cur.execute('UPDATE media_models SET scale=? WHERE id=?', (s, mid))

def add(mid, x, z, yaw=0, yo_override=None, pitch_override=None):
    s, yo, pitch = SCALES.get(mid, (1.0, 0.0, 0))
    if yo_override is not None: yo = yo_override
    if pitch_override is not None: pitch = pitch_override
    objects.append((mid, float(x), float(z), float(yaw), s, yo, pitch))

with open('../client/data/areas/Training Camp/heightmap.bin', 'rb') as f:
    f.read(4)
    cols, rows = struct.unpack('<II', f.read(8))
    f.read(4)
    heights = array.array('f', f.read(cols * rows * 4))

def gy(wx, wz):
    cx = max(0, min(cols-1, int(wx / 2.0)))
    cz = max(0, min(rows-1, int(wz / 2.0)))
    return heights[cz * cols + cx]

# ════════════════════════════════════════════════════════════════════════
# MURO helper: yaw correto por orientação
#   leste-oeste (percorre X): yaw=0
#   norte-sul   (percorre Z): yaw=90
# ════════════════════════════════════════════════════════════════════════
def wall_row(mid, x_start, x_end, z, step=14, yaw=0):
    x = x_start
    while x <= x_end:
        add(mid, x, z, yaw=yaw)
        x += step

def wall_col(mid, z_start, z_end, x, step=14, yaw=90):
    z = z_start
    while z <= z_end:
        add(mid, x, z, yaw=yaw)
        z += step

# ════════════════════════════════════════════════════════════════════════
# 1. MURALHA EXTERNA DO TREINAMENTO (z=820-980, x=280-744)
#    Representa o limite externo do campo de treinamento — grande e aberta
# ════════════════════════════════════════════════════════════════════════
TC_X1, TC_X2 = 280.0, 744.0
TC_Z1, TC_Z2 = 820.0, 978.0
TC_CX = (TC_X1 + TC_X2) / 2

# Paredes
wall_row(114, TC_X1, TC_X2, TC_Z2)              # sul
wall_row(114, TC_X1+30, TC_X2-30, TC_Z1)        # norte (gap para entrada)
wall_col(114, TC_Z1, TC_Z2, TC_X1)              # oeste
wall_col(114, TC_Z1, TC_Z2, TC_X2)              # leste

# Torres nos cantos
for tx, tz in [(TC_X1, TC_Z1),(TC_X2, TC_Z1),(TC_X1, TC_Z2),(TC_X2, TC_Z2)]:
    add(105, tx, tz)

# Portão norte (entrada para zona de treinamento)
add(110, TC_CX, TC_Z1)
add(80,  TC_CX, TC_Z1)

# Props de spawn / área de entrada
for i in range(5):
    x = TC_CX - 80 + i * 40
    add(100, x, TC_Z2 - 30)              # fardos de palha
    add(84,  x + 12, TC_Z2 - 45)         # sacos

add(29,  TC_CX - 100, TC_Z2 - 60)        # carroça
add(72,  TC_CX + 90,  TC_Z2 - 55)        # toras cobertas
add(75,  TC_CX + 110, TC_Z2 - 70)        # toras abertas
add(33,  TC_CX,       TC_Z2 - 80)        # baú (drops do tutorial)

# ════════════════════════════════════════════════════════════════════════
# 2. ZONA DE TREINAMENTO (z=600-820)
#    Campo aberto com obstáculos, postos de treino
# ════════════════════════════════════════════════════════════════════════

# Fileiras de chopping blocks (dummies de treino)
for row in range(3):
    for col in range(6):
        x = 350 + col * 45
        z = 780 - row * 60
        add(36, x, z)                      # chopping block
        add(100, x + 10, z + 8)            # fardo ao lado

# Barris de treino espalhados
for i in range(8):
    angle = i * 45
    x = TC_CX + 120 * math.cos(math.radians(angle))
    z = 710 + 80 * math.sin(math.radians(angle))
    add(12, x, z)

# Trebuchet no campo
add(108, TC_CX + 140, 690, yaw=20)
add(108, TC_CX - 140, 700, yaw=160)

# Armamento/equipamento
add(27,  TC_CX - 60, 650)
add(27,  TC_CX + 60, 650)
add(15,  TC_CX,      640)
add(9,   TC_CX - 80, 680)

# Pequena fortaleza de treino no centro
# Muralhas internas formando quadrado 80x80
add(114, TC_CX - 40, 720, yaw=90)
add(114, TC_CX + 40, 720, yaw=90)
add(114, TC_CX - 40, 680, yaw=90)
add(114, TC_CX + 40, 680, yaw=90)
add(114, TC_CX,      700, yaw=0)
add(114, TC_CX - 28, 700, yaw=0)
add(114, TC_CX + 28, 700, yaw=0)
add(105, TC_CX - 40, 720)
add(105, TC_CX + 40, 720)

# ════════════════════════════════════════════════════════════════════════
# 3. ESTRADA PRINCIPAL (z=380-600)
# ════════════════════════════════════════════════════════════════════════
ROAD_X = 512.0

# Paredes da estrada (corredor)
for z in range(600, 380, -28):
    if abs(z - 490) > 40:  # gap no meio para área aberta
        add(113, ROAD_X - 110, z, yaw=0)
        add(113, ROAD_X + 110, z, yaw=0)

# Ruínas espalhadas
ruins = [(ROAD_X-160,570),(ROAD_X+155,545),(ROAD_X-170,500),
         (ROAD_X+160,470),(ROAD_X-150,430),(ROAD_X+145,410)]
for rx, rz in ruins:
    add(113, rx, rz, yaw=int(rx*7)%180)
    add(9,   rx+15, rz+10)

# Barricas ao longo da estrada (ambos os lados)
for i in range(6):
    z = 580 - i * 36
    add(12, ROAD_X - 35, z)
    add(15, ROAD_X + 35, z)

# ════════════════════════════════════════════════════════════════════════
# 4. ARMIA — CIDADE COMPLETA (z=50-380)
# ════════════════════════════════════════════════════════════════════════
A_CX, A_CZ = 512.0, 200.0
A_W, A_H = 400.0, 280.0   # largura e profundidade da cidade
A_X1 = A_CX - A_W/2       # 312
A_X2 = A_CX + A_W/2       # 712
A_Z1 = A_CZ - A_H/2       # 60
A_Z2 = A_CZ + A_H/2       # 340

# Muralha de castelo — paredes
CW = 30   # castle_wall id
CW_S = 16.0

wall_row(CW, A_X1, A_X2, A_Z2, step=CW_S, yaw=90)    # sul
wall_row(CW, A_X1, A_X2, A_Z1, step=CW_S, yaw=90)    # norte
wall_col(CW, A_Z1, A_Z2, A_X1, step=CW_S, yaw=0)     # oeste
wall_col(CW, A_Z1, A_Z2, A_X2, step=CW_S, yaw=0)     # leste

# Torres nas 4 esquinas
for tx, tz in [(A_X1,A_Z1),(A_X2,A_Z1),(A_X1,A_Z2),(A_X2,A_Z2)]:
    add(105, tx, tz)

# Torres intermediárias nas paredes longas
for tx in [A_X1+100, A_X1+200, A_X1+300]:
    add(105, tx, A_Z1)
    add(105, tx, A_Z2)

# Portão sul (entrada principal de Armia)
add(22, A_CX, A_Z2)
add(80, A_CX, A_Z2)

# Portão norte (saída para zonas avançadas)
add(80, A_CX, A_Z1)

# ── 4a. Castelo / Keep (noroeste) ─────────────────────────────────────
KX, KZ = A_X1 + 80, A_CZ - 60

# Muralha interna do castelo
wall_row(CW, KX-60, KX+60, KZ-50, step=CW_S, yaw=90)
wall_row(CW, KX-60, KX+60, KZ+50, step=CW_S, yaw=90)
wall_col(CW, KZ-50, KZ+50, KX-60, step=CW_S, yaw=0)
wall_col(CW, KZ-50, KZ+50, KX+60, step=CW_S, yaw=0)

add(105, KX-60, KZ-50)
add(105, KX+60, KZ-50)
add(105, KX-60, KZ+50)
add(105, KX+60, KZ+50)

add(64,  KX, KZ)                      # keep central
add(110, KX, KZ - 30)                 # two towers na frente
add(80,  KX, KZ + 50, yaw=180)        # portcullis entrada sul

add(33,  KX + 15, KZ + 10)            # baú
add(75,  KX - 20, KZ + 25)            # toras

# ── 4b. Catedral (nordeste) ────────────────────────────────────────────
CHX, CHZ = A_X2 - 80, A_CZ - 60

add(167, CHX,     CHZ+28)              # fundação
add(142, CHX,     CHZ+18)              # entrada principal
add(141, CHX,     CHZ+22)              # porta
add(162, CHX,     CHZ+26)              # escadas

add(140, CHX-22,  CHZ+5,  yaw=90)     # parede L
add(140, CHX+22,  CHZ+5,  yaw=270)    # parede R
add(140, CHX-22,  CHZ-10, yaw=90)     # parede L back
add(140, CHX+22,  CHZ-10, yaw=270)    # parede R back
add(166, CHX,     CHZ-18)             # parede fundos

add(149, CHX-14,  CHZ+12)             # pilar L
add(149, CHX+14,  CHZ+12)             # pilar R
add(149, CHX-14,  CHZ-5)              # pilar L mid
add(149, CHX+14,  CHZ-5)              # pilar R mid

add(137, CHX,     CHZ+5)              # telhado
add(138, CHX,     CHZ-20)             # telhado espiral
add(165, CHX,     CHZ-30)             # topo da torre
add(152, CHX-22,  CHZ-22)             # pinnacle L
add(152, CHX+22,  CHZ-22)             # pinnacle R

# ── 4c. Distrito residencial (centro) ─────────────────────────────────
houses = [
    # Rua sul (z~260-310)
    (A_CX-140, A_CZ+80,   0, 59),
    (A_CX-100, A_CZ+80,  90, 52),
    (A_CX- 55, A_CZ+85,   0, 54),
    (A_CX+  5, A_CZ+85, 180, 56),
    (A_CX+ 55, A_CZ+80,   0, 59),
    (A_CX+100, A_CZ+80, 270, 52),
    (A_CX+145, A_CZ+80,   0, 54),
    # Rua central (z~190-240)
    (A_CX-150, A_CZ+20,   0, 56),
    (A_CX-110, A_CZ+20,  90, 59),
    (A_CX- 65, A_CZ+25, 180, 52),
    (A_CX+ 60, A_CZ+25,   0, 54),
    (A_CX+110, A_CZ+20,  90, 56),
    (A_CX+155, A_CZ+20,   0, 59),
    # Rua norte (z~120-170)
    (A_CX-130, A_CZ-55,   0, 54),
    (A_CX- 85, A_CZ-55,  90, 52),
    (A_CX- 40, A_CZ-50, 180, 59),
    (A_CX+ 40, A_CZ-50,   0, 56),
    (A_CX+ 85, A_CZ-55,  90, 54),
    (A_CX+130, A_CZ-55,   0, 52),
]
for hx, hz, hy, hm in houses:
    add(hm, hx, hz, yaw=hy)

# ── 4d. Mercado (entrada sul da cidade) ───────────────────────────────
for i in range(6):
    x = A_CX - 75 + i * 30
    add(12, x, A_Z2 - 25)            # barris
    add(84, x + 8, A_Z2 - 35)        # sacos

add(29, A_CX + 100, A_Z2 - 50, yaw=45)  # carroça
add(29, A_CX - 100, A_Z2 - 55, yaw=130)

for i in range(5):
    z = A_Z2 - 20 - i * 35
    add(15, A_CX - 40, z)
    add(9,  A_CX + 40, z)

# ── 4e. Moinho (exterior noroeste) ────────────────────────────────────
add(119, A_X1 - 40, A_CZ - 80)

# ── 4f. Portão interno / praça central ────────────────────────────────
# Ruas internas — fileiras de muros menores separando distritos
wall_row(114, A_X1+20, A_CX-80, A_CZ+50, step=14, yaw=90)
wall_row(114, A_CX+80, A_X2-20, A_CZ+50, step=14, yaw=90)

# Torres intermediárias nessas paredes
add(105, A_X1+20,  A_CZ+50)
add(105, A_CX-80,  A_CZ+50)
add(105, A_CX+80,  A_CZ+50)
add(105, A_X2-20,  A_CZ+50)

# ════════════════════════════════════════════════════════════════════════
# INSERT
# ════════════════════════════════════════════════════════════════════════
inserted = 0
for (mid, x, z, yaw, s, yo, pitch) in objects:
    y = gy(x, z) + yo
    cur.execute(
        'INSERT INTO zone_scenery '
        '(area_name,model_id,material_id,x,y,z,pitch,yaw,roll,sx,sy,sz,'
        'collision,anim_mode,inv_size,ownable,locked) '
        'VALUES (?,?,0,?,?,?,?,?,0,?,?,?,0,0,0,0,0)',
        (AREA, mid, x, y, z, float(pitch), float(yaw), s, s, s))
    inserted += 1

cur.execute('CREATE TABLE IF NOT EXISTS zone_dirty '
            '(area_name TEXT PRIMARY KEY, version INTEGER DEFAULT 0)')
cur.execute("INSERT INTO zone_dirty (area_name,version) VALUES (?,1) "
            "ON CONFLICT(area_name) DO UPDATE SET version=version+1", (AREA,))

con.commit()
print(f'Inserted {inserted} objects.')
print('GUE auto-reloading...')
