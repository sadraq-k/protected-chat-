#ifndef LOG_SYSTEM_H
#define LOG_SYSTEM_H
#include <memory>
#include <spdlog/spdlog.h>

namespace LogSystem {

	// Call this once at app startup
	void init(spdlog::level::level_enum min_level);

	// Optional: call this before program exit
	void shutdown();

	// Get the default logger (used by SPDLOG_INFO, etc.)
	std::shared_ptr<spdlog::logger> get_default();

	// Get a specific logger by name. Creates it if it doesn't exist.
	std::shared_ptr<spdlog::logger> get(const std::string& name);

} // namespace LogSystem
#endif