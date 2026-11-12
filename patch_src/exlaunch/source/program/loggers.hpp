#pragma once

#include <lib/log/svc_logger.hpp>

#if defined(GALAXY_SD_LOG) && GALAXY_SD_LOG
// ATT-0209: hardware builds also write every line to the SD card.
#include "program/patches/sd_log.hpp"
inline exl::log::LoggerMgr<
    exl::log::SvcLogger,
    mod::sdlog::SdLogger
> Logging;
#else
/* Specify logger implementations here. */
inline exl::log::LoggerMgr<
    exl::log::SvcLogger
> Logging;
#endif
