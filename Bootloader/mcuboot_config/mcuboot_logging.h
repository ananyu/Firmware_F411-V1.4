/*
 * mcuboot_logging.h stub for MiniFly MCUBoot port.
 *
 * MCUBOOT_HAVE_LOGGING is not defined, so bootutil_log.h never calls
 * these macros. image_validate.c still includes this header
 * unconditionally via bootutil/crypto/sha.h, hence the stub.
 */

#ifndef __MCUBOOT_LOGGING_H
#define __MCUBOOT_LOGGING_H

#define MCUBOOT_LOG_ERR(...)  do { } while (0)
#define MCUBOOT_LOG_WRN(...)  do { } while (0)
#define MCUBOOT_LOG_INF(...)  do { } while (0)
#define MCUBOOT_LOG_DBG(...)  do { } while (0)
#define MCUBOOT_LOG_SIM(...)  do { } while (0)

#define MCUBOOT_LOG_MODULE_DECLARE(module)   do { } while (0)
#define MCUBOOT_LOG_MODULE_REGISTER(module)  do { } while (0)

#endif /* __MCUBOOT_LOGGING_H */
