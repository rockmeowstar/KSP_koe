# SPDX-License-Identifier: MIT
import re

SOURCE_SHA256 = '548026640ee8ccff59c525767748a79bd46da82373b443c951352f8d8d816571'
EXPECTED_SHA256 = '84a85c517d81b8c4065e63ad5b21734e0af7329ca5f163bb005cc644575877f8'
SHADER_NAME = 'EVE/GBufferWetEffect'


def transform(assembly):
    temps = re.search(r'dcl_temps (\d+)', assembly)
    if not temps or int(temps.group(1)) != 12: raise ValueError('Unexpected shader register layout')
    assembly = assembly.replace(temps.group(), 'dcl_temps 16', 1)
    position = 'div r0.xyz, r0.xyzx, r0.wwww'
    slope = 'dp3 r0.z, r6.xyzx, cb0[0].xyzx'
    if assembly.count(position) != 1 or assembly.count(slope) != 1: raise ValueError('Unknown wet surface reconstruction')
    assembly = assembly.replace(position, position+'\n  mov r12.xyz, r0.xyzx', 1)
    guard = '''deriv_rtx r13.xyz, r12.xyzx
      deriv_rty r14.xyz, r12.xyzx
      mul r15.xyz, r13.yzxy, r14.zxyz
      mad r15.xyz, -r13.zxyz, r14.yzxy, r15.xyzx
      dp3 r13.w, r15.xyzx, r15.xyzx
      max r13.w, r13.w, l(0.00000000000000000001)
      rsq r13.w, r13.w
      mul r15.xyz, r15.xyzx, r13.wwww
      dp3 r13.w, r15.xyzx, cb0[0].xyzx
      min r0.z, r0.z, |r13.w|'''
    assembly = assembly.replace(slope, slope+'\n  '+guard, 1)
    return assembly
