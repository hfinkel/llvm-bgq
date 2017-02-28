; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -slp-vectorizer -S | FileCheck %s --check-prefix=DEFAULT
; RUN: opt < %s -slp-schedule-budget=0 -slp-min-tree-size=0 -slp-threshold=-30 -slp-vectorizer -S | FileCheck %s --check-prefix=GATHER
; RUN: opt < %s -slp-schedule-budget=0 -slp-threshold=-30 -slp-vectorizer -S | FileCheck %s --check-prefix=MAX-COST

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64--linux-gnu"

@a = common global [80 x i8] zeroinitializer, align 16

; DEFAULT-LABEL: @PR28330(
; DEFAULT: %tmp17 = phi i32 [ %tmp34, %for.body ], [ 0, %entry ]
; DEFAULT: %[[S0:.+]] = select <8 x i1> %1, <8 x i32> <i32 -720, i32 -720, i32 -720, i32 -720, i32 -720, i32 -720, i32 -720, i32 -720>, <8 x i32> <i32 -80, i32 -80, i32 -80, i32 -80, i32 -80, i32 -80, i32 -80, i32 -80>
; DEFAULT: %[[R0:.+]] = shufflevector <8 x i32> %[[S0]], <8 x i32> undef, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 undef, i32 undef, i32 undef, i32 undef>
; DEFAULT: %[[R1:.+]] = add <8 x i32> %[[S0]], %[[R0]]
; DEFAULT: %[[R2:.+]] = shufflevector <8 x i32> %[[R1]], <8 x i32> undef, <8 x i32> <i32 2, i32 3, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; DEFAULT: %[[R3:.+]] = add <8 x i32> %[[R1]], %[[R2]]
; DEFAULT: %[[R4:.+]] = shufflevector <8 x i32> %[[R3]], <8 x i32> undef, <8 x i32> <i32 1, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; DEFAULT: %[[R5:.+]] = add <8 x i32> %[[R3]], %[[R4]]
; DEFAULT: %[[R6:.+]] = extractelement <8 x i32> %[[R5]], i32 0
; DEFAULT: %tmp34 = add i32 %[[R6]], %tmp17
;
; GATHER-LABEL: @PR28330(
; GATHER: %tmp17 = phi i32 [ %tmp34, %for.body ], [ 0, %entry ]
; GATHER: %tmp19 = select i1 %tmp1, i32 -720, i32 -80
; GATHER: %tmp21 = select i1 %tmp3, i32 -720, i32 -80
; GATHER: %tmp23 = select i1 %tmp5, i32 -720, i32 -80
; GATHER: %tmp25 = select i1 %tmp7, i32 -720, i32 -80
; GATHER: %tmp27 = select i1 %tmp9, i32 -720, i32 -80
; GATHER: %tmp29 = select i1 %tmp11, i32 -720, i32 -80
; GATHER: %tmp31 = select i1 %tmp13, i32 -720, i32 -80
; GATHER: %tmp33 = select i1 %tmp15, i32 -720, i32 -80
; GATHER: %[[I0:.+]] = insertelement <8 x i32> undef, i32 %tmp19, i32 0
; GATHER: %[[I1:.+]] = insertelement <8 x i32> %[[I0]], i32 %tmp21, i32 1
; GATHER: %[[I2:.+]] = insertelement <8 x i32> %[[I1]], i32 %tmp23, i32 2
; GATHER: %[[I3:.+]] = insertelement <8 x i32> %[[I2]], i32 %tmp25, i32 3
; GATHER: %[[I4:.+]] = insertelement <8 x i32> %[[I3]], i32 %tmp27, i32 4
; GATHER: %[[I5:.+]] = insertelement <8 x i32> %[[I4]], i32 %tmp29, i32 5
; GATHER: %[[I6:.+]] = insertelement <8 x i32> %[[I5]], i32 %tmp31, i32 6
; GATHER: %[[I7:.+]] = insertelement <8 x i32> %[[I6]], i32 %tmp33, i32 7
; GATHER: %[[R0:.+]] = shufflevector <8 x i32> %[[I7]], <8 x i32> undef, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 undef, i32 undef, i32 undef, i32 undef>
; GATHER: %[[R1:.+]] = add <8 x i32> %[[I7]], %[[R0]]
; GATHER: %[[R2:.+]] = shufflevector <8 x i32> %[[R1]], <8 x i32> undef, <8 x i32> <i32 2, i32 3, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; GATHER: %[[R3:.+]] = add <8 x i32> %[[R1]], %[[R2]]
; GATHER: %[[R4:.+]] = shufflevector <8 x i32> %[[R3]], <8 x i32> undef, <8 x i32> <i32 1, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; GATHER: %[[R5:.+]] = add <8 x i32> %[[R3]], %[[R4]]
; GATHER: %[[R6:.+]] = extractelement <8 x i32> %[[R5]], i32 0
; GATHER: %tmp34 = add i32 %[[R6]], %tmp17
;
; MAX-COST-LABEL: @PR28330(
; MAX-COST-NOT: shufflevector

define void @PR28330(i32 %n) {
entry:
  %tmp0 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 1), align 1
  %tmp1 = icmp eq i8 %tmp0, 0
  %tmp2 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 2), align 2
  %tmp3 = icmp eq i8 %tmp2, 0
  %tmp4 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 3), align 1
  %tmp5 = icmp eq i8 %tmp4, 0
  %tmp6 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 4), align 4
  %tmp7 = icmp eq i8 %tmp6, 0
  %tmp8 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 5), align 1
  %tmp9 = icmp eq i8 %tmp8, 0
  %tmp10 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 6), align 2
  %tmp11 = icmp eq i8 %tmp10, 0
  %tmp12 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 7), align 1
  %tmp13 = icmp eq i8 %tmp12, 0
  %tmp14 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 8), align 8
  %tmp15 = icmp eq i8 %tmp14, 0
  br label %for.body

for.body:
  %tmp17 = phi i32 [ %tmp34, %for.body ], [ 0, %entry ]
  %tmp19 = select i1 %tmp1, i32 -720, i32 -80
  %tmp20 = add i32 %tmp17, %tmp19
  %tmp21 = select i1 %tmp3, i32 -720, i32 -80
  %tmp22 = add i32 %tmp20, %tmp21
  %tmp23 = select i1 %tmp5, i32 -720, i32 -80
  %tmp24 = add i32 %tmp22, %tmp23
  %tmp25 = select i1 %tmp7, i32 -720, i32 -80
  %tmp26 = add i32 %tmp24, %tmp25
  %tmp27 = select i1 %tmp9, i32 -720, i32 -80
  %tmp28 = add i32 %tmp26, %tmp27
  %tmp29 = select i1 %tmp11, i32 -720, i32 -80
  %tmp30 = add i32 %tmp28, %tmp29
  %tmp31 = select i1 %tmp13, i32 -720, i32 -80
  %tmp32 = add i32 %tmp30, %tmp31
  %tmp33 = select i1 %tmp15, i32 -720, i32 -80
  %tmp34 = add i32 %tmp32, %tmp33
  br label %for.body
}

define void @PR32038(i32 %n) {
; DEFAULT-LABEL: @PR32038(
; DEFAULT-NEXT:  entry:
; DEFAULT-NEXT:    [[TMP0:%.*]] = load <8 x i8>, <8 x i8>* bitcast (i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 1) to <8 x i8>*), align 1
; DEFAULT-NEXT:    [[TMP1:%.*]] = icmp eq <8 x i8> [[TMP0]], zeroinitializer
; DEFAULT-NEXT:    br label [[FOR_BODY:%.*]]
; DEFAULT:       for.body:
; DEFAULT-NEXT:    [[TMP17:%.*]] = phi i32 [ [[TMP34:%.*]], [[FOR_BODY]] ], [ 0, [[ENTRY:%.*]] ]
; DEFAULT-NEXT:    [[TMP2:%.*]] = select <8 x i1> [[TMP1]], <8 x i32> <i32 -720, i32 -720, i32 -720, i32 -720, i32 -720, i32 -720, i32 -720, i32 -720>, <8 x i32> <i32 -80, i32 -80, i32 -80, i32 -80, i32 -80, i32 -80, i32 -80, i32 -80>
; DEFAULT-NEXT:    [[TMP20:%.*]] = add i32 -5, undef
; DEFAULT-NEXT:    [[TMP22:%.*]] = add i32 [[TMP20]], undef
; DEFAULT-NEXT:    [[TMP24:%.*]] = add i32 [[TMP22]], undef
; DEFAULT-NEXT:    [[TMP26:%.*]] = add i32 [[TMP24]], undef
; DEFAULT-NEXT:    [[TMP28:%.*]] = add i32 [[TMP26]], undef
; DEFAULT-NEXT:    [[TMP30:%.*]] = add i32 [[TMP28]], undef
; DEFAULT-NEXT:    [[TMP32:%.*]] = add i32 [[TMP30]], undef
; DEFAULT-NEXT:    [[RDX_SHUF:%.*]] = shufflevector <8 x i32> [[TMP2]], <8 x i32> undef, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 undef, i32 undef, i32 undef, i32 undef>
; DEFAULT-NEXT:    [[BIN_RDX:%.*]] = add <8 x i32> [[TMP2]], [[RDX_SHUF]]
; DEFAULT-NEXT:    [[RDX_SHUF1:%.*]] = shufflevector <8 x i32> [[BIN_RDX]], <8 x i32> undef, <8 x i32> <i32 2, i32 3, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; DEFAULT-NEXT:    [[BIN_RDX2:%.*]] = add <8 x i32> [[BIN_RDX]], [[RDX_SHUF1]]
; DEFAULT-NEXT:    [[RDX_SHUF3:%.*]] = shufflevector <8 x i32> [[BIN_RDX2]], <8 x i32> undef, <8 x i32> <i32 1, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; DEFAULT-NEXT:    [[BIN_RDX4:%.*]] = add <8 x i32> [[BIN_RDX2]], [[RDX_SHUF3]]
; DEFAULT-NEXT:    [[TMP3:%.*]] = extractelement <8 x i32> [[BIN_RDX4]], i32 0
; DEFAULT-NEXT:    [[BIN_EXTRA:%.*]] = add i32 [[TMP3]], -5
; DEFAULT-NEXT:    [[TMP34]] = add i32 [[BIN_EXTRA]], [[TMP17]]
; DEFAULT-NEXT:    br label [[FOR_BODY]]
;
; GATHER-LABEL: @PR32038(
; GATHER-NEXT:  entry:
; GATHER-NEXT:    [[TMP0:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 1), align 1
; GATHER-NEXT:    [[TMP1:%.*]] = icmp eq i8 [[TMP0]], 0
; GATHER-NEXT:    [[TMP2:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 2), align 2
; GATHER-NEXT:    [[TMP3:%.*]] = icmp eq i8 [[TMP2]], 0
; GATHER-NEXT:    [[TMP4:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 3), align 1
; GATHER-NEXT:    [[TMP5:%.*]] = icmp eq i8 [[TMP4]], 0
; GATHER-NEXT:    [[TMP6:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 4), align 4
; GATHER-NEXT:    [[TMP7:%.*]] = icmp eq i8 [[TMP6]], 0
; GATHER-NEXT:    [[TMP8:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 5), align 1
; GATHER-NEXT:    [[TMP9:%.*]] = icmp eq i8 [[TMP8]], 0
; GATHER-NEXT:    [[TMP10:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 6), align 2
; GATHER-NEXT:    [[TMP11:%.*]] = icmp eq i8 [[TMP10]], 0
; GATHER-NEXT:    [[TMP12:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 7), align 1
; GATHER-NEXT:    [[TMP13:%.*]] = icmp eq i8 [[TMP12]], 0
; GATHER-NEXT:    [[TMP14:%.*]] = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 8), align 8
; GATHER-NEXT:    [[TMP15:%.*]] = icmp eq i8 [[TMP14]], 0
; GATHER-NEXT:    br label [[FOR_BODY:%.*]]
; GATHER:       for.body:
; GATHER-NEXT:    [[TMP17:%.*]] = phi i32 [ [[TMP34:%.*]], [[FOR_BODY]] ], [ 0, [[ENTRY:%.*]] ]
; GATHER-NEXT:    [[TMP19:%.*]] = select i1 [[TMP1]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP20:%.*]] = add i32 -5, [[TMP19]]
; GATHER-NEXT:    [[TMP21:%.*]] = select i1 [[TMP3]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP22:%.*]] = add i32 [[TMP20]], [[TMP21]]
; GATHER-NEXT:    [[TMP23:%.*]] = select i1 [[TMP5]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP24:%.*]] = add i32 [[TMP22]], [[TMP23]]
; GATHER-NEXT:    [[TMP25:%.*]] = select i1 [[TMP7]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP26:%.*]] = add i32 [[TMP24]], [[TMP25]]
; GATHER-NEXT:    [[TMP27:%.*]] = select i1 [[TMP9]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP28:%.*]] = add i32 [[TMP26]], [[TMP27]]
; GATHER-NEXT:    [[TMP29:%.*]] = select i1 [[TMP11]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP30:%.*]] = add i32 [[TMP28]], [[TMP29]]
; GATHER-NEXT:    [[TMP31:%.*]] = select i1 [[TMP13]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP32:%.*]] = add i32 [[TMP30]], [[TMP31]]
; GATHER-NEXT:    [[TMP33:%.*]] = select i1 [[TMP15]], i32 -720, i32 -80
; GATHER-NEXT:    [[TMP0:%.*]] = insertelement <8 x i32> undef, i32 [[TMP19]], i32 0
; GATHER-NEXT:    [[TMP1:%.*]] = insertelement <8 x i32> [[TMP0]], i32 [[TMP21]], i32 1
; GATHER-NEXT:    [[TMP2:%.*]] = insertelement <8 x i32> [[TMP1]], i32 [[TMP23]], i32 2
; GATHER-NEXT:    [[TMP3:%.*]] = insertelement <8 x i32> [[TMP2]], i32 [[TMP25]], i32 3
; GATHER-NEXT:    [[TMP4:%.*]] = insertelement <8 x i32> [[TMP3]], i32 [[TMP27]], i32 4
; GATHER-NEXT:    [[TMP5:%.*]] = insertelement <8 x i32> [[TMP4]], i32 [[TMP29]], i32 5
; GATHER-NEXT:    [[TMP6:%.*]] = insertelement <8 x i32> [[TMP5]], i32 [[TMP31]], i32 6
; GATHER-NEXT:    [[TMP7:%.*]] = insertelement <8 x i32> [[TMP6]], i32 [[TMP33]], i32 7
; GATHER-NEXT:    [[RDX_SHUF:%.*]] = shufflevector <8 x i32> [[TMP7]], <8 x i32> undef, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 undef, i32 undef, i32 undef, i32 undef>
; GATHER-NEXT:    [[BIN_RDX:%.*]] = add <8 x i32> [[TMP7]], [[RDX_SHUF]]
; GATHER-NEXT:    [[RDX_SHUF1:%.*]] = shufflevector <8 x i32> [[BIN_RDX]], <8 x i32> undef, <8 x i32> <i32 2, i32 3, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; GATHER-NEXT:    [[BIN_RDX2:%.*]] = add <8 x i32> [[BIN_RDX]], [[RDX_SHUF1]]
; GATHER-NEXT:    [[RDX_SHUF3:%.*]] = shufflevector <8 x i32> [[BIN_RDX2]], <8 x i32> undef, <8 x i32> <i32 1, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef, i32 undef>
; GATHER-NEXT:    [[BIN_RDX4:%.*]] = add <8 x i32> [[BIN_RDX2]], [[RDX_SHUF3]]
; GATHER-NEXT:    [[TMP8:%.*]] = extractelement <8 x i32> [[BIN_RDX4]], i32 0
; GATHER-NEXT:    [[BIN_EXTRA:%.*]] = add i32 [[TMP8]], -5
; GATHER-NEXT:    [[TMP34]] = add i32 [[BIN_EXTRA]], [[TMP17]]
; GATHER-NEXT:    br label [[FOR_BODY]]
;
; MAX-COST-LABEL: @PR32038(
entry:
  %tmp0 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 1), align 1
  %tmp1 = icmp eq i8 %tmp0, 0
  %tmp2 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 2), align 2
  %tmp3 = icmp eq i8 %tmp2, 0
  %tmp4 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 3), align 1
  %tmp5 = icmp eq i8 %tmp4, 0
  %tmp6 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 4), align 4
  %tmp7 = icmp eq i8 %tmp6, 0
  %tmp8 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 5), align 1
  %tmp9 = icmp eq i8 %tmp8, 0
  %tmp10 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 6), align 2
  %tmp11 = icmp eq i8 %tmp10, 0
  %tmp12 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 7), align 1
  %tmp13 = icmp eq i8 %tmp12, 0
  %tmp14 = load i8, i8* getelementptr inbounds ([80 x i8], [80 x i8]* @a, i64 0, i64 8), align 8
  %tmp15 = icmp eq i8 %tmp14, 0
  br label %for.body

for.body:
  %tmp17 = phi i32 [ %tmp34, %for.body ], [ 0, %entry ]
  %tmp19 = select i1 %tmp1, i32 -720, i32 -80
  %tmp20 = add i32 -5, %tmp19
  %tmp21 = select i1 %tmp3, i32 -720, i32 -80
  %tmp22 = add i32 %tmp20, %tmp21
  %tmp23 = select i1 %tmp5, i32 -720, i32 -80
  %tmp24 = add i32 %tmp22, %tmp23
  %tmp25 = select i1 %tmp7, i32 -720, i32 -80
  %tmp26 = add i32 %tmp24, %tmp25
  %tmp27 = select i1 %tmp9, i32 -720, i32 -80
  %tmp28 = add i32 %tmp26, %tmp27
  %tmp29 = select i1 %tmp11, i32 -720, i32 -80
  %tmp30 = add i32 %tmp28, %tmp29
  %tmp31 = select i1 %tmp13, i32 -720, i32 -80
  %tmp32 = add i32 %tmp30, %tmp31
  %tmp33 = select i1 %tmp15, i32 -720, i32 -80
  %tmp34 = add i32 %tmp32, %tmp33
  br label %for.body
}
