vs_4_0
dcl_constantbuffer cb13[4096], dynamicIndexed
dcl_input v0.xyz
dcl_input v1.xyz
dcl_input_sgv v2.x, instance_id
dcl_output_siv o0.xyzw, position
dcl_output o1.xyzw
dcl_output o2.xyz
dcl_output o3.x
dcl_temps 4
ishl r3.x, v2.x, l(7)
mov o3.x, v2.x
mul r0.xyzw, v0.yyyy, cb13[r3.x + 10].xyzw
mad r0.xyzw, cb13[r3.x + 9].xyzw, v0.xxxx, r0.xyzw
mad r0.xyzw, cb13[r3.x + 11].xyzw, v0.zzzz, r0.xyzw
add r0.xyzw, r0.xyzw, cb13[r3.x + 12].xyzw
mul r1.xyzw, r0.yyyy, cb13[r3.x + 31].xyzw
mad r1.xyzw, cb13[r3.x + 30].xyzw, r0.xxxx, r1.xyzw
mad r1.xyzw, cb13[r3.x + 32].xyzw, r0.zzzz, r1.xyzw
mad r1.xyzw, cb13[r3.x + 33].xyzw, r0.wwww, r1.xyzw
mov o0.xyzw, r1.xyzw
mul r1.y, r1.y, cb13[r3.x + 8].x
mul r2.xzw, r1.xxwy, l(0.500000, 0.000000, 0.500000, 0.500000)
mov o1.zw, r1.zzzw
add o1.xy, r2.zzzz, r2.xwxx
mul r1.xyz, r0.yyyy, cb13[r3.x + 23].xyzx
mad r1.xyz, cb13[r3.x + 22].xyzx, r0.xxxx, r1.xyzx
mad r0.xyz, cb13[r3.x + 24].xyzx, r0.zzzz, r1.xyzx
mad r0.xyz, cb13[r3.x + 25].xyzx, r0.wwww, r0.xyzx
mul r1.xyz, r0.xyzx, l(-1.000000, -1.000000, 1.000000, 0.000000)
mad r0.xyz, -r0.xyzx, l(-1.000000, -1.000000, 1.000000, 0.000000), v1.xyzx
mad o2.xyz, cb13[r3.x + 2].xxxx, r0.xyzx, r1.xyzx
ret
