#pragma once

#include <SKSE/SKSE.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <memory>
#include <filesystem>

namespace PluginLogger
{
    inline void Setup()
    {
        const auto logDirectory = SKSE::log::log_directory();

        if (!logDirectory) {
            SKSE::stl::report_and_fail(
                "SKSE log directory is unavailable."
            );
            return;
        }

        std::filesystem::create_directories(*logDirectory);

        const auto logFilePath =
            *logDirectory / "Death_Freedom_Camera.log";

        auto fileSink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                logFilePath.string(),
                true
            );

        auto loggerPtr =
            std::make_shared<spdlog::logger>(
                "Death_Freedom_Camera",
                std::move(fileSink)
            );

        // 设置 spdlog 的默认日志器
        spdlog::set_default_logger(std::move(loggerPtr));

        // 设置最低日志级别
        spdlog::set_level(spdlog::level::trace);

        // info 及更高级别的日志立即刷新
        spdlog::flush_on(spdlog::level::info);
    }
}
