# SPDX-License-Identifier: MIT
SOURCE_SHA256 = 'e0cc0c4f040b5c4785b73132256a925baae5d88bff3d7a5dec5630ffb85cd772'
EXPECTED_SHA256 = '1267ffe63a65cdaaf3ec9a67838a206d9023005dacded77707d84f94d9289816'
SHADER_NAME = 'Scatterer/DepthBufferScattering'


def transform(assembly):
    start='  add r2.y, r7.w, -cb0[20].x'
    finish='  max r3.xyzw, r2.xyzw, l(0.000000, 0.000000, 0.000000, 0.000000)'
    if assembly.count(start)!=1 or assembly.count(finish)!=1 or 'dcl_temps 14' not in assembly:
        raise ValueError('Unknown finite-segment shader layout')
    a=assembly.index(start);b=assembly.index(finish,a)+len(finish)
    block=assembly[a:b]
    save='\n'.join(f'    mov r{dst}.xyzw, r{src}.xyzw' for src,dst in [(0,14),(1,15),(2,16),(5,17),(7,18)])
    restore='\n'.join(f'    mov r{dst}.xyzw, r{src}.xyzw' for dst,src in [(0,14),(1,15),(2,16),(5,17),(7,18)])
    before='''  add r20.y, r7.w, -cb0[20].x
      max r20.y, r20.y, l(0.000000)
      add r20.z, r7.w, cb0[20].x
      mul r20.y, r20.y, r20.z
      sqrt r20.y, r20.y
      div r20.y, -r20.y, r7.w
      div r19.z, l(0.500000), cb0[23].y
      add r19.x, r7.z, -r20.y
      lt r19.y, |r19.x|, r19.z
      if_nz r19.y
    '''+save+'''
        mul r20.x, r0.z, r1.w
        mad r20.x, -r7.w, r7.z, r20.x
        div r19.w, r19.x, r19.z
        mad_sat r19.w, r19.w, l(0.500000), l(0.500000)
      endif
    '''
    def evaluate(sign, destination):
        return restore+f'''
        add r7.z, r20.y, {sign}r19.z
        mul r23.x, r7.w, r7.w
        mad r23.x, r20.x, r20.x, r23.x
        mul r23.y, r7.w, r20.x
        mul r23.y, r23.y, l(2.000000)
        mad r23.x, r23.y, r7.z, r23.x
        max r23.x, r23.x, l(0.000000)
        sqrt r0.z, r23.x
        mad r1.w, r7.w, r7.z, r20.x
        div r1.w, r1.w, r0.z
        mul r23.x, r7.w, r0.x
        mad r2.x, r20.x, r1.y, r23.x
        div r2.x, r2.x, r0.z
    '''+block+f'\n    mov r{destination}.xyzw, r2.xyzw\n'
    after='\n  if_nz r19.y\n'+evaluate('-',21)+evaluate('',22)+'''
        add r2.xyzw, r22.xyzw, -r21.xyzw
        mad r2.xyzw, r19.wwww, r2.xyzw, r21.xyzw
        max r3.xyzw, r2.xyzw, l(0.000000, 0.000000, 0.000000, 0.000000)
      endif'''
    candidate=(assembly[:a]+before+block+after+assembly[b:]).replace('dcl_temps 14','dcl_temps 24',1)
    return candidate
