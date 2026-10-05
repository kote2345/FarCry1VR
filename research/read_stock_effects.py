import zipfile
z = zipfile.ZipFile('D:/games/Far Cry/FCData/Shaders.pak')
s = z.read('Shaders/HWScripts/Techniques/terrainWater.csl').decode('latin1').replace('\r', '')
p = s.index('CGRCWater_Beach')
print(s[p-1800:p+3600])
for n in z.namelist():
    if 'Particle' in n and 'CGPShaders' in n:
        print(n, z.read(n).decode('latin1').replace('\r', ''))
