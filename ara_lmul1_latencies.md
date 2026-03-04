# ARA RISC-V Vector Instruction Latencies (LMUL=1, Cache Hit)

## Configuration Assumptions

- **LMUL=1**: Baseline vector register size (VL = VLEN/SEW)
- **Cache Hit**: 1 cycle latency for all memory operations  
- **Hardware**: VLEN=128, NrLanes=2
- **Timing**: Pipeline latency + 1 cycle throughput = total execution time

## Hardware Unit Overview

ARA has 7 Vector Functional Units (VFUs) with fixed pipeline latencies:

| VFU | Purpose | Pipeline Latency | Total Cycles (LMUL=1) |
|-----|---------|------------------|-----------------------|
| VFU_Alu | Vector ALU (Arithmetic/Logic) | 1 | 2 |
| VFU_Mul | Integer Multiplier | 0-1 (EW-dependent) | 1-2 |
| VFU_MFpu | Vector Floating-Point Unit | 1-5 (EW+op-dependent) | 2-6 |
| VFU_LoadUnit | Vector Load Operations | 3 + 1 cache hit | 4 |
| VFU_StoreUnit | Vector Store Operations | 2 + 1 cache hit | 3 |
| VFU_Div | Integer Division | Variable (1-64) | 2-65 |
| VFU_SlideUnit | Vector Slide Operations | 1 | 2 |
| VFU_MaskUnit | Vector Mask Logical Operations | 1 | 2 |

## Element Width Support

- **EW8**: 8-bit elements
- **EW16**: 16-bit elements  
- **EW32**: 32-bit elements
- **EW64**: 64-bit elements

---

## VFU_Alu (Vector ALU) - 1 cycle pipeline latency

### Group Latencies
| Element Width | Pipeline Cycles | Total Cycles |
|---------------|-----------------|--------------|
| EW64/32/16/8  | 1               | 2            |

### Specific Instructions

#### Arithmetic Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vadd | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vsub | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vrsub | .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vminu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmin | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmaxu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmax | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vadc | .vv, .vvm, .vx, .vvm | EW64, EW32, EW16, EW8 | 2 |
| vsbc | .vv, .vvm, .vx, .vvm | EW64, EW32, EW16, EW8 | 2 |
| vmadc | .vv, .vvm, .vx, .vvm, .vi | EW64, EW32, EW16, EW8 | 2 |
| vmsbc | .vv, .vvm, .vx, .vvm, .vi | EW64, EW32, EW16, EW8 | 2 |

#### Logical Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vand | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vor | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vxor | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vmandnot | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmor | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmxor | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmnand | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmnor | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |

#### Shift Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vsll | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vsrl | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vsra | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vnsrl | .vv, .vx | EW64, EW32, EW16 | 2 |
| vnsra | .vv, .vx | EW64, EW32, EW16 | 2 |

#### Fixed-Point Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vsaddu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vsadd | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vssubu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vssub | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vaaddu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vaadd | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vasubu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vasub | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vssrl | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vssra | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vnclipu | .vv, .vx, .vi | EW64, EW32, EW16 | 2 |
| vnclip | .vv, .vx, .vi | EW64, EW32, EW16 | 2 |

#### Comparison Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmseq | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vmsne | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vmsltu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmslt | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmsleu | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vmsle | .vv, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |
| vmsgtu | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |
| vmsgt | .vv, .vx | EW64, EW32, EW16, EW8 | 2 |

#### Merge/Move Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmerge | .vvm, .vvm, .vx, .vi | EW64, EW32, EW16, EW8 | 2 |

#### Mask Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmsbf | .m | EW64, EW32, EW16, EW8 | 2 |
| vmsof | .m | EW64, EW32, EW16, EW8 | 2 |
| vmsif | .m | EW64, EW32, EW16, EW8 | 2 |

#### Bit-Manipulation Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| viota | .m | EW64, EW32, EW16, EW8 | 2 |
| vid | .v | EW64, EW32, EW16, EW8 | 2 |

#### Reduction Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vredsum | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredand | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredor | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredxor | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredminu | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredmin | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredmaxu | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vredmax | .vs, .vs | EW64, EW32, EW16, EW8 | 2 |
| vwredsumu | .vs, .vs | EW32, EW16, EW8 | 2 |
| vwredsum | .vs, .vs | EW32, EW16, EW8 | 2 |

---

## VFU_MFpu (Vector Floating-Point Unit) - Variable pipeline latencies

### Group Latencies
| Operation Type | Element Width | Pipeline Cycles | Total Cycles |
|----------------|---------------|-----------------|--------------|
| FP Computation | EW64 | 5 | 6 |
| FP Computation | EW32 | 4 | 5 |
| FP Computation | EW16 | 3 | 4 |
| FP Computation | EW8 | 2 | 3 |
| Non-Computation | All | 1 | 2 |
| Conversion | All | 2 | 3 |
| Div/Sqrt | All | 3 | 4 |

### Specific Instructions

#### Basic FP Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfadd | .vv, .vf | EW64 | 6 |
| vfadd | .vv, .vf | EW32 | 5 |
| vfadd | .vv, .vf | EW16 | 4 |
| vfadd | .vv, .vf | EW8 | 3 |
| vfsub | .vv, .vf | EW64 | 6 |
| vfsub | .vv, .vf | EW32 | 5 |
| vfsub | .vv, .vf | EW16 | 4 |
| vfsub | .vv, .vf | EW8 | 3 |
| vfmul | .vv, .vf | EW64 | 6 |
| vfmul | .vv, .vf | EW32 | 5 |
| vfmul | .vv, .vf | EW16 | 4 |
| vfmul | .vv, .vf | EW8 | 3 |
| vfdiv | .vv, .vf | EW64 | 4 |
| vfdiv | .vv, .vf | EW32 | 4 |
| vfdiv | .vv, .vf | EW16 | 4 |
| vfdiv | .vv, .vf | EW8 | 4 |
| vfrdiv | .vf | EW64 | 4 |
| vfrdiv | .vf | EW32 | 4 |
| vfrdiv | .vf | EW16 | 4 |
| vfrdiv | .vf | EW8 | 4 |
| vfmin | .vv, .vf | EW64 | 2 |
| vfmin | .vv, .vf | EW32 | 2 |
| vfmin | .vv, .vf | EW16 | 2 |
| vfmin | .vv, .vf | EW8 | 2 |
| vfmax | .vv, .vf | EW64 | 2 |
| vfmax | .vv, .vf | EW32 | 2 |
| vfmax | .vv, .vf | EW16 | 2 |
| vfmax | .vv, .vf | EW8 | 2 |
| vfclass | .v | EW64 | 2 |
| vfclass | .v | EW32 | 2 |
| vfclass | .v | EW16 | 2 |
| vfclass | .v | EW8 | 2 |

#### FP Comparison Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmfeq | .vv, .vf | EW64 | 2 |
| vmfeq | .vv, .vf | EW32 | 2 |
| vmfeq | .vv, .vf | EW16 | 2 |
| vmfeq | .vv, .vf | EW8 | 2 |
| vmfle | .vv, .vf | EW64 | 2 |
| vmfle | .vv, .vf | EW32 | 2 |
| vmfle | .vv, .vf | EW16 | 2 |
| vmfle | .vv, .vf | EW8 | 2 |
| vmflt | .vv, .vf | EW64 | 2 |
| vmflt | .vv, .vf | EW32 | 2 |
| vmflt | .vv, .vf | EW16 | 2 |
| vmflt | .vv, .vf | EW8 | 2 |
| vmfne | .vv, .vf | EW64 | 2 |
| vmfne | .vv, .vf | EW32 | 2 |
| vmfne | .vv, .vf | EW16 | 2 |
| vmfne | .vv, .vf | EW8 | 2 |
| vmfgt | .vv, .vf | EW64 | 2 |
| vmfgt | .vv, .vf | EW32 | 2 |
| vmfgt | .vv, .vf | EW16 | 2 |
| vmfgt | .vv, .vf | EW8 | 2 |
| vmfge | .vv, .vf | EW64 | 2 |
| vmfge | .vv, .vf | EW32 | 2 |
| vmfge | .vv, .vf | EW16 | 2 |
| vmfge | .vv, .vf | EW8 | 2 |

#### FP Sign Manipulation Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfsgnj | .vv, .vf | EW64 | 2 |
| vfsgnj | .vv, .vf | EW32 | 2 |
| vfsgnj | .vv, .vf | EW16 | 2 |
| vfsgnj | .vv, .vf | EW8 | 2 |
| vfsgnjn | .vv, .vf | EW64 | 2 |
| vfsgnjn | .vv, .vf | EW32 | 2 |
| vfsgnjn | .vv, .vf | EW16 | 2 |
| vfsgnjn | .vv, .vf | EW8 | 2 |
| vfsgnjx | .vv, .vf | EW64 | 2 |
| vfsgnjx | .vv, .vf | EW32 | 2 |
| vfsgnjx | .vv, .vf | EW16 | 2 |
| vfsgnjx | .vv, .vf | EW8 | 2 |

#### FP Reduction Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfredusum | .vs, .vs | EW64 | 2 |
| vfredusum | .vs, .vs | EW32 | 2 |
| vfredusum | .vs, .vs | EW16 | 2 |
| vfredosum | .vs, .vs | EW64 | 2 |
| vfredosum | .vs, .vs | EW32 | 2 |
| vfredosum | .vs, .vs | EW16 | 2 |
| vfredmin | .vs, .vs | EW64 | 2 |
| vfredmin | .vs, .vs | EW32 | 2 |
| vfredmin | .vs, .vs | EW16 | 2 |
| vfredmax | .vs, .vs | EW64 | 2 |
| vfredmax | .vs, .vs | EW32 | 2 |
| vfredmax | .vs, .vs | EW16 | 2 |
| vfwredusum | .vs, .vs | EW32 | 2 |
| vfwredusum | .vs, .vs | EW16 | 2 |
| vfwredosum | .vs, .vs | EW32 | 2 |
| vfwredosum | .vs, .vs | EW16 | 2 |

#### FP Conversion Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfcvt.xu.f.v | .vv | EW64 | 3 |
| vfcvt.xu.f.v | .vv | EW32 | 3 |
| vfcvt.xu.f.v | .vv | EW16 | 3 |
| vfcvt.xu.f.v | .vv | EW8 | 3 |
| vfcvt.x.f.v | .vv | EW64 | 3 |
| vfcvt.x.f.v | .vv | EW32 | 3 |
| vfcvt.x.f.v | .vv | EW16 | 3 |
| vfcvt.x.f.v | .vv | EW8 | 3 |
| vfcvt.f.xu.v | .vv | EW64 | 3 |
| vfcvt.f.xu.v | .vv | EW32 | 3 |
| vfcvt.f.xu.v | .vv | EW16 | 3 |
| vfcvt.f.xu.v | .vv | EW8 | 3 |
| vfcvt.f.x.v | .vv | EW64 | 3 |
| vfcvt.f.x.v | .vv | EW32 | 3 |
| vfcvt.f.x.v | .vv | EW16 | 3 |
| vfcvt.f.x.v | .vv | EW8 | 3 |
| vfcvt.f.f.v | .vv | EW64 | 3 |
| vfcvt.f.f.v | .vv | EW32 | 3 |
| vfcvt.f.f.v | .vv | EW16 | 3 |
| vfcvt.f.f.v | .vv | EW8 | 3 |
| vfcvt.rtz.xu.f.v | .vv | EW64 | 3 |
| vfcvt.rtz.xu.f.v | .vv | EW32 | 3 |
| vfcvt.rtz.xu.f.v | .vv | EW16 | 3 |
| vfcvt.rtz.xu.f.v | .vv | EW8 | 3 |
| vfcvt.rtz.x.f.v | .vv | EW64 | 3 |
| vfcvt.rtz.x.f.v | .vv | EW32 | 3 |
| vfcvt.rtz.x.f.v | .vv | EW16 | 3 |
| vfcvt.rtz.x.f.v | .vv | EW8 | 3 |
| vfcvt.rod.f.f.v | .vv | EW64 | 3 |
| vfcvt.rod.f.f.v | .vv | EW32 | 3 |
| vfcvt.rod.f.f.v | .vv | EW16 | 3 |
| vfcvt.rod.f.f.v | .vv | EW8 | 3 |

#### FP Special Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfsqrt | .v | EW64 | 4 |
| vfsqrt | .v | EW32 | 4 |
| vfsqrt | .v | EW16 | 4 |
| vfsqrt | .v | EW8 | 4 |
| vfrec7 | .v | EW16 | 2 |
| vfrec7 | .v | EW8 | 2 |
| vfmv.fs | .fs | EW64 | 2 |
| vfmv.fs | .fs | EW32 | 2 |
| vfmv.fs | .fs | EW16 | 2 |
| vfmv.fs | .fs | EW8 | 2 |

#### FP Multiply-Accumulate Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfmadd | .vv, .vf | EW64 | 6 |
| vfmadd | .vv, .vf | EW32 | 5 |
| vfmadd | .vv, .vf | EW16 | 4 |
| vfmadd | .vv, .vf | EW8 | 3 |
| vfnmadd | .vv, .vf | EW64 | 6 |
| vfnmadd | .vv, .vf | EW32 | 5 |
| vfnmadd | .vv, .vf | EW16 | 4 |
| vfnmadd | .vv, .vf | EW8 | 3 |
| vfmsub | .vv, .vf | EW64 | 6 |
| vfmsub | .vv, .vf | EW32 | 5 |
| vfmsub | .vv, .vf | EW16 | 4 |
| vfmsub | .vv, .vf | EW8 | 3 |
| vfnmsub | .vv, .vf | EW64 | 6 |
| vfnmsub | .vv, .vf | EW32 | 5 |
| vfnmsub | .vv, .vf | EW16 | 4 |
| vfnmsub | .vv, .vf | EW8 | 3 |
| vfmacc | .vv, .vf | EW64 | 6 |
| vfmacc | .vv, .vf | EW32 | 5 |
| vfmacc | .vv, .vf | EW16 | 4 |
| vfmacc | .vv, .vf | EW8 | 3 |
| vfnmacc | .vv, .vf | EW64 | 6 |
| vfnmacc | .vv, .vf | EW32 | 5 |
| vfnmacc | .vv, .vf | EW16 | 4 |
| vfnmacc | .vv, .vf | EW8 | 3 |
| vfmsac | .vv, .vf | EW64 | 6 |
| vfmsac | .vv, .vf | EW32 | 5 |
| vfmsac | .vv, .vf | EW16 | 4 |
| vfmsac | .vv, .vf | EW8 | 3 |
| vfnmsac | .vv, .vf | EW64 | 6 |
| vfnmsac | .vv, .vf | EW32 | 5 |
| vfnmsac | .vv, .vf | EW16 | 4 |
| vfnmsac | .vv, .vf | EW8 | 3 |

#### Widening FP Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfwadd | .vv, .vf, .wv | EW32 | 5 |
| vfwadd | .vv, .vf, .wv | EW16 | 4 |
| vfwadd | .vv, .vf, .wv | EW8 | 3 |
| vfwsub | .vv, .vf, .wv | EW32 | 5 |
| vfwsub | .vv, .vf, .wv | EW16 | 4 |
| vfwsub | .vv, .vf, .wv | EW8 | 3 |
| vfwadd.w | .vv, .vf | EW32 | 5 |
| vfwadd.w | .vv, .vf | EW16 | 4 |
| vfwadd.w | .vv, .vf | EW8 | 3 |
| vfwsub.w | .vv, .vf | EW32 | 5 |
| vfwsub.w | .vv, .vf | EW16 | 4 |
| vfwsub.w | .vv, .vf | EW8 | 3 |
| vfwredosum | .vs, .vs | EW32 | 5 |
| vfwredosum | .vs, .vs | EW16 | 4 |
| vfwredusum | .vs, .vs | EW32 | 5 |
| vfwredusum | .vs, .vs | EW16 | 4 |

#### Narrowing FP Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vfncvt.xu.f.w | .vv | EW64 | 3 |
| vfncvt.xu.f.w | .vv | EW32 | 3 |
| vfncvt.xu.f.w | .vv | EW16 | 3 |
| vfncvt.x.f.w | .vv | EW64 | 3 |
| vfncvt.x.f.w | .vv | EW32 | 3 |
| vfncvt.x.f.w | .vv | EW16 | 3 |
| vfncvt.f.xu.w | .vv | EW64 | 3 |
| vfncvt.f.xu.w | .vv | EW32 | 3 |
| vfncvt.f.xu.w | .vv | EW16 | 3 |
| vfncvt.f.x.w | .vv | EW64 | 3 |
| vfncvt.f.x.w | .vv | EW32 | 3 |
| vfncvt.f.x.w | .vv | EW16 | 3 |
| vfncvt.f.f.w | .vv | EW64 | 3 |
| vfncvt.f.f.w | .vv | EW32 | 3 |
| vfncvt.f.f.w | .vv | EW16 | 3 |
| vfncvt.rtz.xu.f.w | .vv | EW64 | 3 |
| vfncvt.rtz.xu.f.w | .vv | EW32 | 3 |
| vfncvt.rtz.xu.f.w | .vv | EW16 | 3 |
| vfncvt.rtz.x.f.w | .vv | EW64 | 3 |
| vfncvt.rtz.x.f.w | .vv | EW32 | 3 |
| vfncvt.rtz.x.f.w | .vv | EW16 | 3 |
| vfncvt.rod.f.f.w | .vv | EW64 | 3 |
| vfncvt.rod.f.f.w | .vv | EW32 | 3 |
| vfncvt.rod.f.f.w | .vv | EW16 | 3 |

---

## Memory Operations (Cache Hit Assumption)

### VFU_LoadUnit - 3 cycles + 1 cache hit = 4 cycles total

#### Group Latencies
| Element Width | Pipeline Cycles | Cache Hit | Total Cycles |
|---------------|-----------------|-----------|--------------|
| EW64/32/16/8  | 3               | 1         | 4            |

#### Specific Instructions
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vle | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 4 |
| vle | .8.ff, .16.ff, .32.ff, .64.ff | EW8, EW16, EW32, EW64 | 4 |
| vle | .8.p, .16.p, .32.p, .64.p | EW8, EW16, EW32, EW64 | 4 |
| vlse | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 4 |
| vlse | .8.ff, .16.ff, .32.ff, .64.ff | EW8, EW16, EW32, EW64 | 4 |
| vlse | .8.p, .16.p, .32.p, .64.p | EW8, EW16, EW32, EW64 | 4 |
| vlxe | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 4 |
| vlxe | .8.ff, .16.ff, .32.ff, .64.ff | EW8, EW16, EW32, EW64 | 4 |
| vlxe | .8.p, .16.p, .32.p, .64.p | EW8, EW16, EW32, EW64 | 4 |

#### Segment Load Operations ( decomposed to regular loads)
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vlse | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .2e8, .2e16, .2e32, .2e64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .3e8, .3e16, .3e32, .3e64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .4e8, .4e16, .4e32, .4e64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .5e8, .5e16, .5e32, .5e64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .6e8, .6e16, .6e32, .6e64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .7e8, .7e16, .7e32, .7e64 | EW8, EW16, EW32, EW64 | 4 |
| vlsseg | .8e8, .8e16, .8e32, .8e64 | EW8, EW16, EW32, EW64 | 4 |

### VFU_StoreUnit - 2 cycles + 1 cache hit = 3 cycles total

#### Group Latencies
| Element Width | Pipeline Cycles | Cache Hit | Total Cycles |
|---------------|-----------------|-----------|--------------|
| EW64/32/16/8  | 2               | 1         | 3            |

#### Specific Instructions
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vse | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 3 |
| vse | .8.ff, .16.ff, .32.ff, .64.ff | EW8, EW16, EW32, EW64 | 3 |
| vse | .8.p, .16.p, .32.p, .64.p | EW8, EW16, EW32, EW64 | 3 |
| vsse | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 3 |
| vsse | .8.ff, .16.ff, .32.ff, .64.ff | EW8, EW16, EW32, EW64 | 3 |
| vsse | .8.p, .16.p, .32.p, .64.p | EW8, EW16, EW32, EW64 | 3 |
| vsxe | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 3 |
| vsxe | .8.ff, .16.ff, .32.ff, .64.ff | EW8, EW16, EW32, EW64 | 3 |
| vsxe | .8.p, .16.p, .32.p, .64.p | EW8, EW16, EW32, EW64 | 3 |

#### Segment Store Operations ( decomposed to regular stores)
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vsse | .8, .16, .32, .64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .2e8, .2e16, .2e32, .2e64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .3e8, .3e16, .3e32, .3e64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .4e8, .4e16, .4e32, .4e64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .5e8, .5e16, .5e32, .5e64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .6e8, .6e16, .6e32, .6e64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .7e8, .7e16, .7e32, .7e64 | EW8, EW16, EW32, EW64 | 3 |
| vssseg | .8e8, .8e16, .8e32, .8e64 | EW8, EW16, EW32, EW64 | 3 |

---

## VFU_Mul (Integer Multiplier) - Element width dependent

### Group Latencies
| Element Width | Pipeline Cycles | Total Cycles |
|---------------|-----------------|--------------|
| EW64/32/16 | 1 | 2 |
| EW8 | 0 | 1 |

### Specific Instructions
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmul | .vv, .vx | EW64 | 2 |
| vmul | .vv, .vx | EW32 | 2 |
| vmul | .vv, .vx | EW16 | 2 |
| vmul | .vv, .vx | EW8 | 1 |
| vmulh | .vv, .vx | EW64 | 2 |
| vmulh | .vv, .vx | EW32 | 2 |
| vmulh | .vv, .vx | EW16 | 2 |
| vmulhsu | .vv, .vx | EW64 | 2 |
| vmulhsu | .vv, .vx | EW32 | 2 |
| vmulhsu | .vv, .vx | EW16 | 2 |
| vmulhu | .vv, .vx | EW64 | 2 |
| vmulhu | .vv, .vx | EW32 | 2 |
| vmulhu | .vv, .vx | EW16 | 2 |

#### Widening Multiply Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vwmul | .vv, .vx | EW32 | 2 |
| vwmul | .vv, .vx | EW16 | 2 |
| vwmul | .vv, .vx | EW8 | 1 |
| vwmulu | .vv, .vx | EW32 | 2 |
| vwmulu | .vv, .vx | EW16 | 2 |
| vwmulu | .vv, .vx | EW8 | 1 |
| vwmulsu | .vv, .vx | EW32 | 2 |
| vwmulsu | .vv, .vx | EW16 | 2 |
| vwmulsu | .vv, .vx | EW8 | 1 |

---

## VFU_Div (Integer Division) - Variable latency

### Group Latencies
| Element Width | Pipeline Range | Total Cycles Range |
|---------------|---------------|-------------------|
| EW64 | 8-64 | 9-65 |
| EW32 | 4-32 | 5-33 |
| EW16 | 2-16 | 3-17 |
| EW8 | 1-8 | 2-9 |

### Specific Instructions
| Instruction | Variants | Element Width | Total Cycles Range |
|-------------|----------|---------------|-------------------|
| vdiv | .vv, .vx | EW64 | 9-65 |
| vdiv | .vv, .vx | EW32 | 5-33 |
| vdiv | .vv, .vx | EW16 | 3-17 |
| vdiv | .vv, .vx | EW8 | 2-9 |
| vrem | .vv, .vx | EW64 | 9-65 |
| vrem | .vv, .vx | EW32 | 5-33 |
| vrem | .vv, .vx | EW16 | 3-17 |
| vrem | .vv, .vx | EW8 | 2-9 |
| vdivu | .vv, .vx | EW64 | 9-65 |
| vdivu | .vv, .vx | EW32 | 5-33 |
| vdivu | .vv, .vx | EW16 | 3-17 |
| vdivu | .vv, .vx | EW8 | 2-9 |
| vremu | .vv, .vx | EW64 | 9-65 |
| vremu | .vv, .vx | EW32 | 5-33 |
| vremu | .vv, .vx | EW16 | 3-17 |
| vremu | .vv, .vx | EW8 | 2-9 |

---

## VFU_SlideUnit (Vector Slide Operations) - 1 cycle pipeline latency

### Group Latencies
| Element Width | Pipeline Cycles | Total Cycles |
|---------------|-----------------|--------------|
| EW64/32/16/8  | 1               | 2            |

### Specific Instructions
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vslideup | .vi, .vx | EW64, EW32, EW16, EW8 | 2 |
| vslideup | .vi, .vx | EW64, EW32, EW16, EW8 | 2 |
| vslidedown | .vi, .vx | EW64, EW32, EW16, EW8 | 2 |
| vslidedown | .vi, .vx | EW64, EW32, EW16, EW8 | 2 |

---

## VFU_MaskUnit (Vector Mask Unit) - 1 cycle pipeline latency

### Group Latencies
| Element Width | Pipeline Cycles | Total Cycles |
|---------------|-----------------|--------------|
| EW64/32/16/8  | 1               | 2            |

### Specific Instructions

#### Mask Logical Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmandnot | .mm | EW64, EW32, EW16, EW8 | 2 |
| vmor | .mm | EW64, EW32, EW16, EW8 | 2 |
| vmxor | .mm | EW64, EW32, EW16, EW8 | 2 |
| vmnand | .mm | EW64, EW32, EW16, EW8 | 2 |
| vmnor | .mm | EW64, EW32, EW16, EW8 | 2 |
| vmxnor | .mm | EW64, EW32, EW16, EW8 | 2 |

#### Mask-to-Mask Operations
| Instruction | Variants | Element Width | Total Cycles |
|-------------|----------|---------------|--------------|
| vmsbf | .m | EW64, EW32, EW16, EW8 | 2 |
| vmsof | .m | EW64, EW32, EW16, EW8 | 2 |
| vmsif | .m | EW64, EW32, EW16, EW8 | 2 |

---

## Special Cases & Notes

### Variable Latency Operations
- **Integer Division (VFU_Div)**: Uses serial divider implementation. Latency is proportional to the number of bits in the operands, ranging from 1-64 pipeline cycles plus 1 cycle throughput.
- **Integer Division Range Notes**: Actual latency depends on the specific operand values. Worst-case division by small numbers takes longer than division by large numbers.

### Memory System Assumptions
- **Cache Hit**: All memory operations assume L1 cache hit with 1 cycle latency.
- **Real-world Variability**: In actual systems, memory latencies can vary significantly based on cache hierarchy, memory controller, and DRAM timing.

### Configuration Dependencies
- **VLEN**: Vector length in bits. Default configuration uses VLEN=128.
- **NrLanes**: Number of parallel lanes. Default configuration uses 2 lanes, providing 128-bit/cycle bandwidth.
- **FP Support**: Floating-point precision support is configurable via compile-time parameters. All widths (EW8-EW64) shown assume full support is enabled.

### Element Width Restrictions
- **EW8 Multiplier**: Has 0-cycle pipeline latency (purely combinational) due to 8-bit multiplication being very fast.
- **FP EW8/EW16**: Support is optional and may not be enabled in all configurations. Shown assuming full support.
- **Widening Operations**: Result element width is always double the source element width.

### Pipeline vs Total Execution Time
- **Pipeline Latency**: Fixed depth for first element processing, independent of LMUL.
- **Total Execution**: Pipeline latency + 1 cycle throughput (for LMUL=1 with VLEN=128, NrLanes=2).
- **LMUL Scaling**: For LMUL>1, total execution time scales linearly, but pipeline latencies remain unchanged.

---

*This document provides comprehensive latency information for all ARA RISC-V vector instructions when configured with LMUL=1 and assuming cache hits for memory operations. Latencies are deterministic except for division operations, which have variable latency based on operand values.*