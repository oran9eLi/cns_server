// 本文件只负责命令行短路并启动可测试的路由服务进程编排。
#include "core/cli/command_line.hpp"
#include "core/runtime/service_runner.hpp"
#include "service_environment.hpp"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);

    const auto options = cns::cli::ParseCommandLine(arguments);
    if (!options) {
      std::cerr << "错误：" << options.error() << '\n' << cns::cli::Usage();
      return 1;
    }
    if (options->show_help) {
      std::cout << cns::cli::Usage();
      return 0;
    }
    if (options->show_version) {
      std::cout << "route_service " << CNS_ROUTE_SERVICE_VERSION << '\n';
      return 0;
    }

    cns::ServiceEnvironment environment{options->config_path,
                                        options->migrations_path};
    return cns::runtime::RunService(
        options->migrate_only ? cns::runtime::RunMode::kMigrateOnly
                              : cns::runtime::RunMode::kNormal,
        environment);
  } catch (const std::exception&) {
    std::cerr << "错误：路由服务发生未预期异常\n";
    return 1;
  } catch (...) {
    std::cerr << "错误：路由服务发生未知异常\n";
    return 1;
  }
}
