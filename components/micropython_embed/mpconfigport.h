/* SolarOS MicroPython embed configuration. */

#include <port/mpconfigport_common.h>

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)

#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_ENABLE_FINALISER                (1)
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_PERSISTENT_CODE_LOAD            (1)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_GCREGS_SETJMP                  (1)

#define MICROPY_PY_GC                           (1)
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_PLATFORM                 "solaros"
#define MICROPY_PY_SYS_ARGV                     (1)
#define MICROPY_PY_SYS_EXIT                     (1)
#define MICROPY_PY_MICROPYTHON                  (1)
#define MICROPY_PY_BUILTINS_MIN_MAX             (1)
#define MICROPY_PY_BUILTINS_BYTEARRAY           (1)
#define MICROPY_PY_ARRAY                        (1)
#define MICROPY_KBD_EXCEPTION                   (1)
#define MICROPY_HELPER_REPL                     (1)

#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_TERSE)
#define MICROPY_WARNINGS                        (0)
#define MICROPY_READER_POSIX                    (0)

void solar_os_micropython_vm_hook(void);

#define MICROPY_VM_HOOK_LOOP                    solar_os_micropython_vm_hook();
