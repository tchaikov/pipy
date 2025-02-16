/*
 *  Copyright (c) 2019 by flomesh.io
 *
 *  Unless prior written consent has been obtained from the copyright
 *  owner, the following shall not be allowed.
 *
 *  1. The distribution of any source codes, header files, make files,
 *     or libraries of the software.
 *
 *  2. Disclosure of any source codes pertaining to the software to any
 *     additional parties.
 *
 *  3. Alteration or removal of any notices in or on the software or
 *     within the documentation included within the software.
 *
 *  ALL SOURCE CODE AS WELL AS ALL DOCUMENTATION INCLUDED WITH THIS
 *  SOFTWARE IS PROVIDED IN AN “AS IS” CONDITION, WITHOUT WARRANTY OF ANY
 *  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "version.h"

#include "admin-link.hpp"
#include "admin-service.hpp"
#include "admin-proxy.hpp"
#include "api/crypto.hpp"
#include "api/logging.hpp"
#include "api/stats.hpp"
#include "codebase.hpp"
#include "file.hpp"
#include "fs.hpp"
#include "filters/tls.hpp"
#include "input.hpp"
#include "listener.hpp"
#include "main-options.hpp"
#include "net.hpp"
#include "status.hpp"
#include "timer.hpp"
#include "utils.hpp"
#include "worker.hpp"

#include <signal.h>

#include <list>
#include <string>
#include <tuple>

#include <openssl/opensslv.h>

using namespace pipy;

static AdminService *s_admin = nullptr;
static AdminProxy *s_admin_proxy = nullptr;
static AdminLink *s_admin_link = nullptr;
static std::string s_admin_ip;
static int s_admin_port = 0;
static AdminService::Options s_admin_options;

//
// Show version
//

static void show_version() {
  std::cout << "Version     : " << PIPY_VERSION << std::endl;
  std::cout << "Commit      : " << PIPY_COMMIT << std::endl;
  std::cout << "Commit Date : " << PIPY_COMMIT_DATE << std::endl;
  std::cout << "Host        : " << PIPY_HOST << std::endl;
  std::cout << "OpenSSL     : " << OPENSSL_VERSION_TEXT << std::endl;

#ifdef PIPY_USE_GUI
  std::cout << "Builtin GUI : " << "Yes" << std::endl;
#else
  std::cout << "Builtin GUI : " << "No" << std::endl;
#endif

#ifdef PIPY_USE_SAMPLES
  std::cout << "Samples     : " << "Yes" << std::endl;
#else
  std::cout << "Samples     : " << "No" << std::endl;
#endif
}
//
// Reload codebase
//

static void reload_codebase() {
  if (auto *codebase = Codebase::current()) {
    Codebase::current()->sync(
      Status::local, true,
      [](bool succ) {
        if (succ) Worker::restart();
      }
    );
  }
}

//
// Establish admin link
//

static void start_admin_link(const std::string &url) {
  std::string url_path = url;
  if (url_path.back() != '/') url_path += '/';
  url_path += Status::local.uuid;
  s_admin_link = new AdminLink(url_path);
  s_admin_link->add_handler(
    [](const std::string &command, const Data &) {
      if (command == "reload") {
        reload_codebase();
        return true;
      } else {
        return false;
      }
    }
  );
  logging::Logger::set_admin_link(s_admin_link);
}

//
// Periodically clean up pools
//

static void start_cleaning_pools() {
  static Timer timer;
  static std::function<void()> clean;
  clean = []() {
    for (const auto &p : pjs::PooledClass::all()) {
      p.second->clean();
    }
    timer.schedule(5, clean);
  };
  clean();
}

//
// Periodically check codebase updates
//

static void start_checking_updates() {
  static Timer timer;
  static std::function<void()> poll;
  poll = []() {
    if (!Worker::exited()) {
      InputContext ic;
      Status::local.timestamp = utils::now();
      Codebase::current()->sync(
        Status::local, false,
        [&](bool ok) {
          if (ok) {
            Worker::restart();
          }
        }
      );
    }
    timer.schedule(5, poll);
  };
  poll();
}

//
// Periodically report metrics
//

static void start_reporting_metrics() {
  static Data::Producer s_dp("Metric Reports");
  static Timer timer;
  static std::function<void()> report;
  static int connection_id = 0;
  report = []() {
    if (!Worker::exited()) {
      InputContext ic;
      auto conn_id = s_admin_link->connect();
      Data buf; buf.push("metrics\n", &s_dp);
      stats::Metric::local().collect_all();
      stats::Metric::local().serialize(buf, Status::local.uuid, conn_id != connection_id);
      s_admin_link->send(buf);
      connection_id = conn_id;
    }
    timer.schedule(5, report);
  };
  report();
}

//
// Open/close admin port
//

static void toggle_admin_port() {
  if (s_admin_port) {
    if (s_admin) {
      logging::Logger::set_admin_service(nullptr);
      s_admin->close();
      delete s_admin;
      s_admin = nullptr;
      Log::info("[admin] Admin service stopped on port %d", s_admin_port);
    } else {
      s_admin = new AdminService(nullptr);
      s_admin->open(s_admin_ip, s_admin_port, s_admin_options);
      logging::Logger::set_admin_service(s_admin);
    }
  }
}

//
// Handle signals
//

static void handle_signal(int sig) {
  static bool s_admin_closed = false;

  if (auto worker = Worker::current()) {
    if (worker->handling_signal(sig)) {
      return;
    }
  }

  switch (sig) {
    case SIGINT:
      if (!s_admin_closed) {
        logging::Logger::shutdown_all();
        if (s_admin_link) s_admin_link->close();
        if (s_admin) s_admin->close();
        s_admin_closed = true;
      }
      Worker::exit(-1);
      break;
    case SIGHUP:
      reload_codebase();
      break;
    case SIGTSTP:
      toggle_admin_port();
      break;
  }
}

//
// Wait for signals
//

static void wait_for_signals(asio::signal_set &signals) {
  signals.async_wait(
    [&](const std::error_code &ec, int sig) {
      if (!ec) handle_signal(sig);
      wait_for_signals(signals);
    }
  );
}

//
// Program entrance
//

int main(int argc, char *argv[]) {
  try {
    MainOptions opts(argc, argv);

    if (opts.version) {
      show_version();
      return 0;
    }

    if (opts.help) {
      MainOptions::show_help();
      return 0;
    }

    Status::local.timestamp = utils::now();
    Status::local.name = opts.instance_name;

    if (opts.instance_uuid.empty()) {
      utils::gen_uuid_v4(Status::local.uuid);
    } else {
      Status::local.uuid = opts.instance_uuid;
    }

    pjs::Math::init();
    Log::init();
    Log::set_level(opts.log_level);
    Log::set_graph_enabled(!opts.no_graph);
    Status::register_metrics();
    Listener::set_reuse_port(opts.reuse_port);
    crypto::Crypto::init(opts.openssl_engine);
    tls::TLSSession::init();
    File::start_bg_thread();

    s_admin_options.cert = opts.admin_tls_cert;
    s_admin_options.key = opts.admin_tls_key;
    s_admin_options.trusted = opts.admin_tls_trusted;

    std::string admin_ip("::");
    int admin_port = 6060; // default repo port
    auto admin_ip_port = opts.admin_port;
    if (!admin_ip_port.empty()) {
      if (!utils::get_host_port(admin_ip_port, admin_ip, admin_port)) {
        admin_port = std::atoi(admin_ip_port.c_str());
      }
      if (admin_ip.empty()) admin_ip = "::";
    }

    bool is_eval = false;
    bool is_repo = false;
    bool is_repo_proxy = false;
    bool is_remote = false;
    bool is_tls = false;

    if (opts.eval) {
      is_eval = true;

    } else if (opts.filename.empty()) {
      is_repo = true;

    } else if (utils::starts_with(opts.filename, "http://")) {
      is_remote = true;

    } else if (utils::starts_with(opts.filename, "https://")) {
      is_remote = true;
      is_tls = true;

    } else if (utils::is_host_port(opts.filename)) {
      is_repo_proxy = true;

    } else {
      auto full_path = fs::abs_path(opts.filename);
      opts.filename = full_path;
      if (!fs::exists(full_path)) {
        std::string msg("file or directory does not exist: ");
        throw std::runtime_error(msg + full_path);
      }
      is_repo = fs::is_dir(full_path);
    }

    if (is_remote) {
      auto i = opts.filename.find('/');
      auto target = opts.filename.substr(i+2);
      if (!target.empty() && target.back() == '/') {
        target.resize(target.size() - 1);
      }
      if (utils::is_host_port(target)) {
        opts.filename = target;
        is_remote = false;
        is_repo_proxy = true;
      }
    }

    Store *store = nullptr;
    CodebaseStore *repo = nullptr;
    Codebase *codebase = nullptr;

    std::function<void()> load, fail;
    Timer retry_timer;

    // Start as codebase repo service
    if (is_repo) {
      store = opts.filename.empty()
        ? Store::open_memory()
        : Store::open_level_db(opts.filename);
      repo = new CodebaseStore(store);
      s_admin = new AdminService(repo);
      s_admin->open(admin_ip, admin_port, s_admin_options);
      logging::Logger::set_admin_service(s_admin);

#ifdef PIPY_USE_GUI
      std::cout << std::endl;
      std::cout << "=============================================" << std::endl;
      std::cout << std::endl;
      std::cout << "  You can now view Pipy GUI in the browser:" << std::endl;
      std::cout << std::endl;
      std::cout << "    http://localhost:" << admin_port << '/' << std::endl;
      std::cout << std::endl;
      std::cout << "=============================================" << std::endl;
      std::cout << std::endl;
#endif

    // Start as codebase repo proxy
    } else if (is_repo_proxy) {
      AdminProxy::Options options;
      options.cert = opts.admin_tls_cert;
      options.key = opts.admin_tls_key;
      options.trusted = opts.admin_tls_trusted;
      options.fetch_options.tls = is_tls;
      options.fetch_options.cert = opts.tls_cert;
      options.fetch_options.key = opts.tls_key;
      options.fetch_options.trusted = opts.tls_trusted;
      s_admin_proxy = new AdminProxy(opts.filename);
      s_admin_proxy->open(admin_ip, admin_port, options);

    // Start as a fixed codebase
    } else {
      if (is_remote) {
        Fetch::Options options;
        options.tls = is_tls;
        options.cert = opts.tls_cert;
        options.key = opts.tls_key;
        options.trusted = opts.tls_trusted;
        codebase = Codebase::from_http(opts.filename, options);
      } else if (is_eval) {
        codebase = Codebase::from_fs(
          fs::abs_path("."),
          opts.filename
        );
      } else {
        codebase = Codebase::from_fs(opts.filename);
      }

      codebase->set_current();

      load = [&]() {
        codebase->sync(
          Status::local, true,
          [&](bool ok) {
            if (!ok) {
              fail();
              return;
            }

            auto &entry = Codebase::current()->entry();
            auto worker = Worker::make();
            auto mod = worker->load_js_module(entry);

            if (!mod) {
              fail();
              return;
            }

            if (opts.verify) {
              Worker::exit(0);
              return;
            }

            if (!worker->start()) {
              fail();
              return;
            }

            Status::local.version = Codebase::current()->version();
            Status::local.update();

            s_admin_ip = admin_ip;
            s_admin_port = admin_port;

            if (!opts.admin_port.empty()) toggle_admin_port();

            Log::set_graph_enabled(false);
            start_checking_updates();

            if (is_remote) {
              start_admin_link(opts.filename);
              start_reporting_metrics();
            }
          }
        );
      };

      fail = [&]() {
        if (is_remote) {
          retry_timer.schedule(5, load);
        } else {
          Worker::exit(-1);
        }
      };

      load();
    }

    asio::signal_set signals(Net::context());
    signals.add(SIGINT);
    signals.add(SIGHUP);
    signals.add(SIGTSTP);
    wait_for_signals(signals);

    start_cleaning_pools();

    Net::run();

    File::stop_bg_thread();

    delete s_admin_link;
    delete s_admin;
    delete s_admin_proxy;
    delete repo;

    if (store) store->close();

    crypto::Crypto::free();
    Log::shutdown();

    std::cerr << "Done." << std::endl;

  } catch (std::runtime_error &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  return Worker::exit_code();
}
