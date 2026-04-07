# Branch Coverage Limitations

This document records the branch-coverage limitations for the completed coverage
iteration on `coverage` at `bf3f76f`. Source data is
`build/meson-logs/coverage.xml` from the passing full unit-test CI wrapper run
on 2026-05-21.

Overall coverage from that run:

- Line coverage: 95.2% (8202/8612)
- Function coverage: 97.7% (683/699)
- Branch coverage: 83.5% (7179/8595)
- Decision coverage: 89.7% (1948/2171)

This file documents only files that still have at least one uncovered branch.
Files at 100% branch coverage are omitted.

Fixable-by-UT coverage work for this set is done. The remaining rows document
real limitations or compiler-generated coverage artifacts; no production code
change is planned for these entries.

The `Compiler-generated arcs` rows are estimates because the coverage XML does
not tag those arcs separately. The repo already excludes throw, unreachable,
function-line, and non-code arcs in `gcovr.cfg`. The primary limitation rows are
rounded from each file's total uncovered count; the compiler-generated arc rows
are separate estimates and may overlap those totals instead of summing exactly.

## `debug_token/main.cpp`

- Total branches: 46
- Covered: 45
- Branch coverage: 97.8%
- Uncovered count: 1

| Limitation (type)           | Affected functions/area                          | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                          |
| --------------------------- | ------------------------------------------------ | ---------------------------------------: | ------------------------------------------------------------------------------------------------- |
| DBus/NSM error matrix paths | Debug-token command startup and request dispatch |                                      ~2% | Requires service and transport failure permutations beyond useful UT scope; no production change. |
| Compiler-generated arcs     | Local object cleanup and branch scaffolding      |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.                            |

## `debug_token/nsm_debug_token.cpp`

- Total branches: 477
- Covered: 441
- Branch coverage: 92.5%
- Uncovered count: 36

| Limitation (type)           | Affected functions/area                                           | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                   |
| --------------------------- | ----------------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------------ |
| DBus/NSM error matrix paths | NSM endpoint lookup, token request handling, and error responses  |                                      ~8% | Requires NSM and DBus transport permutations beyond useful UT scope; no production change. |
| Compiler-generated arcs     | RAII cleanup, optional/error helper paths, and lambda scaffolding |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                     |

## `debug_token/tlv/error.h`

- Total branches: 8
- Covered: 7
- Branch coverage: 87.5%
- Uncovered count: 1

| Limitation (type)        | Affected functions/area                 | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                           |
| ------------------------ | --------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------------- |
| Defensive TLV error path | Error helper and conversion scaffolding |                                     ~13% | Residual defensive path; no production change.                                     |
| Compiler-generated arcs  | Header-inline helper scaffolding        |                             Not isolated | The file is too small to separate generated arcs from the single uncovered branch. |

## `debug_token/token_utility.hpp`

- Total branches: 90
- Covered: 88
- Branch coverage: 97.8%
- Uncovered count: 2

| Limitation (type)        | Affected functions/area                             | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                       |
| ------------------------ | --------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------ |
| Token parsing edge paths | Header-inline token helpers and validation branches |                                      ~2% | Residual malformed-token combinations after UT coverage; no production change. |
| Compiler-generated arcs  | Template and inline helper scaffolding              |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.         |

## `debug_token/update_debug_token.cpp`

- Total branches: 673
- Covered: 647
- Branch coverage: 96.1%
- Uncovered count: 26

| Limitation (type)           | Affected functions/area                                             | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                             |
| --------------------------- | ------------------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------------------------------- |
| DBus/NSM error matrix paths | Update flow, token staging, NSM response mapping, and cleanup paths |                                      ~4% | Requires transport and filesystem failure permutations beyond useful UT scope; no production change. |
| Compiler-generated arcs     | Local RAII cleanup, lambda, and error-wrapper scaffolding           |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                               |

## `fw-status/ap_resource.cpp`

- Total branches: 12
- Covered: 10
- Branch coverage: 83.3%
- Uncovered count: 2

| Limitation (type)                | Affected functions/area                           | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                     |
| -------------------------------- | ------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | AP resource health and recovery state transitions |                                     ~17% | Depends on hardware and DBus state combinations; no production change.       |
| Compiler-generated arcs          | Resource callback and cleanup scaffolding         |                             Not isolated | Small file; generated arcs cannot be separated from resource state branches. |

## `fw-status/ap_resource.hpp`

- Total branches: 4
- Covered: 3
- Branch coverage: 75.0%
- Uncovered count: 1

| Limitation (type)                | Affected functions/area         | Approx. % of file's branches not covered | Unblocked by (or Reason)                                              |
| -------------------------------- | ------------------------------- | ---------------------------------------: | --------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | AP resource inline state helper |                                     ~25% | Depends on product state combinations; no production change.          |
| Compiler-generated arcs          | Header-inline class scaffolding |                             Not isolated | Small file; the single uncovered branch cannot be classified further. |

## `fw-status/base_resource.hpp`

- Total branches: 23
- Covered: 19
- Branch coverage: 82.6%
- Uncovered count: 4

| Limitation (type)                    | Affected functions/area                                        | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                 |
| ------------------------------------ | -------------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------ |
| Shared recovery interface edge paths | Base resource callback dispatch and recovery-state scaffolding |                                     ~17% | Residual shared interface paths after UT coverage; no production change. |
| Compiler-generated arcs              | Header-inline virtual and destructor scaffolding               |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.   |

## `fw-status/connectx_resource.hpp`

- Total branches: 346
- Covered: 198
- Branch coverage: 57.2%
- Uncovered count: 148

| Limitation (type)                | Affected functions/area                                      | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                |
| -------------------------------- | ------------------------------------------------------------ | ---------------------------------------: | --------------------------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | ConnectX health, MCTP/SMA discovery, and GPIO recovery paths |                                     ~43% | Depends on hardware, GPIO, I2C, MCTP, or DBus state combinations; no production change. |
| Compiler-generated arcs          | Template, inline lambda, optional, and cleanup scaffolding   |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.                  |

## `fw-status/cpld_resource.cpp`

- Total branches: 222
- Covered: 145
- Branch coverage: 65.3%
- Uncovered count: 77

| Limitation (type)                | Affected functions/area                                    | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                             |
| -------------------------------- | ---------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------ |
| Hardware/DBus state matrix paths | CPLD log monitoring, endpoint matching, and recovery state |                                     ~35% | Depends on product log, hardware, and DBus state combinations; no production change. |
| Compiler-generated arcs          | Callback, optional, and cleanup scaffolding                |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.               |

## `fw-status/erot_resource.cpp`

- Total branches: 38
- Covered: 33
- Branch coverage: 86.8%
- Uncovered count: 5

| Limitation (type)                | Affected functions/area                     | Approx. % of file's branches not covered | Unblocked by (or Reason)                                               |
| -------------------------------- | ------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | ERoT resource health and recovery callbacks |                                     ~13% | Depends on hardware and DBus state combinations; no production change. |
| Compiler-generated arcs          | Resource callback and cleanup scaffolding   |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap. |

## `fw-status/erot_resource.hpp`

- Total branches: 17
- Covered: 15
- Branch coverage: 88.2%
- Uncovered count: 2

| Limitation (type)                | Affected functions/area                | Approx. % of file's branches not covered | Unblocked by (or Reason)                                            |
| -------------------------------- | -------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | ERoT inline state and recovery helpers |                                     ~12% | Depends on product state combinations; no production change.        |
| Compiler-generated arcs          | Header-inline class scaffolding        |                             Not isolated | Small file; generated arcs cannot be separated from state branches. |

## `fw-status/force_recovery/mcu_recovery_mode_manager.cpp`

- Total branches: 12
- Covered: 11
- Branch coverage: 91.7%
- Uncovered count: 1

| Limitation (type)       | Affected functions/area            | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                     |
| ----------------------- | ---------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------- |
| Recovery-mode edge path | MCU recovery-mode manager dispatch |                                      ~8% | Residual recovery-mode state branch after UT coverage; no production change. |
| Compiler-generated arcs | Local cleanup scaffolding          |                             Not isolated | Small file; the single uncovered branch cannot be classified further.        |

## `fw-status/force_recovery/recovery_mode_manager_base.hpp`

- Total branches: 10
- Covered: 4
- Branch coverage: 40.0%
- Uncovered count: 6

| Limitation (type)                    | Affected functions/area                              | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                           |
| ------------------------------------ | ---------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------------------------------- |
| Shared recovery interface edge paths | Base SetRecoveryMode interface and callback dispatch |                                     ~60% | Header is mostly shared scaffolding with product-specific override behavior; no production change. |
| Compiler-generated arcs              | Header-inline virtual and destructor scaffolding     |                             Not isolated | Small file; generated arcs cannot be separated from interface branches.                            |

## `fw-status/fw-status.cpp`

- Total branches: 1039
- Covered: 859
- Branch coverage: 82.7%
- Uncovered count: 180

| Limitation (type)                             | Affected functions/area                                                | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                    |
| --------------------------------------------- | ---------------------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------------- |
| Configuration matrix / deployment-shape paths | Resource discovery, inventory parsing, and recovery-object publication |                                     ~17% | Requires many product/model option permutations and inventory shapes; no production change. |
| Compiler-generated arcs                       | Local lambda, optional, container, and cleanup scaffolding             |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                      |

## `fw-status/gpio_resource.cpp`

- Total branches: 114
- Covered: 109
- Branch coverage: 95.6%
- Uncovered count: 5

| Limitation (type)                | Affected functions/area                                       | Approx. % of file's branches not covered | Unblocked by (or Reason)                                               |
| -------------------------------- | ------------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | GPIO resource health/state transitions and recovery callbacks |                                      ~4% | Depends on GPIO and DBus state combinations; no production change.     |
| Compiler-generated arcs          | Callback and cleanup scaffolding                              |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap. |

## `fw-status/gpio_resource.hpp`

- Total branches: 15
- Covered: 12
- Branch coverage: 80.0%
- Uncovered count: 3

| Limitation (type)                | Affected functions/area         | Approx. % of file's branches not covered | Unblocked by (or Reason)                                            |
| -------------------------------- | ------------------------------- | ---------------------------------------: | ------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | GPIO inline state helpers       |                                     ~20% | Depends on product GPIO state combinations; no production change.   |
| Compiler-generated arcs          | Header-inline class scaffolding |                             Not isolated | Small file; generated arcs cannot be separated from state branches. |

## `fw-status/gpu_resource.hpp`

- Total branches: 196
- Covered: 133
- Branch coverage: 67.9%
- Uncovered count: 63

| Limitation (type)                | Affected functions/area                                               | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                         |
| -------------------------------- | --------------------------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | GPU OCP recovery health/state transitions and InfoROM companion state |                                     ~32% | Depends on hardware, DBus, and InfoROM state combinations; no production change. |
| Compiler-generated arcs          | Header-inline templates, callbacks, and cleanup scaffolding           |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.           |

## `fw-status/mctp_discovery_resource.cpp`

- Total branches: 50
- Covered: 48
- Branch coverage: 96.0%
- Uncovered count: 2

| Limitation (type)           | Affected functions/area                                    | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                      |
| --------------------------- | ---------------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------- |
| Async/event-loop edge paths | MCTP discovery resource registration and endpoint handling |                                      ~4% | Needs real event-loop and endpoint timing combinations; no production change. |
| Compiler-generated arcs     | Callback and cleanup scaffolding                           |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.        |

## `fw-status/mctp_util/handler.hpp`

- Total branches: 106
- Covered: 91
- Branch coverage: 85.8%
- Uncovered count: 15

| Limitation (type)                         | Affected functions/area                                        | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                         |
| ----------------------------------------- | -------------------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------------- |
| Async/coroutine and event-loop edge paths | MCTP handler registration, receive path, and callback dispatch |                                     ~14% | Needs real event-loop/socket timing or coroutine behavior; no production change. |
| Compiler-generated arcs                   | Coroutine, lambda, and header-inline scaffolding               |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.           |

## `fw-status/mctp_util/instance_id.hpp`

- Total branches: 24
- Covered: 16
- Branch coverage: 66.7%
- Uncovered count: 8

| Limitation (type)         | Affected functions/area                             | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                      |
| ------------------------- | --------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------- |
| Protocol state edge paths | MCTP instance-id allocation and wrap/error handling |                                     ~33% | Residual protocol-state combinations after UT coverage; no production change. |
| Compiler-generated arcs   | Header-inline helper scaffolding                    |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.        |

## `fw-status/mctp_util/mctp_endpoint_discovery.cpp`

- Total branches: 136
- Covered: 96
- Branch coverage: 70.6%
- Uncovered count: 40

| Limitation (type)                         | Affected functions/area                                             | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                   |
| ----------------------------------------- | ------------------------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------- |
| Async/coroutine and event-loop edge paths | MCTP endpoint discovery, filtering, retry, and endpoint publication |                                     ~29% | Needs real MCTP endpoint timing and socket behavior; no production change. |
| Compiler-generated arcs                   | Coroutine/lambda cleanup and optional/container scaffolding         |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.     |

## `fw-status/mctp_util/request.hpp`

- Total branches: 28
- Covered: 22
- Branch coverage: 78.6%
- Uncovered count: 6

| Limitation (type)                         | Affected functions/area                        | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                             |
| ----------------------------------------- | ---------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------ |
| Async/coroutine and event-loop edge paths | MCTP request send/receive and retry handling   |                                     ~21% | Needs real socket timing and coroutine final-suspend behavior; no production change. |
| Compiler-generated arcs                   | Coroutine and header-inline helper scaffolding |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.               |

## `fw-status/mctp_util/socket_handler.cpp`

- Total branches: 54
- Covered: 47
- Branch coverage: 87.0%
- Uncovered count: 7

| Limitation (type)           | Affected functions/area                             | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                   |
| --------------------------- | --------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------- |
| Async/event-loop edge paths | Socket setup, receive, error, and callback handling |                                     ~13% | Needs real socket lifecycle and timing combinations; no production change. |
| Compiler-generated arcs     | Callback and cleanup scaffolding                    |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.     |

## `fw-status/mctp_util/socket_handler.hpp`

- Total branches: 60
- Covered: 50
- Branch coverage: 83.3%
- Uncovered count: 10

| Limitation (type)           | Affected functions/area                           | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                   |
| --------------------------- | ------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------- |
| Async/event-loop edge paths | Socket handler inline state and callback dispatch |                                     ~17% | Needs real socket lifecycle and timing combinations; no production change. |
| Compiler-generated arcs     | Header-inline callback and cleanup scaffolding    |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.     |

## `fw-status/mctp_util/utils.hpp`

- Total branches: 21
- Covered: 8
- Branch coverage: 38.1%
- Uncovered count: 13

| Limitation (type)           | Affected functions/area                              | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                              |
| --------------------------- | ---------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------------------------------- |
| Protocol utility edge paths | MCTP utility validation and endpoint helper branches |                                     ~62% | Header consists of narrow protocol helper paths and invalid-state combinations; no production change. |
| Compiler-generated arcs     | Header-inline optional and conversion scaffolding    |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.                                |

## `fw-status/mcu_resource.hpp`

- Total branches: 57
- Covered: 34
- Branch coverage: 59.6%
- Uncovered count: 23

| Limitation (type)                | Affected functions/area                                      | Approx. % of file's branches not covered | Unblocked by (or Reason)                                               |
| -------------------------------- | ------------------------------------------------------------ | ---------------------------------------: | ---------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | MCU resource health/state transitions and recovery callbacks |                                     ~40% | Depends on hardware and DBus state combinations; no production change. |
| Compiler-generated arcs          | Header-inline callback and cleanup scaffolding               |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap. |

## `fw-status/nvlinkmgmt_nic_resource.hpp`

- Total branches: 300
- Covered: 193
- Branch coverage: 64.3%
- Uncovered count: 107

| Limitation (type)                | Affected functions/area                                              | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                |
| -------------------------------- | -------------------------------------------------------------------- | ---------------------------------------: | --------------------------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | NVLink management NIC health, SMA discovery, and GPIO recovery paths |                                     ~36% | Depends on hardware, GPIO, I2C, MCTP, or DBus state combinations; no production change. |
| Compiler-generated arcs          | Template, inline lambda, optional, and cleanup scaffolding           |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.                  |

## `fw-status/nvswitch_resource.hpp`

- Total branches: 303
- Covered: 199
- Branch coverage: 65.7%
- Uncovered count: 104

| Limitation (type)                | Affected functions/area                                    | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                |
| -------------------------------- | ---------------------------------------------------------- | ---------------------------------------: | --------------------------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | NVSwitch health, SMA discovery, and GPIO recovery paths    |                                     ~34% | Depends on hardware, GPIO, I2C, MCTP, or DBus state combinations; no production change. |
| Compiler-generated arcs          | Template, inline lambda, optional, and cleanup scaffolding |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.                  |

## `fw-status/usb_rcm_resource.cpp`

- Total branches: 74
- Covered: 73
- Branch coverage: 98.6%
- Uncovered count: 1

| Limitation (type)                | Affected functions/area                                          | Approx. % of file's branches not covered | Unblocked by (or Reason)                                               |
| -------------------------------- | ---------------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------- |
| Hardware/DBus state matrix paths | USB RCM resource health/state transitions and recovery callbacks |                                      ~1% | Residual product-state branch after UT coverage; no production change. |
| Compiler-generated arcs          | Callback and cleanup scaffolding                                 |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap. |

## `recovery_tool/common/i2c_utils.cpp`

- Total branches: 58
- Covered: 40
- Branch coverage: 69.0%
- Uncovered count: 18

| Limitation (type)                  | Affected functions/area                                 | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                      |
| ---------------------------------- | ------------------------------------------------------- | ---------------------------------------: | --------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | I2C transaction open, ioctl, retry, and error reporting |                                     ~31% | Requires device and kernel failure combinations beyond useful UT scope; no production change. |
| Compiler-generated arcs            | Local cleanup and error-wrapper scaffolding             |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                        |

## `recovery_tool/common/message_registry.cpp`

- Total branches: 92
- Covered: 74
- Branch coverage: 80.4%
- Uncovered count: 18

| Limitation (type)                  | Affected functions/area                                    | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                    |
| ---------------------------------- | ---------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | Message registry lookup, formatting, and fallback branches |                                     ~20% | Residual registry and formatting edge combinations after UT coverage; no production change. |
| Compiler-generated arcs            | Container lookup and formatting helper scaffolding         |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                      |

## `recovery_tool/glacier_recovery_tool/glacier_recovery_commands.cpp`

- Total branches: 391
- Covered: 317
- Branch coverage: 81.1%
- Uncovered count: 74

| Limitation (type)                  | Affected functions/area                                              | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                        |
| ---------------------------------- | -------------------------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | Glacier command execution, transport failures, and response decoding |                                     ~19% | Represents protocol/device failures or impossible mock-only combinations; no production change. |
| Compiler-generated arcs            | Command dispatch, optional, and cleanup scaffolding                  |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                          |

## `recovery_tool/glacier_recovery_tool/glacier_recovery_interface.cpp`

- Total branches: 67
- Covered: 54
- Branch coverage: 80.6%
- Uncovered count: 13

| Limitation (type)                  | Affected functions/area                                         | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                        |
| ---------------------------------- | --------------------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | Glacier interface setup, command routing, and response handling |                                     ~19% | Represents protocol/device failures or impossible mock-only combinations; no production change. |
| Compiler-generated arcs            | Interface dispatch and cleanup scaffolding                      |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                          |

## `recovery_tool/glacier_recovery_tool/glacier_recovery_utils.cpp`

- Total branches: 81
- Covered: 78
- Branch coverage: 96.3%
- Uncovered count: 3

| Limitation (type)                  | Affected functions/area                          | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                       |
| ---------------------------------- | ------------------------------------------------ | ---------------------------------------: | ------------------------------------------------------------------------------ |
| Protocol/device error matrix paths | Glacier utility validation and fallback branches |                                      ~4% | Residual malformed-input combinations after UT coverage; no production change. |
| Compiler-generated arcs            | Conversion and cleanup scaffolding               |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.         |

## `recovery_tool/glacier_recovery_tool/main.cpp`

- Total branches: 227
- Covered: 204
- Branch coverage: 89.9%
- Uncovered count: 23

| Limitation (type)                   | Affected functions/area                                        | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                    |
| ----------------------------------- | -------------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------------- |
| CLI and protocol error matrix paths | Glacier command-line parsing, validation, and command dispatch |                                     ~10% | Represents CLI/device failure combinations beyond practical UT scope; no production change. |
| Compiler-generated arcs             | CLI option and local cleanup scaffolding                       |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                      |

## `recovery_tool/mcu_recovery_tool/mcu_recovery_manager.cpp`

- Total branches: 404
- Covered: 367
- Branch coverage: 90.8%
- Uncovered count: 37

| Limitation (type)                  | Affected functions/area                                               | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                        |
| ---------------------------------- | --------------------------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | MCU recovery manager, provisioning, reset, registry, and helper paths |                                      ~9% | Represents protocol/device failures or impossible mock-only combinations; no production change. |
| Compiler-generated arcs            | Callback, optional, and cleanup scaffolding                           |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                          |

## `recovery_tool/mcu_recovery_tool/mcu_recovery_manager.hpp`

- Total branches: 20
- Covered: 18
- Branch coverage: 90.0%
- Uncovered count: 2

| Limitation (type)                  | Affected functions/area                                 | Approx. % of file's branches not covered | Unblocked by (or Reason)                                               |
| ---------------------------------- | ------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------- |
| Protocol/device error matrix paths | MCU manager inline helpers and recovery-state interface |                                     ~10% | Residual interface branch after UT coverage; no production change.     |
| Compiler-generated arcs            | Header-inline class scaffolding                         |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap. |

## `recovery_tool/mcu_recovery_tool/utils.cpp`

- Total branches: 248
- Covered: 232
- Branch coverage: 93.5%
- Uncovered count: 16

| Limitation (type)                  | Affected functions/area                                      | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                           |
| ---------------------------------- | ------------------------------------------------------------ | ---------------------------------------: | -------------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | MCU utility parsing, message handling, and fallback branches |                                      ~6% | Residual malformed-input and device-response combinations after UT coverage; no production change. |
| Compiler-generated arcs            | Container, conversion, and cleanup scaffolding               |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                             |

## `recovery_tool/ocp_recovery_tool/recovery_commands.cpp`

- Total branches: 236
- Covered: 222
- Branch coverage: 94.1%
- Uncovered count: 14

| Limitation (type)                  | Affected functions/area                              | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                        |
| ---------------------------------- | ---------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | OCP recovery command execution and response decoding |                                      ~6% | Represents protocol/device failures or impossible mock-only combinations; no production change. |
| Compiler-generated arcs            | Command dispatch and cleanup scaffolding             |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                          |

## `recovery_tool/ocp_recovery_tool/recoverytool_utils.cpp`

- Total branches: 236
- Covered: 212
- Branch coverage: 89.8%
- Uncovered count: 24

| Limitation (type)                  | Affected functions/area                                      | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                        |
| ---------------------------------- | ------------------------------------------------------------ | ---------------------------------------: | ----------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | OCP utility parsing, device mapping, and verbose diagnostics |                                     ~10% | Represents protocol/device failures or impossible mock-only combinations; no production change. |
| Compiler-generated arcs            | Container, optional, and cleanup scaffolding                 |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                          |

## `recovery_tool/ocp_recovery_tool/usb_i2c_mapper.cpp`

- Total branches: 74
- Covered: 72
- Branch coverage: 97.3%
- Uncovered count: 2

| Limitation (type)                  | Affected functions/area                  | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                        |
| ---------------------------------- | ---------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | USB/I2C mapping and fallback diagnostics |                                      ~3% | Residual device-discovery combinations after UT coverage; no production change. |
| Compiler-generated arcs            | Container lookup and cleanup scaffolding |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.          |

## `recovery_tool/usbrcm_recovery_tool/ecid_parser.hpp`

- Total branches: 10
- Covered: 9
- Branch coverage: 90.0%
- Uncovered count: 1

| Limitation (type)                  | Affected functions/area                   | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                      |
| ---------------------------------- | ----------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------- |
| Protocol/device error matrix paths | ECID parsing and malformed-input fallback |                                     ~10% | Residual malformed-input combination after UT coverage; no production change. |
| Compiler-generated arcs            | Header-inline parser scaffolding          |                             Not isolated | Small file; the single uncovered branch cannot be classified further.         |

## `recovery_tool/usbrcm_recovery_tool/force_recovery.cpp`

- Total branches: 123
- Covered: 114
- Branch coverage: 92.7%
- Uncovered count: 9

| Limitation (type)                  | Affected functions/area                                           | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                             |
| ---------------------------------- | ----------------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | USB RCM force-recovery flow, register reads, and recovery toggles |                                      ~7% | Represents device and register failure combinations beyond practical UT scope; no production change. |
| Compiler-generated arcs            | Local cleanup and conversion scaffolding                          |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                               |

## `recovery_tool/usbrcm_recovery_tool/progress_code_parser.hpp`

- Total branches: 123
- Covered: 121
- Branch coverage: 98.4%
- Uncovered count: 2

| Limitation (type)                  | Affected functions/area                            | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                      |
| ---------------------------------- | -------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------- |
| Protocol/device error matrix paths | Progress-code parsing and malformed-input fallback |                                      ~2% | Residual malformed-input combination after UT coverage; no production change. |
| Compiler-generated arcs            | Header-inline parser scaffolding                   |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.        |

## `recovery_tool/usbrcm_recovery_tool/progress_code_queue.cpp`

- Total branches: 72
- Covered: 62
- Branch coverage: 86.1%
- Uncovered count: 10

| Limitation (type)                  | Affected functions/area                                    | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                         |
| ---------------------------------- | ---------------------------------------------------------- | ---------------------------------------: | ------------------------------------------------------------------------------------------------ |
| Protocol/device error matrix paths | Progress-code queue updates, polling, and timeout handling |                                     ~14% | Represents timing and device-event combinations beyond practical UT scope; no production change. |
| Compiler-generated arcs            | Container and cleanup scaffolding                          |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                           |

## `recovery_tool/usbrcm_recovery_tool/usb_device_manager.cpp`

- Total branches: 142
- Covered: 135
- Branch coverage: 95.1%
- Uncovered count: 7

| Limitation (type)                  | Affected functions/area                                       | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                      |
| ---------------------------------- | ------------------------------------------------------------- | ---------------------------------------: | --------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | USB RCM device discovery, open/close, and descriptor handling |                                      ~5% | Represents USB device lifecycle combinations beyond practical UT scope; no production change. |
| Compiler-generated arcs            | Local cleanup and container scaffolding                       |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                        |

## `recovery_tool/usbrcm_recovery_tool/usb_io.cpp`

- Total branches: 38
- Covered: 35
- Branch coverage: 92.1%
- Uncovered count: 3

| Limitation (type)                  | Affected functions/area                  | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                      |
| ---------------------------------- | ---------------------------------------- | ---------------------------------------: | --------------------------------------------------------------------------------------------- |
| Protocol/device error matrix paths | USB IO reads, writes, and error handling |                                      ~8% | Represents USB transfer failure combinations beyond practical UT scope; no production change. |
| Compiler-generated arcs            | Local cleanup scaffolding                |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.                        |

## `src/base_controller.cpp`

- Total branches: 24
- Covered: 21
- Branch coverage: 87.5%
- Uncovered count: 3

| Limitation (type)                    | Affected functions/area                                      | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                             |
| ------------------------------------ | ------------------------------------------------------------ | ---------------------------------------: | ---------------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Core code-manager controller flow and DBus interaction paths |                                     ~13% | Requires process, DBus, or timer lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Local cleanup scaffolding                                    |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                               |

## `src/base_item_updater.cpp`

- Total branches: 164
- Covered: 140
- Branch coverage: 85.4%
- Uncovered count: 24

| Limitation (type)                    | Affected functions/area                                              | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                                         |
| ------------------------------------ | -------------------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Item updater activation, watchdog, image handling, and error cleanup |                                     ~15% | Requires process, DBus, filesystem, or timer lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Lambda, optional, and cleanup scaffolding                            |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                                           |

## `src/base_item_updater.hpp`

- Total branches: 77
- Covered: 68
- Branch coverage: 88.3%
- Uncovered count: 9

| Limitation (type)                    | Affected functions/area                                      | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                                         |
| ------------------------------------ | ------------------------------------------------------------ | ---------------------------------------: | ---------------------------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Item updater inline helpers and activation-state scaffolding |                                     ~12% | Requires process, DBus, filesystem, or timer lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Header-inline helper and cleanup scaffolding                 |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                                           |

## `src/dbusutils.cpp`

- Total branches: 211
- Covered: 104
- Branch coverage: 49.3%
- Uncovered count: 107

| Limitation (type)                    | Affected functions/area                                   | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                                 |
| ------------------------------------ | --------------------------------------------------------- | ---------------------------------------: | -------------------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | DBus method/property helpers and mapper response decoding |                                     ~51% | Requires process, DBus, and mapper failure combinations beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Variant, optional, lambda, and cleanup scaffolding        |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.                                   |

## `src/dbusutils.hpp`

- Total branches: 21
- Covered: 5
- Branch coverage: 23.8%
- Uncovered count: 16

| Limitation (type)                    | Affected functions/area                                | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                            |
| ------------------------------------ | ------------------------------------------------------ | ---------------------------------------: | --------------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Header-inline DBus helper templates and error handling |                                     ~76% | Template helpers expand DBus/error combinations that are not production gaps; no production change. |
| Compiler-generated arcs              | Header-inline template and variant scaffolding         |                                      ~3% | Estimate only; remaining generated arcs are not a production-code gap.                              |

## `src/main.cpp`

- Total branches: 38
- Covered: 36
- Branch coverage: 94.7%
- Uncovered count: 2

| Limitation (type)                    | Affected functions/area                             | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                      |
| ------------------------------------ | --------------------------------------------------- | ---------------------------------------: | --------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Code-manager startup and process lifecycle handling |                                      ~5% | Requires process and DBus lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Local object cleanup scaffolding                    |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.                        |

## `src/signature_verifier.cpp`

- Total branches: 73
- Covered: 67
- Branch coverage: 91.8%
- Uncovered count: 6

| Limitation (type)                         | Affected functions/area                                        | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                              |
| ----------------------------------------- | -------------------------------------------------------------- | ---------------------------------------: | ----------------------------------------------------------------------------------------------------- |
| External command / filesystem error paths | Signature verification command, file checks, and error cleanup |                                      ~8% | Requires process and filesystem failure combinations beyond safe UT simulation; no production change. |
| Compiler-generated arcs                   | Local cleanup and error-wrapper scaffolding                    |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                                |

## `src/version.cpp`

- Total branches: 214
- Covered: 145
- Branch coverage: 67.8%
- Uncovered count: 69

| Limitation (type)                    | Affected functions/area                                                   | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                                         |
| ------------------------------------ | ------------------------------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Version object lifecycle, task/logging paths, and watchdog timer callback |                                     ~32% | Requires process, DBus, filesystem, or timer lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Lambda, optional, and cleanup scaffolding                                 |                                      ~2% | Estimate only; remaining generated arcs are not a production-code gap.                                           |

## `src/version.hpp`

- Total branches: 29
- Covered: 27
- Branch coverage: 93.1%
- Uncovered count: 2

| Limitation (type)                    | Affected functions/area                    | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                    |
| ------------------------------------ | ------------------------------------------ | ---------------------------------------: | ------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Version inline helpers and state callbacks |                                      ~7% | Requires DBus and timer lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Header-inline helper scaffolding           |                                      ~1% | Estimate only; remaining generated arcs are not a production-code gap.                      |

## `src/watch.cpp`

- Total branches: 52
- Covered: 49
- Branch coverage: 94.2%
- Uncovered count: 3

| Limitation (type)                    | Affected functions/area                               | Approx. % of file's branches not covered | Unblocked by (or Reason)                                                                       |
| ------------------------------------ | ----------------------------------------------------- | ---------------------------------------: | ---------------------------------------------------------------------------------------------- |
| Service lifecycle / DBus error paths | Watchdog timer, callback, and process-lifecycle paths |                                      ~6% | Requires timer and process lifecycle failures beyond safe UT simulation; no production change. |
| Compiler-generated arcs              | Callback and cleanup scaffolding                      |                                      <1% | Estimate only; remaining generated arcs are not a production-code gap.                         |
