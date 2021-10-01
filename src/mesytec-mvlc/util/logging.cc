#include "logging.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace mesytec
{
namespace mvlc
{

std::shared_ptr<spdlog::logger>
    create_logger(const std::string &name, const std::vector<spdlog::sink_ptr> &sinks)
{
    auto logger = spdlog::get(name);

    if (!logger)
    {
        if (!sinks.empty())
        {
            logger = std::make_shared<spdlog::logger>(name, std::begin(sinks), std::end(sinks));
            spdlog::register_logger(logger);
        }
        else
        {
            logger = spdlog::stdout_color_mt(name);
        }
    }

    return logger;
}

std::shared_ptr<spdlog::logger>
    get_logger(const std::string &name)
{
    auto logger = spdlog::get(name);

    if (!logger)
        logger = create_logger(name);

    return logger;
}

} // end namespace mvlc
} // end namespace mesytec