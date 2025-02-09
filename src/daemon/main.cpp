// Copyright (c) 2014-2016, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include "common/command_line.h"
#include "common/scoped_message_writer.h"
#include "common/util.h"
#include "common/stack_trace.h"
#include "cryptonote_core/cryptonote_core.h"
#include "cryptonote_core/miner.h"
#include "daemon/command_server.h"
#include "daemon/daemon.h"
#include "daemon/executor.h"
#include "daemonizer/daemonizer.h"
#include "misc_log_ex.h"
#include "p2p/net_node.h"
#include "rpc/core_rpc_server.h"
#include "daemon/command_line_args.h"
#include "blockchain_db/db_types.h"

namespace po = boost::program_options;
namespace bf = boost::filesystem;

int main(int argc, char const * argv[])
{
  try {

    _note_c("dbg/main", "Begin of main()");
    // TODO parse the debug options like set log level right here at start

    tools::sanitize_locale();

    epee::string_tools::set_module_name_and_folder(argv[0]);

    // Build argument description
    po::options_description all_options("All");
    po::options_description hidden_options("Hidden");
    po::options_description visible_options("Options");
    po::options_description core_settings("Settings");
    po::positional_options_description positional_options;
    {
      bf::path default_data_dir = daemonizer::get_default_data_dir();
      bf::path default_testnet_data_dir = {default_data_dir / "testnet"};

      // Misc Options

      command_line::add_arg(visible_options, command_line::arg_help);
      command_line::add_arg(visible_options, command_line::arg_version);
      command_line::add_arg(visible_options, daemon_args::arg_os_version);
      bf::path default_conf = default_data_dir / std::string(CRYPTONOTE_NAME ".conf");
      command_line::add_arg(visible_options, daemon_args::arg_config_file, default_conf.string());
      command_line::add_arg(visible_options, command_line::arg_test_dbg_lock_sleep);
      cryptonote::core::init_options(core_settings);

      // Settings
      bf::path default_log = default_data_dir / std::string(CRYPTONOTE_NAME ".log");
      command_line::add_arg(core_settings, daemon_args::arg_log_file, default_log.string());
      command_line::add_arg(core_settings, daemon_args::arg_log_level);
      command_line::add_arg(core_settings, daemon_args::arg_max_concurrency);

      daemonizer::init_options(hidden_options, visible_options);
      daemonize::t_executor::init_options(core_settings);

      // Hidden options
      command_line::add_arg(hidden_options, daemon_args::arg_command);

      visible_options.add(core_settings);
      all_options.add(visible_options);
      all_options.add(hidden_options);

      // Positional
      positional_options.add(daemon_args::arg_command.name, -1); // -1 for unlimited arguments
    }

    // Do command line parsing
    po::variables_map vm;
    bool ok = command_line::handle_error_helper(visible_options, [&]()
    {
      boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv)
          .options(all_options).positional(positional_options).run()
      , vm
      );

      return true;
    });
    if (!ok) return 1;

    if (command_line::get_arg(vm, command_line::arg_help))
    {
      std::cout << "Monero '" << MONERO_RELEASE_NAME << "' (v" << MONERO_VERSION_FULL << ")" << ENDL << ENDL;
      std::cout << "Usage: " + std::string{argv[0]} + " [options|settings] [daemon_command...]" << std::endl << std::endl;
      std::cout << visible_options << std::endl;
      return 0;
    }

    // Monero Version
    if (command_line::get_arg(vm, command_line::arg_version))
    {
      std::cout << "Monero '" << MONERO_RELEASE_NAME << "' (v" << MONERO_VERSION_FULL << ")" << ENDL;
      return 0;
    }

    // OS
    if (command_line::get_arg(vm, daemon_args::arg_os_version))
    {
      std::cout << "OS: " << tools::get_os_version_string() << ENDL;
      return 0;
    }

    epee::debug::g_test_dbg_lock_sleep() = command_line::get_arg(vm, command_line::arg_test_dbg_lock_sleep);

    std::string db_type = command_line::get_arg(vm, command_line::arg_db_type);

    // verify that blockchaindb type is valid
    if(cryptonote::blockchain_db_types.count(db_type) == 0)
    {
      std::cout << "Invalid database type (" << db_type << "), available types are:" << std::endl;
      for (const auto& type : cryptonote::blockchain_db_types)
      {
        std::cout << "\t" << type << std::endl;
      }
      return 0;
    }

    bool testnet_mode = command_line::get_arg(vm, command_line::arg_testnet_on);

    auto data_dir_arg = testnet_mode ? command_line::arg_testnet_data_dir : command_line::arg_data_dir;

    // data_dir
    //   default: e.g. ~/.bitmonero/ or ~/.bitmonero/testnet
    //   if data-dir argument given:
    //     absolute path
    //     relative path: relative to cwd

    // Create data dir if it doesn't exist
    boost::filesystem::path data_dir = boost::filesystem::absolute(
        command_line::get_arg(vm, data_dir_arg));
    tools::create_directories_if_necessary(data_dir.string());

    // FIXME: not sure on windows implementation default, needs further review
    //bf::path relative_path_base = daemonizer::get_relative_path_base(vm);
    bf::path relative_path_base = data_dir;

    std::string config = command_line::get_arg(vm, daemon_args::arg_config_file);

    boost::filesystem::path data_dir_path(data_dir);
    boost::filesystem::path config_path(config);
    if (!config_path.has_parent_path())
    {
      config_path = data_dir / config_path;
    }

    boost::system::error_code ec;
    if (bf::exists(config_path, ec))
    {
      po::store(po::parse_config_file<char>(config_path.string<std::string>().c_str(), core_settings), vm);
    }
    po::notify(vm);

    // If there are positional options, we're running a daemon command
    {
      auto command = command_line::get_arg(vm, daemon_args::arg_command);

      if (command.size())
      {
        auto rpc_ip_str = command_line::get_arg(vm, cryptonote::core_rpc_server::arg_rpc_bind_ip);
        auto rpc_port_str = command_line::get_arg(vm, cryptonote::core_rpc_server::arg_rpc_bind_port);
        if (testnet_mode)
        {
          rpc_port_str = command_line::get_arg(vm, cryptonote::core_rpc_server::arg_testnet_rpc_bind_port);
        }

        uint32_t rpc_ip;
        uint16_t rpc_port;
        if (!epee::string_tools::get_ip_int32_from_string(rpc_ip, rpc_ip_str))
        {
          std::cerr << "Invalid IP: " << rpc_ip_str << std::endl;
          return 1;
        }
        if (!epee::string_tools::get_xtype_from_string(rpc_port, rpc_port_str))
        {
          std::cerr << "Invalid port: " << rpc_port_str << std::endl;
          return 1;
        }

        daemonize::t_command_server rpc_commands{rpc_ip, rpc_port};
        if (rpc_commands.process_command_vec(command))
        {
          return 0;
        }
        else
        {
          std::cerr << "Unknown command" << std::endl;
          return 1;
        }
      }
    }

    // Start with log level 0
    epee::log_space::get_set_log_detalisation_level(true, LOG_LEVEL_0);

    // Set log level
    {
      int new_log_level = command_line::get_arg(vm, daemon_args::arg_log_level);
      if(new_log_level < LOG_LEVEL_MIN || new_log_level > LOG_LEVEL_MAX)
      {
        LOG_PRINT_L0("Wrong log level value: " << new_log_level);
      }
      else if (epee::log_space::get_set_log_detalisation_level(false) != new_log_level)
      {
        epee::log_space::get_set_log_detalisation_level(true, new_log_level);
        int otshell_utils_log_level = 100 - (new_log_level * 20);
        gCurrentLogger.setDebugLevel(otshell_utils_log_level);
        LOG_PRINT_L0("LOG_LEVEL set to " << new_log_level);
      }
    }

    // log_file_path
    //   default: <data_dir>/<CRYPTONOTE_NAME>.log
    //   if log-file argument given:
    //     absolute path
    //     relative path: relative to data_dir

    // Set log file
    {
      bf::path log_file_path {data_dir / std::string(CRYPTONOTE_NAME ".log")};
      if (! vm["log-file"].defaulted())
        log_file_path = command_line::get_arg(vm, daemon_args::arg_log_file);
      log_file_path = bf::absolute(log_file_path, relative_path_base);

      epee::log_space::log_singletone::add_logger(
          LOGGER_FILE
        , log_file_path.filename().string().c_str()
        , log_file_path.parent_path().string().c_str()
        );
      tools::set_stack_trace_log(log_file_path.filename().string());
    }

    if (command_line::has_arg(vm, daemon_args::arg_max_concurrency))
      tools::set_max_concurrency(command_line::get_arg(vm, daemon_args::arg_max_concurrency));

    _note_c("dbg/main", "Moving from main() into the daemonize now.");

    return daemonizer::daemonize(argc, argv, daemonize::t_executor{}, vm);
  }
  catch (std::exception const & ex)
  {
    LOG_ERROR("Exception in main! " << ex.what());
  }
  catch (...)
  {
    LOG_ERROR("Exception in main!");
  }
  return 1;
}
