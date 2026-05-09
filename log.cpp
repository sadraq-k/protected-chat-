#include "log.h"

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/basic_file_sink.h> // Include if you want to use basic file sink
#include <spdlog/sinks/dist_sink.h>      // Useful for aggregating logs

#include <map>
#include <string>
#include <vector>

namespace LogSystem {

    static std::shared_ptr<spdlog::logger> g_default_logger;
    static std::map<std::string, std::shared_ptr<spdlog::logger>> g_named_loggers;
    static std::shared_ptr<spdlog::sinks::dist_sink<std::mutex>> g_dist_sink; // Sink to distribute logs to others

    // Helper function to create a logger with specific sinks
    std::shared_ptr<spdlog::logger> create_logger(
        const std::string& name,
        spdlog::level::level_enum min_level,
        const std::vector<spdlog::sink_ptr>& sinks)
    {
        auto logger = std::make_shared<spdlog::async_logger>(
            name,
            sinks.begin(),
            sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest);

        logger->set_level(min_level);
        logger->flush_on(spdlog::level::warn); // Adjust flushing behavior as needed
        return logger;
    }

    void init(spdlog::level::level_enum min_level)
    {
        if (g_default_logger) {
            return; // Already initialized
        }

        // Initialize thread pool once
        spdlog::init_thread_pool(8192, 2); // Increased worker threads for potentially more loggers

        // --- Default Logger Sinks ---
        // Console sink for general output
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        // Rotating file sink for the main server log
        auto rotating_file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/server.log",
            10 * 1024 * 1024, // 10 MB per file
            5                 // keep 5 files
        );
        rotating_file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");

        std::vector<spdlog::sink_ptr> default_sinks{ console_sink, rotating_file_sink };

        // Create the default logger
        g_default_logger = create_logger("default", min_level, default_sinks);
        spdlog::set_default_logger(g_default_logger);


        // --- Module-Specific Sinks Setup ---
        // Example: A specific sink for a "Network" module
        auto network_file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/network.log",
            5 * 1024 * 1024, // 5 MB per file for network logs
            3                // keep 3 files
        );
        network_file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [Network] [%l] [thread %t] %v");

        // Example: A specific sink for a "Database" module
        auto db_file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/database.log",
            5 * 1024 * 1024, // 5 MB per file for database logs
            3
        );
        db_file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [Database] [%l] [thread %t] %v");

        // You can create more sinks for other modules here...

        // --- Register Named Loggers ---
        // For the "Network" module, use its specific sink and the console sink
        std::vector<spdlog::sink_ptr> network_sinks{ console_sink, network_file_sink };
        g_named_loggers["network"] = create_logger("network", spdlog::level::trace, network_sinks); // Network can log everything

        // For the "Database" module, use its specific sink and the console sink
        std::vector<spdlog::sink_ptr> db_sinks{ console_sink, db_file_sink };
        g_named_loggers["database"] = create_logger("database", spdlog::level::debug, db_sinks); // Database logs from Debug level up

        // IMPORTANT: Register named loggers with SPDLog's registry if you want to use SPDLOG_LOGGER_INFO macros
        spdlog::register_logger(g_named_loggers["network"]);
        spdlog::register_logger(g_named_loggers["database"]);
    }

    void shutdown()
    {
        if (!g_default_logger && g_named_loggers.empty()) return;
        // Unregister named loggers before shutting down the main pool
        for (auto const& [name, logger] : g_named_loggers) {
            spdlog::drop(name); // Remove logger from SPDLog registry
        }
        g_named_loggers.clear();

        if (g_default_logger) {
            spdlog::drop(g_default_logger->name()); // Remove default logger from registry
            g_default_logger.reset();
        }

        spdlog::shutdown(); // This flushes and stops the thread pool
    }

    std::shared_ptr<spdlog::logger> get_default()
    {
        return g_default_logger;
    }

    std::shared_ptr<spdlog::logger> get(const std::string& name)
    {
        auto it = g_named_loggers.find(name);
        if (it != g_named_loggers.end()) {
            return it->second;
        }
        // If logger doesn't exist, you might want to create it on the fly,
        // but for controlled environments, it's often better to pre-define them in init().
        // For this example, we'll return nullptr if not found.
        return nullptr;
    }

} // namespace LogSystem