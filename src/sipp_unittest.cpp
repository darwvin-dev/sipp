/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Author : Rob Day - 11 May 2014
 */

#define GLOBALS_FULL_DEFINITION
#include "sipp.hpp"

#include "multi_instance.hpp"

#include "gtest/gtest.h"
#include <string.h>

int main(int argc, char* argv[])
{
    globalVariables = new AllocVariableTable(nullptr);
    userVariables = new AllocVariableTable(globalVariables);
    main_scenario = new scenario(0, 0);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

/* Quickfix to fix unittests that depend on sipp_exit availability,
 * now that sipp_exit has been moved into sipp.cpp which is not
 * included. */
void sipp_exit(int rc, int rtp_errors, int echo_errors)
{
    exit(rc);
}

static MultiInstanceArgParseResult parse_launcher_args(
    const std::vector<std::string> &arguments,
    MultiInstanceOptions *options,
    std::string *error)
{
    std::vector<std::string> storage = arguments;
    std::vector<char *> argv;
    argv.reserve(storage.size());
    for (std::string &argument : storage) {
        argv.push_back(argument.data());
    }
    return parse_multi_instance_launcher_args(static_cast<int>(argv.size()),
                                              argv.data(),
                                              5060,
                                              options,
                                              error);
}

TEST(MultiInstanceConfig, ParsesQuotedCsvAndExpandsInstanceArguments)
{
    const std::string csv =
        "role,count,args\n"
        "uas,2,\"-sn uas -p {port}\"\n"
        "uac,2,\"-sn uac 127.0.0.1:{instance_port} -m 100 -nostdin\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;

    ASSERT_TRUE(parse_multi_instance_csv(csv, "inline.csv", &specs, &error)) << error;
    ASSERT_EQ(2u, specs.size());
    EXPECT_EQ("uas", specs[0].role);
    EXPECT_EQ(2, specs[0].count);
    EXPECT_EQ(std::vector<std::string>({"-sn", "uas", "-p", "{port}"}), specs[0].args);
    EXPECT_EQ("uac", specs[1].role);
    EXPECT_EQ(2, specs[1].count);

    std::vector<MultiInstanceCommand> commands;
    ASSERT_TRUE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error)) << error;

    ASSERT_EQ(4u, commands.size());
    EXPECT_EQ(std::vector<std::string>({"./sipp", "-sn", "uas", "-p", "5060"}), commands[0].argv);
    EXPECT_EQ(std::vector<std::string>({"./sipp", "-sn", "uas", "-p", "5061"}), commands[1].argv);
    EXPECT_EQ(std::vector<std::string>({"./sipp", "-sn", "uac", "127.0.0.1:5060", "-m", "100", "-nostdin"}), commands[2].argv);
    EXPECT_EQ(std::vector<std::string>({"./sipp", "-sn", "uac", "127.0.0.1:5061", "-m", "100", "-nostdin"}), commands[3].argv);
}

TEST(MultiInstanceConfig, ReportsInvalidCsvRows)
{
    const std::string csv =
        "role,count,args\n"
        "uas,0,\"-sn uas\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;

    EXPECT_FALSE(parse_multi_instance_csv(csv, "bad.csv", &specs, &error));
    EXPECT_NE(std::string::npos, error.find("count must be greater than zero"));
}

TEST(MultiInstanceConfig, AcceptsHeaderAfterCommentsAndCaseInsensitively)
{
    const std::string csv =
        "# generated test configuration\n"
        "\n"
        "Role,Count,Args\n"
        "uas,1,\"-sn uas -nostdin\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;

    ASSERT_TRUE(parse_multi_instance_csv(csv, "comments.csv", &specs, &error)) << error;
    ASSERT_EQ(1u, specs.size());
    EXPECT_EQ("uas", specs[0].role);
}

TEST(MultiInstanceConfig, PreservesQuotedCsvWhitespace)
{
    const std::string csv =
        "role,count,args\n"
        "\"  spaced role  \",1,\"-key role {role}\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;

    ASSERT_TRUE(parse_multi_instance_csv(csv, "spaces.csv", &specs, &error)) << error;
    ASSERT_EQ(1u, specs.size());
    EXPECT_EQ("  spaced role  ", specs[0].role);

    std::vector<MultiInstanceCommand> commands;
    ASSERT_TRUE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error)) << error;
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(4u, commands[0].argv.size());
    EXPECT_EQ("  spaced role  ", commands[0].argv[3]);
}

TEST(MultiInstanceConfig, ExpandsRoleWithQuoteWithoutResplitting)
{
    const std::string csv =
        "role,count,args\n"
        "ua's,1,\"-key role {role}\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;
    ASSERT_TRUE(parse_multi_instance_csv(csv, "quote.csv", &specs, &error)) << error;

    std::vector<MultiInstanceCommand> commands;
    ASSERT_TRUE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error)) << error;
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(4u, commands[0].argv.size());
    EXPECT_EQ("ua's", commands[0].argv[3]);
}

TEST(MultiInstanceConfig, RejectsUnterminatedArgumentQuotes)
{
    const std::string csv =
        "role,count,args\n"
        "uas,1,\"-sn 'uas\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;
    EXPECT_FALSE(parse_multi_instance_csv(csv, "quote.csv", &specs, &error));
    EXPECT_NE(std::string::npos, error.find("unterminated quoted argument"));
}

TEST(MultiInstanceConfig, RejectsCountOverflowAndExcessiveChildren)
{
    std::string error;
    std::vector<MultiInstanceSpec> specs;

    const std::string huge_count =
        "role,count,args\n"
        "uas,4294967296,\"-sn uas\"\n";
    EXPECT_FALSE(parse_multi_instance_csv(huge_count, "huge.csv", &specs, &error));
    EXPECT_NE(std::string::npos, error.find("count"));

    error.clear();
    const std::string too_many =
        "role,count,args\n"
        "uas,128,\"-sn uas\"\n"
        "uac,129,\"-sn uac 127.0.0.1\"\n";
    EXPECT_FALSE(parse_multi_instance_csv(too_many, "many.csv", &specs, &error));
    EXPECT_NE(std::string::npos, error.find("maximum of 256 child processes"));
}

TEST(MultiInstanceConfig, RejectsPortAllocationOverflow)
{
    MultiInstanceSpec spec;
    spec.role = "uas";
    spec.count = 2;
    spec.args = {"-sn", "uas", "-p", "{port}"};

    std::string error;
    std::vector<MultiInstanceCommand> commands;
    EXPECT_FALSE(build_multi_instance_commands("./sipp", {spec}, 65535, &commands, &error));
    EXPECT_NE(std::string::npos, error.find("exceeds 65535"));
    EXPECT_TRUE(commands.empty());
}

TEST(MultiInstanceConfig, RejectsNestedLauncherOptionsAfterExpansion)
{
    std::string error;
    std::vector<MultiInstanceSpec> specs;
    std::vector<MultiInstanceCommand> commands;

    const std::string direct =
        "role,count,args\n"
        "rec,2,\"-multi rec.csv\"\n";
    ASSERT_TRUE(parse_multi_instance_csv(direct, "direct.csv", &specs, &error)) << error;
    EXPECT_FALSE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error));
    EXPECT_NE(std::string::npos, error.find("launcher-only option -multi"));

    error.clear();
    const std::string via_placeholder =
        "role,count,args\n"
        "-multi,2,\"{role} rec.csv\"\n";
    ASSERT_TRUE(parse_multi_instance_csv(via_placeholder, "placeholder.csv", &specs, &error)) << error;
    EXPECT_FALSE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error));
    EXPECT_NE(std::string::npos, error.find("launcher-only option -multi"));
}

TEST(MultiInstanceConfig, ContinuesInstanceNumbersAcrossRepeatedRoleRows)
{
    const std::string csv =
        "role,count,args\n"
        "uas,1,\"-sn uas -p {instance_port}\"\n"
        "uas,1,\"-sn uas -p {instance_port}\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;
    ASSERT_TRUE(parse_multi_instance_csv(csv, "repeated.csv", &specs, &error)) << error;

    std::vector<MultiInstanceCommand> commands;
    ASSERT_TRUE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error)) << error;
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ(0, commands[0].instance);
    EXPECT_EQ(1, commands[1].instance);
    EXPECT_EQ("5060", commands[0].argv.back());
    EXPECT_EQ("5061", commands[1].argv.back());
}

TEST(MultiInstanceConfig, TracksGlobalPortPlaceholderUsage)
{
    const std::string csv =
        "role,count,args\n"
        "uas,1,\"-sn uas -p {instance_port}\"\n"
        "uac,1,\"-sn uac -p {port}\"\n";

    std::string error;
    std::vector<MultiInstanceSpec> specs;
    ASSERT_TRUE(parse_multi_instance_csv(csv, "ports.csv", &specs, &error)) << error;

    std::vector<MultiInstanceCommand> commands;
    ASSERT_TRUE(build_multi_instance_commands("./sipp", specs, 5060, &commands, &error)) << error;
    ASSERT_EQ(2u, commands.size());
    EXPECT_FALSE(commands[0].uses_port);
    EXPECT_TRUE(commands[1].uses_port);
}

TEST(MultiInstanceArgs, BasePortWithoutMultiIsRejected)
{
    MultiInstanceOptions options;
    std::string error;
    EXPECT_EQ(MultiInstanceArgParseResult::INVALID,
              parse_launcher_args({"sipp", "-multi_base_port", "5070"}, &options, &error));
    EXPECT_NE(std::string::npos, error.find("requires -multi"));
}

TEST(MultiInstanceArgs, RejectsUnrelatedArgumentsInMultiMode)
{
    MultiInstanceOptions options;
    std::string error;
    EXPECT_EQ(MultiInstanceArgParseResult::INVALID,
              parse_launcher_args({"sipp", "-multi", "multi.csv", "-m", "999"},
                                  &options,
                                  &error));
    EXPECT_NE(std::string::npos, error.find("Unexpected argument"));
    EXPECT_NE(std::string::npos, error.find("-m"));
}

TEST(MultiInstanceArgs, AcceptsOnlyLauncherArguments)
{
    MultiInstanceOptions options;
    std::string error;
    EXPECT_EQ(MultiInstanceArgParseResult::READY,
              parse_launcher_args({"sipp", "--multi_base_port", "5070", "--multi", "multi.csv"},
                                  &options,
                                  &error));
    EXPECT_EQ("multi.csv", options.config_path);
    EXPECT_EQ(5070, options.base_port);
}

TEST(MultiInstanceArgs, OrdinarySippArgumentsDoNotActivateLauncher)
{
    MultiInstanceOptions options;
    std::string error;
    EXPECT_EQ(MultiInstanceArgParseResult::NOT_REQUESTED,
              parse_launcher_args({"sipp", "-sn", "uas", "-m", "10"}, &options, &error));
}

TEST(MultiInstanceArgs, RejectsDuplicateLauncherOptions)
{
    MultiInstanceOptions options;
    std::string error;
    EXPECT_EQ(MultiInstanceArgParseResult::INVALID,
              parse_launcher_args({"sipp", "-multi", "a.csv", "-multi", "b.csv"},
                                  &options,
                                  &error));
    EXPECT_NE(std::string::npos, error.find("only be specified once"));
}

TEST(MultiInstanceArgs, ShellSplitterPreservesEmptyQuotedArgument)
{
    std::vector<std::string> words;
    std::string error;
    ASSERT_TRUE(split_command_args("-key value \"\" tail", &words, &error)) << error;
    EXPECT_EQ(std::vector<std::string>({"-key", "value", "", "tail"}), words);
}
