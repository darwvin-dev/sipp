/*
 * Multi-instance launcher support for SIPp.
 */

#ifndef __SIPP_MULTI_INSTANCE_H__
#define __SIPP_MULTI_INSTANCE_H__

#include <iosfwd>
#include <string>
#include <vector>

constexpr int MAX_MULTI_INSTANCE_CHILDREN = 256;

enum class MultiInstanceArgParseResult {
    NOT_REQUESTED,
    READY,
    INVALID
};

struct MultiInstanceOptions {
    std::string config_path;
    int base_port;
};

struct MultiInstanceSpec {
    std::string role;
    int count;
    std::vector<std::string> args;
};

struct MultiInstanceCommand {
    std::string role;
    int instance;
    int port;
    std::string executable_path;
    std::vector<std::string> argv;
};

std::string trim_copy(const std::string &value);

bool split_command_args(const std::string &args,
                        std::vector<std::string> *words,
                        std::string *error);

MultiInstanceArgParseResult
parse_multi_instance_launcher_args(int argc,
                                   char *argv[],
                                   int default_base_port,
                                   MultiInstanceOptions *options,
                                   std::string *error);

std::string resolve_current_executable_path(const char *argv0);

bool parse_multi_instance_csv(const std::string &csv,
                              const std::string &source_name,
                              std::vector<MultiInstanceSpec> *specs,
                              std::string *error);

bool parse_multi_instance_csv_file(const std::string &path,
                                   std::vector<MultiInstanceSpec> *specs,
                                   std::string *error);

bool build_multi_instance_commands(const std::string &program_path,
                                   const std::vector<MultiInstanceSpec> &specs,
                                   int base_port,
                                   std::vector<MultiInstanceCommand> *commands,
                                   std::string *error);

int run_multi_instance_commands(const std::vector<MultiInstanceCommand> &commands,
                                std::ostream &out,
                                std::ostream &err);

#endif
