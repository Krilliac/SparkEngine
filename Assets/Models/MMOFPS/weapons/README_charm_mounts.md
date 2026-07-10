# TERRAFRONT P0 — weapon charm mount points (README_charm_mounts.md)

Every P0 weapon models a physical charm lug (small cross-pin, DARK material zone)
at the exact grip-space anchor the engine's procedural pendulum uses
(TFViewModel.cpp CharmStyleForSlot). Coordinates are meters in GRIP SPACE
(= OBJ origin; +Z muzzle, Y up):

| slot family                       | lug position (x, y, z) |
|-----------------------------------|------------------------|
| rifle / carbine / lmg / shotgun   |  0.035, -0.045, 0.16   |
| pistol                            |  0.022, -0.035, 0.05   |
| sniper                            |  0.030, -0.045, 0.24   |
| launcher                          |  0.035, -0.060, 0.12   |
| melee / tool                      |  0.000, -0.040, 0.06   |

No data or code change needed: the engine already hangs the charm at these
anchors; the lug simply gives the chain a believable attachment on the mesh.
Mounted weapons (col_cannon, veh_*) have no charm lug.
