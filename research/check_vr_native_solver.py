"""Generate a host fixture using the current CryPhysics solver and sphere formula."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'SourceCode/CryPhysics/rigidbody.cpp').read_text()
adapter='#include "platform.h"\n#include <algorithm>\n#undef min\n#undef max\nusing std::min; using std::max;\n#define NO_CRY_STREAM\n#include "StdAfx.h"\n#define FUNCTION_PROFILER(...)\n#define FRAME_PROFILER(...)\n#include <cstdio>\n'
include='#include "stdafx.h"' if '#include "stdafx.h"' in source else '#include "StdAfx.h"'
source=source.replace(include,adapter,1)
sphere=(root/'SourceCode/CryPhysics/spheregeom.cpp').read_text()
body=sphere.split('int CSphereGeom::CalcPhysicalProperties(phys_geometry *pgeom)',1)[1].split('int CSphereGeom::PointInsideStatus',1)[0]
body=body.replace('pgeom->pGeom = this;','pgeom->pGeom = NULL;').replace('m_sphere.center','vectorf(zero)').replace('m_sphere.r','radius')
fixture=(root/'research/vr_native_grip_cases.cpp').read_text()
(root/'research/native_grip_solver.generated.cpp').write_text(source+'\nint SphereMassProperties(float radius,phys_geometry *pgeom)'+body+'\n'+fixture)
subprocess.run(['cmd.exe','/d','/c',str(root/'research/build_native_grip_solver.cmd')],cwd=root,check=True)
for dt in (.005,.01,1/72,1/90):
    for mass in (.35,.5,1.348):
        print(f'Native dt={dt:.6f} mass={mass}',flush=True)
        subprocess.run([str(root/'research/native_grip_solver.exe'),str(dt),str(mass)],cwd=root,check=True)

for dt in (.005,.01,1/72,1/90):
    subprocess.run([str(root/"research/native_grip_solver.exe"),str(dt),".5","static"],cwd=root,check=True)

for dt in (.005,.01,1/72,1/90):
    for mass in (.35,.5,1.348):
        subprocess.run([str(root/"research/native_grip_solver.exe"),str(dt),str(mass),"walk"],cwd=root,check=True)
