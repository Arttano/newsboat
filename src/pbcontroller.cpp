#include "pbcontroller.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "colormanager.h"
#include "config.h"
#include "configcontainer.h"
#include "exceptions.h"
#include "globals.h"
#include "keymap.h"
#include "logger.h"
#include "pbview.h"
#include "poddlthread.h"
#include "queueloader.h"
#include "strprintf.h"
#include "utils.h"

using namespace newsboat;

static void ctrl_c_action(int sig)
{
	LOG(Level::DEBUG, "caugh signal %d", sig);
	Stfl::reset();
	::exit(EXIT_FAILURE);
}

namespace podboat {

/**
 * \brief Try to setup XDG style dirs.
 *
 * returns false, if that fails
 */
bool PbController::setup_dirs_xdg(const char* env_home)
{
	const char* env_xdg_config;
	const char* env_xdg_data;
	std::string xdg_config_dir;
	std::string xdg_data_dir;

	env_xdg_config = ::getenv("XDG_CONFIG_HOME");
	if (env_xdg_config) {
		xdg_config_dir = env_xdg_config;
	} else {
		xdg_config_dir = env_home;
		xdg_config_dir.append(NEWSBEUTER_PATH_SEP);
		xdg_config_dir.append(".config");
	}

	env_xdg_data = ::getenv("XDG_DATA_HOME");
	if (env_xdg_data) {
		xdg_data_dir = env_xdg_data;
	} else {
		xdg_data_dir = env_home;
		xdg_data_dir.append(NEWSBEUTER_PATH_SEP);
		xdg_data_dir.append(".local");
		xdg_data_dir.append(NEWSBEUTER_PATH_SEP);
		xdg_data_dir.append("share");
	}

	xdg_config_dir.append(NEWSBEUTER_PATH_SEP);
	xdg_config_dir.append(NEWSBOAT_SUBDIR_XDG);

	xdg_data_dir.append(NEWSBEUTER_PATH_SEP);
	xdg_data_dir.append(NEWSBOAT_SUBDIR_XDG);

	bool config_dir_exists =
		0 == access(xdg_config_dir.c_str(), R_OK | X_OK);

	if (!config_dir_exists) {
		std::cerr << StrPrintf::fmt(
				     _("XDG: configuration directory '%s' not "
				       "accessible, "
				       "using '%s' instead."),
				     xdg_config_dir,
				     config_dir)
			  << std::endl;

		return false;
	}

	/* Invariant: config dir exists.
	 *
	 * At this point, we're confident we'll be using XDG. We don't check if
	 * data dir exists, because if it doesn't we'll create it. */

	config_dir = xdg_config_dir;

	// create data directory if it doesn't exist
	int ret = Utils::mkdir_parents(xdg_data_dir, 0700);
	if (ret && errno != EEXIST) {
		LOG(Level::CRITICAL,
			"Couldn't create `%s': (%i) %s",
			xdg_data_dir,
			errno,
			strerror(errno));
		::exit(EXIT_FAILURE);
	}

	/* in config */
	url_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + url_file;
	config_file =
		config_dir + std::string(NEWSBEUTER_PATH_SEP) + config_file;

	/* in data */
	cache_file =
		xdg_data_dir + std::string(NEWSBEUTER_PATH_SEP) + cache_file;
	lock_file = cache_file + LOCK_SUFFIX;
	queue_file =
		xdg_data_dir + std::string(NEWSBEUTER_PATH_SEP) + queue_file;
	searchfile = StrPrintf::fmt(
		"%s%shistory.search", xdg_data_dir, NEWSBEUTER_PATH_SEP);
	cmdlinefile = StrPrintf::fmt(
		"%s%shistory.cmdline", xdg_data_dir, NEWSBEUTER_PATH_SEP);

	return true;
}

PbController::PbController()
	: v(0)
	, config_file("config")
	, queue_file("queue")
	, cfg(0)
	, view_update_(true)
	, max_dls(1)
	, ql(0)
	, lock_file("pb-lock.pid")
{
	char* cfgdir;
	if (!(cfgdir = ::getenv("HOME"))) {
		struct passwd* spw = ::getpwuid(::getuid());
		if (spw) {
			cfgdir = spw->pw_dir;
		} else {
			std::cout << _("Fatal error: couldn't determine home "
				       "directory!")
				  << std::endl;
			std::cout << StrPrintf::fmt(
					     _("Please set the HOME "
					       "environment variable or add a "
					       "valid user for UID %u!"),
					     ::getuid())
				  << std::endl;
			::exit(EXIT_FAILURE);
		}
	}
	config_dir = cfgdir;

	if (setup_dirs_xdg(cfgdir))
		return;

	config_dir.append(NEWSBEUTER_PATH_SEP);
	config_dir.append(NEWSBOAT_CONFIG_SUBDIR);

	// create configuration directory if it doesn't exist
	int ret = ::mkdir(config_dir.c_str(), 0700);
	if (ret && errno != EEXIST) {
		std::cerr << StrPrintf::fmt(
				     _("Fatal error: couldn't create "
				       "configuration directory `%s': (%i) %s"),
				     config_dir,
				     errno,
				     std::strerror(errno))
			  << std::endl;
		::exit(EXIT_FAILURE);
	}

	config_file =
		config_dir + std::string(NEWSBEUTER_PATH_SEP) + config_file;
	queue_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + queue_file;
	lock_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + lock_file;
}

PbController::~PbController()
{
	delete cfg;
}

int PbController::run(int argc, char* argv[])
{
	int c;
	bool automatic_dl = false;

	::signal(SIGINT, ctrl_c_action);

	static const char getopt_str[] = "C:q:d:l:havV";
	static const struct option longopts[] = {
		{"config-file", required_argument, 0, 'C'},
		{"queue-file", required_argument, 0, 'q'},
		{"log-file", required_argument, 0, 'd'},
		{"log-level", required_argument, 0, 'l'},
		{"help", no_argument, 0, 'h'},
		{"autodownload", no_argument, 0, 'a'},
		{"version", no_argument, 0, 'v'},
		{0, 0, 0, 0}};

	while ((c = ::getopt_long(argc, argv, getopt_str, longopts, nullptr)) !=
		-1) {
		switch (c) {
		case ':':
		case '?':
			print_usage(argv[0]);
			return EXIT_FAILURE;
		case 'C':
			config_file = optarg;
			break;
		case 'q':
			queue_file = optarg;
			break;
		case 'a':
			automatic_dl = true;
			break;
		case 'd':
			Logger::getInstance().set_logfile(optarg);
			break;
		case 'l': {
			Level l = static_cast<Level>(atoi(optarg));
			if (l > Level::NONE && l <= Level::DEBUG) {
				Logger::getInstance().set_loglevel(l);
			} else {
				std::cerr << StrPrintf::fmt(_("%s: %d: invalid "
							      "loglevel value"),
						     argv[0],
						     l)
					  << std::endl;
				return EXIT_FAILURE;
			}
		} break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		}
	};

	std::cout << StrPrintf::fmt(
			     _("Starting %s %s..."), "podboat", PROGRAM_VERSION)
		  << std::endl;

	fslock = std::unique_ptr<FsLock>(new FsLock());
	pid_t pid;
	if (!fslock->try_lock(lock_file, pid)) {
		std::cout << StrPrintf::fmt(
				     _("Error: an instance of %s is already "
				       "running (PID: %u)"),
				     "podboat",
				     pid)
			  << std::endl;
		return EXIT_FAILURE;
	}

	std::cout << _("Loading configuration...");
	std::cout.flush();

	ConfigParser cfgparser;
	cfg = new ConfigContainer();
	cfg->register_commands(cfgparser);
	ColorManager* colorman = new ColorManager();
	colorman->register_commands(cfgparser);

	KeyMap keys(KM_PODBOAT);
	cfgparser.register_handler("bind-key", &keys);
	cfgparser.register_handler("unbind-key", &keys);

	NullConfigActionHandler null_cah;
	cfgparser.register_handler("macro", &null_cah);
	cfgparser.register_handler("ignore-article", &null_cah);
	cfgparser.register_handler("always-download", &null_cah);
	cfgparser.register_handler("define-filter", &null_cah);
	cfgparser.register_handler("highlight", &null_cah);
	cfgparser.register_handler("highlight-article", &null_cah);
	cfgparser.register_handler("reset-unread-on-update", &null_cah);

	try {
		cfgparser.parse("/etc/newsboat/config");
		cfgparser.parse(config_file);
	} catch (const ConfigException& ex) {
		std::cout << ex.what() << std::endl;
		delete colorman;
		return EXIT_FAILURE;
	}

	if (colorman->colors_loaded())
		colorman->set_pb_colors(v);
	delete colorman;

	max_dls = cfg->get_configvalue_as_int("max-downloads");

	std::cout << _("done.") << std::endl;

	ql = new QueueLoader(queue_file, this);
	ql->reload(downloads_);

	v->set_keymap(&keys);

	v->run(automatic_dl);

	Stfl::reset();

	std::cout << _("Cleaning up queue...");
	std::cout.flush();

	ql->reload(downloads_);
	delete ql;

	std::cout << _("done.") << std::endl;

	return EXIT_SUCCESS;
}

void PbController::print_usage(const char* argv0)
{
	auto msg = StrPrintf::fmt(
		_("%s %s\nusage %s [-C <file>] [-q <file>] [-h]\n"),
		"podboat",
		PROGRAM_VERSION,
		argv0);
	std::cout << msg;

	struct Arg {
		const char name;
		const std::string longname;
		const std::string params;
		const std::string desc;
	};

	static const std::vector<Arg> args = {
		{'C',
			"config-file",
			_s("<configfile>"),
			_s("read configuration from <configfile>")},
		{'q',
			"queue-file",
			_s("<queuefile>"),
			_s("use <queuefile> as queue file")},
		{'a', "autodownload", "", _s("start download on startup")},
		{'l',
			"log-level",
			_s("<loglevel>"),
			_s("write a log with a certain loglevel (valid values: "
			   "1 to "
			   "6)")},
		{'d',
			"log-file",
			_s("<logfile>"),
			_s("use <logfile> as output log file")},
		{'h', "help", "", _s("this help")}};

	for (const auto& a : args) {
		std::string longcolumn("-");
		longcolumn += a.name;
		longcolumn += ", --" + a.longname;
		longcolumn += a.params.size() > 0 ? "=" + a.params : "";
		std::cout << "\t" << longcolumn;
		for (unsigned int j = 0; j < Utils::gentabs(longcolumn); j++) {
			std::cout << "\t";
		}
		std::cout << a.desc << std::endl;
	}
}

std::string PbController::get_dlpath()
{
	return cfg->get_configvalue("download-path");
}

std::string PbController::get_formatstr()
{
	return cfg->get_configvalue("podlist-format");
}

unsigned int PbController::downloads_in_progress()
{
	unsigned int count = 0;
	for (const auto& dl : downloads_) {
		if (dl.status() == DlStatus::DOWNLOADING)
			++count;
	}
	return count;
}

unsigned int PbController::get_maxdownloads()
{
	return max_dls;
}

void PbController::reload_queue(bool remove_unplayed)
{
	if (ql) {
		ql->reload(downloads_, remove_unplayed);
	}
}

double PbController::get_total_kbps()
{
	double result = 0.0;
	for (const auto& dl : downloads_) {
		if (dl.status() == DlStatus::DOWNLOADING) {
			result += dl.kbps();
		}
	}
	return result;
}

void PbController::start_downloads()
{
	int dl2start = get_maxdownloads() - downloads_in_progress();
	for (auto& download : downloads_) {
		if (dl2start == 0)
			break;

		if (download.status() == DlStatus::QUEUED) {
			std::thread t{PodDlThread(&download, cfg)};
			--dl2start;
			t.detach();
		}
	}
}

void PbController::increase_parallel_downloads()
{
	++max_dls;
}

void PbController::decrease_parallel_downloads()
{
	if (max_dls > 1)
		--max_dls;
}

void PbController::play_file(const std::string& file)
{
	std::string cmdline;
	std::string player = cfg->get_configvalue("player");
	if (player == "")
		return;
	cmdline.append(player);
	cmdline.append(" '");
	cmdline.append(Utils::replace_all(file, "'", "%27"));
	cmdline.append("'");
	Stfl::reset();
	Utils::run_interactively(cmdline, "PbController::play_file");
}

} // namespace podboat