# avcprobe

Read-only/attachment-only probe for Apple's legacy `IOFireWireAVCLib` user-space interface.

The operational FW410 enumerates as an `IOFireWireUnit` with `Unit_Spec_ID = 0xA02D` and `Unit_SW_Version = 0x14001`. Apple's generic `IOFireWireAVCUnit` personality does not instantiate for this unit, so this probe tests whether the AVCLib plugin can nevertheless be created directly from the `IOFireWireUnit` service.

## Build

```bash
make
```

## Run

```bash
./avcprobe
```

The probe only attempts to create/query `IOFireWireAVCLibUnitInterface`. It does **not** open the AVC unit and does **not** send an AV/C command.

If `IOFireWireAVCLibUnitInterface` can be acquired, the next experiment can use its synchronous `AVCCommand()` method for non-destructive STATUS queries. If it cannot be acquired, `macfw` will implement the FCP command/response transport directly with `IOFireWireLib`.
