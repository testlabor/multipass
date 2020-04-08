/*
 * Copyright (C) 2018-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "mock_logger.h"
#include "sftp_server_test_fixture.h"
#include "signal.h"

#include <multipass/exceptions/sshfs_missing_error.h>
#include <multipass/logging/log.h>
#include <multipass/optional.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/sshfs_mount/sshfs_mount.h>
#include <multipass/utils.h>

#include <algorithm>
#include <gmock/gmock.h>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpt = multipass::test;

using namespace testing;

typedef std::vector<std::pair<std::string, std::string>> CommandVector;

namespace
{
struct SshfsMount : public mp::test::SftpServerTest
{
    SshfsMount()
    {
        mpl::set_logger(logger);
        channel_read.returnValue(0);
        channel_is_closed.returnValue(0);
    }

    ~SshfsMount()
    {
        mpl::set_logger(nullptr);
    }

    mp::SshfsMount make_sshfsmount(mp::optional<std::string> target = mp::nullopt)
    {
        mp::SSHSession session{"a", 42};
        return {std::move(session), default_source, target.value_or(default_target), default_map, default_map};
    }

    auto make_exec_that_fails_for(const std::vector<std::string>& expected_cmds, bool& invoked)
    {
        auto request_exec = [this, expected_cmds, &invoked](ssh_channel, const char* raw_cmd) {
            std::string cmd{raw_cmd};

            for (const auto expected_cmd : expected_cmds)
            {
                if (cmd.find(expected_cmd) != std::string::npos)
                {
                    invoked = true;
                    exit_status_mock.return_exit_code(SSH_ERROR);
                }
            }
            return SSH_OK;
        };
        return request_exec;
    }

    // The 'invoked' parameter binds the execution and read mocks. We need a better mechanism to make them
    // cooperate better, i.e., make the reader read only when a command was issued.
    auto make_exec_to_check_commands(const CommandVector& commands, std::string::size_type& remaining,
                                     CommandVector::const_iterator& next_expected_cmd, std::string& output,
                                     bool& invoked)
    {
        auto request_exec = [this, &commands, &remaining, &next_expected_cmd, &output, &invoked](ssh_channel,
                                                                                                 const char* raw_cmd) {
            invoked = false;

            std::string cmd{raw_cmd};

            if (next_expected_cmd != commands.end())
            {
                // Check if the first element of the given commands list is what we are expecting. In that case,
                // give the correct answer. If not, check the rest of the list to see if we broke the execution
                // order.
                auto pred = [&cmd](auto it) { return cmd == it.first; };
                CommandVector::const_iterator found_cmd = std::find_if(next_expected_cmd, commands.end(), pred);

                if (found_cmd == next_expected_cmd)
                {
                    invoked = true;
                    output = next_expected_cmd->second;
                    remaining = output.size();
                    ++next_expected_cmd;

                    return SSH_OK;
                }
                else if (found_cmd != commands.end())
                {
                    output = found_cmd->second;
                    remaining = output.size();
                    ADD_FAILURE() << "\"" << (found_cmd->first) << "\" executed out of order; expected \""
                                  << next_expected_cmd->first << "\"";

                    return SSH_OK;
                }
            }

            // If the command list was entirely checked or if the executed command is not on the list, check the
            // default commands to see if there is an answer to the current command. If not, answer with a
            // newline, because all strings returned by the server we are mocking end in newline.
            auto it = default_cmds.find(cmd);
            if (it != default_cmds.end())
            {
                output = it->second;
                remaining = output.size();
                invoked = true;
            }

            return SSH_OK;
        };

        return request_exec;
    }

    // Mock that fails after executing some commands.
    auto make_exec_that_executes_and_fails(const CommandVector& commands, const std::string& fail_cmd,
                                           std::string::size_type& remaining,
                                           CommandVector::const_iterator& next_expected_cmd, std::string& output,
                                           bool& invoked_cmd, bool& invoked_fail)
    {
        auto request_exec = [this, &commands, &fail_cmd, &remaining, &next_expected_cmd, &output, &invoked_cmd,
                             &invoked_fail](ssh_channel, const char* raw_cmd) {
            std::string cmd{raw_cmd};

            if (cmd.find(fail_cmd) != std::string::npos)
            {
                invoked_fail = true;
                exit_status_mock.return_exit_code(SSH_ERROR);
            }
            else if (next_expected_cmd != commands.end() && cmd == next_expected_cmd->first)
            {
                output = next_expected_cmd->second;
                remaining = output.size();
                invoked_cmd = true;
                ++next_expected_cmd;
            }
            else
            {
                auto it = default_cmds.find(cmd);
                if (it != default_cmds.end())
                {
                    output = it->second;
                    remaining = output.size();
                    invoked_cmd = true;
                }
            }

            return SSH_OK;
        };

        return request_exec;
    }

    auto make_channel_read_return(const std::string& output, std::string::size_type& remaining, bool& prereq_invoked)
    {
        auto channel_read = [&output, &remaining, &prereq_invoked](ssh_channel, void* dest, uint32_t count,
                                                                   int is_stderr, int) {
            if (!prereq_invoked)
                return 0u;
            const auto num_to_copy = std::min(count, static_cast<uint32_t>(remaining));
            const auto begin = output.begin() + output.size() - remaining;
            std::copy_n(begin, num_to_copy, reinterpret_cast<char*>(dest));
            remaining -= num_to_copy;
            return num_to_copy;
        };
        return channel_read;
    }

    // Check that a given command is invoked and that it throws when failing.
    void test_failed_invocation(const std::string& fail_cmd)
    {
        CommandVector commands = {
            {"snap run multipass-sshfs.env", "LD_LIBRARY_PATH=/foo/bar\nSNAP=/baz\n"},
            {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: 3.0.0\n"},
            {"pwd", "/home/ubuntu\n"},
            {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
             "/home/ubuntu/\n"}};

        bool invoked_cmd{false}, invoked_fail{false};
        std::string output;
        std::string::size_type remaining;
        CommandVector::const_iterator next_expected_cmd = commands.begin();

        auto channel_read = make_channel_read_return(output, remaining, invoked_cmd);
        REPLACE(ssh_channel_read_timeout, channel_read);

        auto request_exec = make_exec_that_executes_and_fails(commands, fail_cmd, remaining, next_expected_cmd, output,
                                                              invoked_cmd, invoked_fail);
        REPLACE(ssh_channel_request_exec, request_exec);

        EXPECT_THROW(make_sshfsmount(), std::runtime_error);
        EXPECT_TRUE(invoked_fail);
    }

    void test_command_execution(const CommandVector& commands, mp::optional<std::string> target = mp::nullopt)
    {
        bool invoked{false};
        std::string output;
        auto remaining = output.size();
        CommandVector::const_iterator next_expected_cmd = commands.begin();

        auto channel_read = make_channel_read_return(output, remaining, invoked);
        REPLACE(ssh_channel_read_timeout, channel_read);

        auto request_exec = make_exec_to_check_commands(commands, remaining, next_expected_cmd, output, invoked);
        REPLACE(ssh_channel_request_exec, request_exec);

        make_sshfsmount(target.value_or(default_target));

        EXPECT_TRUE(next_expected_cmd == commands.end()) << "\"" << next_expected_cmd->first << "\" not executed";
    }

    template <typename Matcher>
    auto make_cstring_matcher(const Matcher& matcher)
    {
        return Property(&mpl::CString::c_str, matcher);
    }

    mpt::ExitStatusMock exit_status_mock;
    decltype(MOCK(ssh_channel_read_timeout)) channel_read{MOCK(ssh_channel_read_timeout)};
    decltype(MOCK(ssh_channel_is_closed)) channel_is_closed{MOCK(ssh_channel_is_closed)};

    std::string default_source{"source"};
    std::string default_target{"target"};
    std::unordered_map<int, int> default_map;
    int default_id{1000};
    std::shared_ptr<NiceMock<mpt::MockLogger>> logger = std::make_shared<NiceMock<mpt::MockLogger>>();

    const std::unordered_map<std::string, std::string> default_cmds{
        {"snap run multipass-sshfs.env", "LD_LIBRARY_PATH=/foo/bar\nSNAP=/baz\n"},
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: 3.0.0\n"},
        {"id -u", "1000\n"},
        {"id -g", "1000\n"},
        {"pwd", "/home/ubuntu\n"},
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -o slave -o transform_symlinks -o allow_other :\"source\" "
         "\"target\"",
         "don't care\n"}};
};
} // namespace

TEST_F(SshfsMount, throws_when_sshfs_does_not_exist)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"sudo multipass-sshfs.env", "which sshfs"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), mp::SSHFSMissingError);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_make_target_dir)
{
    test_failed_invocation("mkdir");
}

TEST_F(SshfsMount, throws_when_unable_to_chown)
{
    test_failed_invocation("chown");
}

TEST_F(SshfsMount, throws_when_unable_to_obtain_user_id)
{
    test_failed_invocation("id -u");
}

TEST_F(SshfsMount, throws_when_uid_is_not_an_integer)
{
    bool cmd_invoked{false};
    bool uid_invoked{false};
    std::string output;
    std::string::size_type remaining;

    auto request_exec = [&cmd_invoked, &uid_invoked, &remaining, &output](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};

        if (cmd.find("id -u") != std::string::npos)
        {
            output = "ubuntu\n";
            remaining = output.size();
            uid_invoked = true;
        }
        if (cmd.find("id -g") != std::string::npos)
        {
            output = "1000\n";
            remaining = output.size();
            cmd_invoked = true;
        }
        else if (cmd.find("pwd") != std::string::npos)
        {
            output = "/home/ubuntu\n";
            remaining = output.size();
            cmd_invoked = true;
        }
        else if (cmd.find("P=") != std::string::npos)
        {
            output = "/home/ubuntu/\n";
            remaining = output.size();
            cmd_invoked = true;
        }

        return SSH_OK;
    };

    REPLACE(ssh_channel_request_exec, request_exec);

    auto channel_read = make_channel_read_return(output, remaining, cmd_invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    EXPECT_THROW(make_sshfsmount(), std::invalid_argument);
    EXPECT_TRUE(uid_invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_obtain_group_id)
{
    test_failed_invocation("id -g");
}

TEST_F(SshfsMount, throws_when_gid_is_not_an_integer)
{
    bool cmd_invoked{false};
    bool gid_invoked{false};
    std::string output;
    std::string::size_type remaining;

    auto request_exec = [&cmd_invoked, &gid_invoked, &remaining, &output](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};

        if (cmd.find("id -u") != std::string::npos)
        {
            output = "1000\n";
            remaining = output.size();
            cmd_invoked = true;
        }
        if (cmd.find("id -g") != std::string::npos)
        {
            output = "ubuntu\n";
            remaining = output.size();
            gid_invoked = true;
        }
        else if (cmd.find("pwd") != std::string::npos)
        {
            output = "/home/ubuntu\n";
            remaining = output.size();
            cmd_invoked = true;
        }
        else if (cmd.find("P=") != std::string::npos)
        {
            output = "/home/ubuntu/\n";
            remaining = output.size();
            cmd_invoked = true;
        }

        return SSH_OK;
    };

    REPLACE(ssh_channel_request_exec, request_exec);

    auto channel_read = make_channel_read_return(output, remaining, cmd_invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    EXPECT_THROW(make_sshfsmount(), std::invalid_argument);
    EXPECT_TRUE(gid_invoked);
}

TEST_F(SshfsMount, unblocks_when_sftpserver_exits)
{
    bool invoked{false};
    std::string output{"1000\n"};
    auto remaining = output.size();
    auto channel_read = make_channel_read_return(output, remaining, invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    auto request_exec = [&invoked, &remaining, &output](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd.find("id -") != std::string::npos)
        {
            invoked = true;
            output = "1000\n";
            remaining = output.size();
        }
        else if (cmd.find("pwd") != std::string::npos)
        {
            invoked = true;
            output = "/home/ubuntu\n";
            remaining = output.size();
        }
        else if (cmd.find("P=") != std::string::npos)
        {
            invoked = true;
            output = "/home/ubuntu/\n";
            remaining = output.size();
        }
        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    mpt::Signal client_message;
    auto get_client_msg = [&client_message](sftp_session) {
        client_message.wait();
        return nullptr;
    };
    REPLACE(sftp_get_client_message, get_client_msg);

    bool stopped_ok = false;
    std::thread mount([&] {
        auto sshfs_mount = make_sshfsmount(); // blocks until it asked to quit
        stopped_ok = true;
    });

    client_message.signal();

    mount.join();
    EXPECT_TRUE(stopped_ok);
}

TEST_F(SshfsMount, throws_when_unable_to_change_dir)
{
    test_failed_invocation("cd");
}

TEST_F(SshfsMount, invalid_fuse_version_throws)
{
    CommandVector commands = {
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: fu.man.chu\n"},
        {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
         "/home/ubuntu/\n"}};

    EXPECT_THROW(test_command_execution(commands), std::runtime_error);
}

TEST_F(SshfsMount, blank_fuse_version_logs_error)
{
    CommandVector commands = {
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version:\n"},
        {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
         "/home/ubuntu\n"}};

    EXPECT_CALL(*logger, log(Matcher<multipass::logging::Level>(_), Matcher<multipass::logging::CString>(_),
                             Matcher<multipass::logging::CString>(_)))
        .WillRepeatedly(Return());
    EXPECT_CALL(*logger, log(Eq(mpl::Level::warning), make_cstring_matcher(StrEq("sshfs mount")),
                             make_cstring_matcher(StrEq("Unable to parse the FUSE library version"))));
    EXPECT_CALL(*logger,
                log(Eq(mpl::Level::debug), make_cstring_matcher(StrEq("sshfs mount")),
                    make_cstring_matcher(StrEq("Unable to parse the FUSE library version: FUSE library version:"))));

    test_command_execution(commands);
}

TEST_F(SshfsMount, fuse_version_less_than_3_nonempty)
{
    CommandVector commands = {
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: 2.9.0\n"},
        {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
         "/home/ubuntu/\n"},
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -o slave -o transform_symlinks -o "
         "allow_other -o nonempty :\"source\" \"target\"",
         "don't care\n"}};

    test_command_execution(commands);
}

TEST_F(SshfsMount, throws_when_unable_to_get_current_dir)
{
    test_failed_invocation("pwd");
}

TEST_F(SshfsMount, executes_commands)
{
    CommandVector commands = {
        {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
         "/home/ubuntu/\n"},
        {"sudo /bin/bash -c 'cd \"/home/ubuntu/\" && mkdir -p \"target\"'", "\n"},
        {"sudo /bin/bash -c 'cd \"/home/ubuntu/\" && chown -R 1000:1000 target'", "\n"}};

    test_command_execution(commands, std::string("target"));
}

TEST_F(SshfsMount, works_with_absolute_paths)
{
    CommandVector commands = {
        {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
         "/home/ubuntu/\n"}};

    test_command_execution(commands, std::string("/home/ubuntu/target"));
}

TEST_F(SshfsMount, throws_install_sshfs_which_snap_fails)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"which snap"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    mp::SSHSession session{"a", 42};

    EXPECT_THROW(mp::utils::install_sshfs_for("foo", session), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_install_sshfs_no_snap_dir_fails)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"[ -e /snap ]"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    mp::SSHSession session{"a", 42};

    EXPECT_THROW(mp::utils::install_sshfs_for("foo", session), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_install_sshfs_snap_install_fails)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"sudo snap install multipass-sshfs"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    mp::SSHSession session{"a", 42};

    EXPECT_THROW(mp::utils::install_sshfs_for("foo", session), mp::SSHFSMissingError);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, install_sshfs_no_failures_does_not_throw)
{
    mp::SSHSession session{"a", 42};

    EXPECT_NO_THROW(mp::utils::install_sshfs_for("foo", session));
}

TEST_F(SshfsMount, install_sshfs_timeout_logs_info)
{
    ssh_channel_callbacks callbacks{nullptr};
    bool sleep{false};

    auto request_exec = [&sleep](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd == "sudo snap install multipass-sshfs")
            sleep = true;

        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    auto add_channel_cbs = [&callbacks](ssh_channel, ssh_channel_callbacks cb) mutable {
        callbacks = cb;
        return SSH_OK;
    };
    REPLACE(ssh_add_channel_callbacks, add_channel_cbs);

    auto event_dopoll = [&callbacks, &sleep](ssh_event, int timeout) {
        if (!callbacks)
            return SSH_ERROR;

        if (sleep)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout + 1));
        else
            callbacks->channel_exit_status_function(nullptr, nullptr, 0, callbacks->userdata);

        return SSH_OK;
    };
    REPLACE(ssh_event_dopoll, event_dopoll);

    EXPECT_CALL(*logger, log(Matcher<multipass::logging::Level>(_), Matcher<multipass::logging::CString>(_),
                             Matcher<multipass::logging::CString>(_)))
        .WillRepeatedly(Return());
    EXPECT_CALL(*logger, log(Eq(mpl::Level::info), make_cstring_matcher(StrEq("utils")),
                             make_cstring_matcher(StrEq("Timeout while installing 'sshfs' in 'foo'"))));

    mp::SSHSession session{"a", 42};

    mp::utils::install_sshfs_for("foo", session, std::chrono::milliseconds(1));
}
