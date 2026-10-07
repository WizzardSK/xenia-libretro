# vupkd3d128 dest, src, imm
#
# The third operand is the raw IMM field, and the pack type is IMM >> 2. Every
# case below therefore runs type 0 or type 1 whatever its name once said, so
# they are named for the type that actually runs and the imm that selects it.
#
# type:
#   0 = PACK_TYPE_D3DCOLOR
#   1 = PACK_TYPE_SHORT_2
#   2 = PACK_TYPE_UINT_2101010
#   3 = PACK_TYPE_FLOAT16_2
#   4 = PACK_TYPE_SHORT_4
#   5 = PACK_TYPE_FLOAT16_4
#   6 = PACK_TYPE_ULONG_4202020
#
# TODO: types 2 through 6 have no coverage at all. Adding it needs a console
# capture, both for the expected values and to confirm that IMM >> 2 is the
# real decode and what the low two bits do.

test_vupkd3d128_d3dcolor_imm0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 04010203]
  vupkd3d128 v3, v3, 0
  blr
  #_ REGISTER_OUT v3 [3f800001, 3f800002, 3f800003, 3f800004]

test_vupkd3d128_d3dcolor_imm1_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 7FFF8001]
  vupkd3d128 v3, v3, 1
  blr
  #_ REGISTER_OUT v3 [3F8000FF, 3F800080, 3F800001, 3F80007F]
test_vupkd3d128_d3dcolor_imm1_1:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 4000C000]
  vupkd3d128 v3, v3, 1
  blr
  #_ REGISTER_OUT v3 [3F800000, 3F8000C0, 3F800000, 3F800040]
test_vupkd3d128_d3dcolor_imm1_2:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 7FFFF333]
  vupkd3d128 v3, v3, 1
  blr
  #_ REGISTER_OUT v3 [3F8000FF, 3F8000F3, 3F800033, 3F80007F]
test_vupkd3d128_d3dcolor_imm1_3:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 00008000]
  vupkd3d128 v3, v3, 1
  blr
  #_ REGISTER_OUT v3 [3F800000, 3F800080, 3F800000, 3F800000]

test_vupkd3d128_short2_imm4:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 7FFFFFFF, 007FFFF8]
  vupkd3d128 v3, v3, 4
  blr
  #_ REGISTER_OUT v3 [4040007F, 403FFFF8, 00000000, 3F800000]

test_vupkd3d128_d3dcolor_imm3:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 3800B800]
  vupkd3d128 v3, v3, 3
  blr
  #_ REGISTER_OUT v3 [3F800000, 3F8000B8, 3F800000, 3F800038]

test_vupkd3d128_short2_imm5:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 3800B801, 3802B803]
  vupkd3d128 v3, v3, 5
  blr
  #_ REGISTER_OUT v3 [40403802, 403FB803, 00000000, 3F800000]

test_vupkd3d128_d3dcolor_imm2_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 400001FF]
  vupkd3d128 v3, v3, 2
  blr
  #_ REGISTER_OUT v3 [3F800000, 3F800001, 3F8000FF, 3F800040]
test_vupkd3d128_d3dcolor_imm2_1:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 40000201]
  vupkd3d128 v3, v3, 2
  blr
  #_ REGISTER_OUT v3 [3F800000, 3F800002, 3F800001, 3F800040]
test_vupkd3d128_d3dcolor_imm2_2:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 40000200]
  vupkd3d128 v3, v3, 2
  blr
  #_ REGISTER_OUT v3 [3F800000, 3F800002, 3F800000, 3F800040]
