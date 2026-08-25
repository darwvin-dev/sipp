/*
 * Multi-instance launcher support for SIPp.
 */

#include "multi_instance.hpp"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

std::string trim_copy(const std::string &value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::string lowercase_copy(std::string value)
{
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

static bool multi_option_matches(const char *value, const char *name)
{
    if (!value) {
        return false;
    }

    std::string short_option = "-" + std::string(name);
    std::string long_option = "--" + std::string(name);
    return value == short_option || value == long_option;
}

static bool parse_port(const std::string &value, int *port)
{
    errno = 0;
    char *end = nullptr;
    long parsed = strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
        parsed <= 0 || parsed > 65535) {
        return false;
    }

    *port = static_cast<int>(parsed);
    return true;
}

MultiInstanceArgParseResult
parse_multi_instance_launcher_args(int argc,
                                   char *argv[],
                                   int default_base_port,
                                   MultiInstanceOptions *options,
                                   std::string *error)
{
    options->config_path.clear();
    options->base_port = default_base_port;

    bool saw_multi = false;
    bool saw_base_port = false;
    std::string unexpected_argument;

    for (int argi = 1; argi < argc; ++argi) {
        if (multi_option_matches(argv[argi], "multi")) {
            if (saw_multi) {
                *error = "-multi may only be specified once";
                return MultiInstanceArgParseResult::INVALID;
            }
            if ((argi + 1) >= argc) {
                *error = "Missing argument for -multi";
                return MultiInstanceArgParseResult::INVALID;
            }
            saw_multi = true;
            options->config_path = argv[++argi];
        } else if (multi_option_matches(argv[argi], "multi_base_port")) {
            if (saw_base_port) {
                *error = "-multi_base_port may only be specified once";
                return MultiInstanceArgParseResult::INVALID;
            }
            if ((argi + 1) >= argc) {
                *error = "Missing argument for -multi_base_port";
                return MultiInstanceArgParseResult::INVALID;
            }
            saw_base_port = true;
            std::string value = argv[++argi];
            if (!parse_port(value, &options->base_port)) {
                *error = "Invalid -multi_base_port value: " + value;
                return MultiInstanceArgParseResult::INVALID;
            }
        } else if (unexpected_argument.empty()) {
            unexpected_argument = argv[argi];
        }
    }

    if (!saw_multi && !saw_base_port) {
        return MultiInstanceArgParseResult::NOT_REQUESTED;
    }

    if (!saw_multi) {
        *error = "-multi_base_port requires -multi";
        return MultiInstanceArgParseResult::INVALID;
    }

    if (options->config_path.empty()) {
        *error = "-multi requires a non-empty CSV file path";
        return MultiInstanceArgParseResult::INVALID;
    }

    if (!unexpected_argument.empty()) {
        *error = "Unexpected argument in -multi mode: " + unexpected_argument +
                 ". Only -multi and -multi_base_port are accepted by the launcher";
        return MultiInstanceArgParseResult::INVALID;
    }

    return MultiInstanceArgParseResult::READY;
}

static void finish_csv_field(std::string *current,
                             bool field_was_quoted,
                             std::vector<std::string> *fields)
{
    if (field_was_quoted) {
        fields->push_back(*current);
    } else {
        fields->push_back(trim_copy(*current));
    }
    current->clear();
}

static bool parse_csv_line(const std::string &line,
                           std::vector<std::string> *fields,
                           std::string *error)
{
    fields->clear();
    std::string current;
    bool in_quotes = false;
    bool field_was_quoted = false;
    bool quote_closed = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];

        if (in_quotes) {
            if (ch == '"') {
                if ((i + 1 < line.size()) && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                    quote_closed = true;
                }
            } else {
                current.push_back(ch);
            }
            continue;
        }

        if (quote_closed) {
            if (ch == ',') {
                finish_csv_field(&current, field_was_quoted, fields);
                field_was_quoted = false;
                quote_closed = false;
            } else if (!std::isspace(static_cast<unsigned char>(ch))) {
                *error = "unexpected character after quoted CSV field";
                return false;
            }
            continue;
        }

        if (ch == ',') {
            finish_csv_field(&current, field_was_quoted, fields);
            field_was_quoted = false;
        } else if (ch == '"') {
            if (!trim_copy(current).empty()) {
                *error = "unexpected quote in unquoted CSV field";
                return false;
            }
            current.clear();
            in_quotes = true;
            field_was_quoted = true;
        } else {
            current.push_back(ch);
        }
    }

    if (in_quotes) {
        *error = "unterminated quoted CSV field";
        return false;
    }

    finish_csv_field(&current, field_was_quoted, fields);
    return true;
}

bool split_command_args(const std::string &args,
                        std::vector<std::string> *words,
                        std::string *error)
{
    words->clear();
    std::string current;
    char quote = 0;
    bool escaping = false;
    bool token_started = false;

    for (char ch : args) {
        if (escaping) {
            current.push_back(ch);
            escaping = false;
            token_started = true;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            token_started = true;
            continue;
        }
        if (quote) {
            if (ch == quote) {
                quote = 0;
            } else {
                current.push_back(ch);
            }
            token_started = true;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            token_started = true;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            if (token_started) {
                words->push_back(current);
                current.clear();
                token_started = false;
            }
        } else {
            current.push_back(ch);
            token_started = true;
        }
    }

    if (escaping) {
        current.push_back('\\');
    }
    if (quote) {
        *error = "unterminated quoted argument";
        return false;
    }
    if (token_started) {
        words->push_back(current);
    }

    return true;
}

static void replace_all(std::string *value,
                        const std::string &needle,
                        const std::string &replacement)
{
    size_t pos = 0;
    while ((pos = value->find(needle, pos)) != std::string::npos) {
        value->replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

static bool canonical_regular_file_path(const std::string &path,
                                        std::string *canonical_path,
                                        std::string *error)
{
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved)) {
        *error = "unable to resolve multi-instance file: " + path + ": " +
                 strerror(errno);
        return false;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        *error = "unable to stat multi-instance file: " + std::string(resolved) +
                 ": " + strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *error = "multi-instance file is not a regular file: " +
                 std::string(resolved);
        return false;
    }

    *canonical_path = resolved;
    return true;
}

#ifndef __linux__
#ifndef __APPLE__
static std::string resolve_from_argv0(const char *argv0)
{
    if (!argv0 || !argv0[0]) {
        return "";
    }

    char resolved[PATH_MAX];
    if (realpath(argv0, resolved)) {
        return resolved;
    }

    if (strchr(argv0, '/')) {
        return argv0;
    }

    const char *path_value = getenv("PATH");
    if (!path_value) {
        return "";
    }

    std::istringstream paths(path_value);
    std::string directory;
    while (std::getline(paths, directory, ':')) {
        if (directory.empty()) {
            directory = ".";
        }
        std::string candidate = directory + "/" + argv0;
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            access(candidate.c_str(), X_OK) == 0) {
            if (realpath(candidate.c_str(), resolved)) {
                return resolved;
            }
            return candidate;
        }
    }

    return "";
}
#endif
#endif

std::string resolve_current_executable_path(const char *argv0)
{
#ifdef __linux__
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length > 0) {
        path[length] = '\0';
        return path;
    }
    return "";
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(path, resolved)) {
            return resolved;
        }
        return path;
    }
    return "";
#else
    return resolve_from_argv0(argv0);
#endif
}

bool parse_multi_instance_csv(const std::string &csv,
                              const std::string &source_name,
                              std::vector<MultiInstanceSpec> *specs,
                              std::string *error)
{
    specs->clear();
    std::istringstream input(csv);
    std::string line;
    int line_number = 0;
    bool first_content_row = true;
    int total_children = 0;

    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        std::vector<std::string> fields;
        std::string parse_error;
        if (!parse_csv_line(line, &fields, &parse_error)) {
            *error = source_name + ":" + std::to_string(line_number) + ": " + parse_error;
            return false;
        }

        if (fields.size() != 3) {
            *error = source_name + ":" + std::to_string(line_number) +
                     ": expected role,count,args";
            return false;
        }

        if (first_content_row) {
            first_content_row = false;
            if (lowercase_copy(fields[0]) == "role" &&
                lowercase_copy(fields[1]) == "count" &&
                lowercase_copy(fields[2]) == "args") {
                continue;
            }
        }

        if (trim_copy(fields[0]).empty()) {
            *error = source_name + ":" + std::to_string(line_number) +
                     ": role must not be empty";
            return false;
        }

        errno = 0;
        char *end = nullptr;
        long count = strtol(fields[1].c_str(), &end, 10);
        if (errno == ERANGE || count > INT_MAX) {
            *error = source_name + ":" + std::to_string(line_number) +
                     ": count is out of range";
            return false;
        }
        if (end == fields[1].c_str() || *end != '\0') {
            *error = source_name + ":" + std::to_string(line_number) +
                     ": count must be a number";
            return false;
        }
        if (count <= 0) {
            *error = source_name + ":" + std::to_string(line_number) +
                     ": count must be greater than zero";
            return false;
        }
        if (count > MAX_MULTI_INSTANCE_CHILDREN ||
            total_children > MAX_MULTI_INSTANCE_CHILDREN - count) {
            *error = source_name + ":" + std::to_string(line_number) +
                     ": multi-instance configuration exceeds the maximum of " +
                     std::to_string(MAX_MULTI_INSTANCE_CHILDREN) + " child processes";
            return false;
        }

        std::vector<std::string> words;
        if (!split_command_args(fields[2], &words, &parse_error)) {
            *error = source_name + ":" + std::to_string(line_number) + ": " + parse_error;
            return false;
        }

        MultiInstanceSpec spec;
        spec.role = fields[0];
        spec.count = static_cast<int>(count);
        spec.args = words;
        specs->push_back(spec);
        total_children += spec.count;
    }

    if (specs->empty()) {
        *error = source_name + ": no multi-instance rows found";
        return false;
    }

    return true;
}

bool parse_multi_instance_csv_file(const std::string &path,
                                   std::vector<MultiInstanceSpec> *specs,
                                   std::string *error)
{
    std::string canonical_path;
    if (!canonical_regular_file_path(path, &canonical_path, error)) {
        return false;
    }

    std::ifstream file(canonical_path);
    if (!file.good()) {
        *error = "unable to open multi-instance file: " + canonical_path;
        return false;
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return parse_multi_instance_csv(contents.str(), canonical_path, specs, error);
}

bool build_multi_instance_commands(const std::string &program_path,
                                   const std::vector<MultiInstanceSpec> &specs,
                                   int base_port,
                                   std::vector<MultiInstanceCommand> *commands,
                                   std::string *error)
{
    commands->clear();

    if (program_path.empty()) {
        *error = "unable to resolve SIPp executable path";
        return false;
    }
    if (base_port <= 0 || base_port > 65535) {
        *error = "multi-instance base port must be between 1 and 65535";
        return false;
    }

    for (const MultiInstanceSpec &spec : specs) {
        if (spec.count <= 0 ||
            commands->size() + static_cast<size_t>(spec.count) >
                static_cast<size_t>(MAX_MULTI_INSTANCE_CHILDREN)) {
            *error = "multi-instance configuration exceeds the maximum of " +
                     std::to_string(MAX_MULTI_INSTANCE_CHILDREN) + " child processes";
            commands->clear();
            return false;
        }

        for (int instance = 0; instance < spec.count; ++instance) {
            long long next_port = static_cast<long long>(base_port) +
                                  static_cast<long long>(commands->size());
            long long instance_port = static_cast<long long>(base_port) + instance;
            if (next_port > 65535 || instance_port > 65535) {
                *error = "multi-instance port allocation exceeds 65535";
                commands->clear();
                return false;
            }

            MultiInstanceCommand command;
            command.role = spec.role;
            command.instance = instance;
            command.port = static_cast<int>(next_port);
            command.executable_path = program_path;
            command.argv.push_back(program_path);

            for (std::string word : spec.args) {
                replace_all(&word, "{role}", spec.role);
                replace_all(&word, "{instance}", std::to_string(instance));
                replace_all(&word, "{instance_port}", std::to_string(instance_port));
                replace_all(&word, "{base_port}", std::to_string(base_port));
                replace_all(&word, "{port}", std::to_string(next_port));
                command.argv.push_back(word);
            }

            commands->push_back(command);
        }
    }

    return true;
}

static void kill_and_reap_children(const std::vector<pid_t> &children,
                                   std::ostream &err)
{
    for (pid_t child : children) {
        if (kill(child, SIGKILL) != 0 && errno != ESRCH) {
            err << "failed to terminate child " << child << ": " << strerror(errno) << "\n";
        }
    }

    for (pid_t child : children) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                err << "waitpid failed for " << child << ": " << strerror(errno) << "\n";
            }
            break;
        }
    }
}

int run_multi_instance_commands(const std::vector<MultiInstanceCommand> &commands,
                                std::ostream &out,
                                std::ostream &err)
{
    std::vector<pid_t> children;
    int exit_code = 0;

    for (const MultiInstanceCommand &command : commands) {
        out << "Starting " << command.role << "[" << command.instance << "]";
        if (command.port > 0) {
            out << " port=" << command.port;
        }
        out << ":";
        for (const std::string &arg : command.argv) {
            out << " " << arg;
        }
        out << "\n";
        out.flush();

        pid_t child = fork();
        if (child < 0) {
            err << "fork failed: " << strerror(errno) << "\n";
            kill_and_reap_children(children, err);
            return 1;
        }
        if (child == 0) {
            std::vector<char *> argv;
            argv.reserve(command.argv.size() + 1);
            for (const std::string &arg : command.argv) {
                argv.push_back(const_cast<char *>(arg.c_str()));
            }
            argv.push_back(nullptr);
            execv(command.executable_path.c_str(), argv.data());
            std::cerr << "exec failed for " << command.executable_path << ": "
                      << strerror(errno) << "\n";
            _exit(127);
        }
        children.push_back(child);
    }

    for (pid_t child : children) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0) {
            err << "waitpid failed for " << child << ": " << strerror(errno) << "\n";
            exit_code = 1;
            continue;
        }

        if (WIFEXITED(status)) {
            int child_exit = WEXITSTATUS(status);
            if (child_exit != 0 && exit_code == 0) {
                exit_code = child_exit;
            }
        } else if (WIFSIGNALED(status)) {
            int signal_number = WTERMSIG(status);
            if (exit_code == 0) {
                exit_code = 128 + signal_number;
            }
        }
    }

    return exit_code;
}
