import zipfile
z = zipfile.ZipFile('D:/games/Far Cry/FCData/Shaders.pak')
for n in z.namelist():
    if any(k in n for k in ['CGVProgTerrainLayerTempl.', 'AmbPass_Particle_VP.', 'CGVProgSimple_Particle.', 'TerrainLayerTemplate.']):
        print(n, z.read(n).decode('latin1').replace('\r', ''))
