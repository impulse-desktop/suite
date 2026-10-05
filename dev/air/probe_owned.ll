source_filename = "probe_owned"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx14.0.0"

@sharedA = internal addrspace(3) global [576 x float] undef, align 4
@sharedB = internal addrspace(3) global [576 x float] undef, align 4

define <4 x float> @layer(<2 x i32> %local, <2 x i32> %origin, i32 addrspace(1)* %words) #0 {
  %x = extractelement <2 x i32> %local, i64 0
  %y = extractelement <2 x i32> %local, i64 1
  %row = mul i32 %y, 24
  %lane = add i32 %row, %x
  %lane64 = zext i32 %lane to i64
  %wp = getelementptr inbounds i32, i32 addrspace(1)* %words, i64 %lane64
  %w = load i32, i32 addrspace(1)* %wp, align 4
  %wf = call float @air.convert.f.f32.u.i32(i32 %w) #1
  %ox = extractelement <2 x i32> %origin, i64 0
  %oxf = call float @air.convert.f.f32.s.i32(i32 %ox) #1
  %a = fadd float %wf, %oxf
  %ap = getelementptr inbounds [576 x float], [576 x float] addrspace(3)* @sharedA, i64 0, i64 %lane64
  store float %a, float addrspace(3)* %ap, align 4
  call void @air.wg.barrier(i32 2, i32 1) #2
  %n = add i32 %lane, 1
  %m = urem i32 %n, 576
  %m64 = zext i32 %m to i64
  %np = getelementptr inbounds [576 x float], [576 x float] addrspace(3)* @sharedA, i64 0, i64 %m64
  %b = load float, float addrspace(3)* %np, align 4
  %bp = getelementptr inbounds [576 x float], [576 x float] addrspace(3)* @sharedB, i64 0, i64 %lane64
  store float %b, float addrspace(3)* %bp, align 4
  call void @air.wg.barrier(i32 2, i32 1) #2
  %r = load float, float addrspace(3)* %bp, align 4
  %oy = extractelement <2 x i32> %origin, i64 1
  %oyf = call float @air.convert.f.f32.s.i32(i32 %oy) #1
  %v0 = insertelement <4 x float> <float undef, float undef, float 0.000000e+00, float 1.000000e+00>, float %r, i64 0
  %v1 = insertelement <4 x float> %v0, float %oyf, i64 1
  ret <4 x float> %v1
}

declare float @air.convert.f.f32.u.i32(i32) #1
declare float @air.convert.f.f32.s.i32(i32) #1
declare void @air.wg.barrier(i32, i32) #2

attributes #0 = { convergent nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { convergent nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.visible = !{!9}
!air.compile_options = !{!16, !17, !18}
!air.version = !{!19}
!air.language_version = !{!20}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 14, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{<4 x float> (<2 x i32>, <2 x i32>, i32 addrspace(1)*)* @layer, !10, !12}
!10 = !{!11}
!11 = !{!"air.visible_output", !"air.arg_type_name", !"float4"}
!12 = !{!13, !14, !15}
!13 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"uint2", !"air.arg_name", !"local"}
!14 = !{i32 1, !"air.visible_input", !"air.arg_type_name", !"int2", !"air.arg_name", !"origin"}
!15 = !{i32 2, !"air.visible_input", !"air.arg_type_name", !"uint", !"air.arg_name", !"words"}
!16 = !{!"air.compile.denorms_disable"}
!17 = !{!"air.compile.fast_math_enable"}
!18 = !{!"air.compile.framebuffer_fetch_enable"}
!19 = !{i32 2, i32 6, i32 0}
!20 = !{!"Metal", i32 3, i32 0, i32 0}
