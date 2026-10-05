import zipfile, pathlib, struct
for pak in pathlib.Path('D:/games/Far Cry/FCData').glob('*.pak'):
    with zipfile.ZipFile(pak) as z:
        for n in z.namelist():
            if n.lower().endswith(('cloud2.dds', 'cloud1.dds')):
                b=z.read(n)
                print(pak.name, n, 'bytes',len(b), 'header',struct.unpack_from('<7I', b, 4),
                      'pixel format',struct.unpack_from('<8I',b,76))
