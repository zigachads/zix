#include "logging.hh"
#include "file-descriptor.hh"
#include "environment-variables.hh"
#include "terminal.hh"
#include "util.hh"
#include "config-global.hh"
#include "source-path.hh"
#include "position.hh"

#include <atomic>
#include <sstream>
#include <iostream>

namespace nix {

LoggerSettings loggerSettings;

static GlobalConfig::Register rLoggerSettings(&loggerSettings);

static thread_local ActivityId curActivity = 0;

ActivityId getCurActivity()
{
    return curActivity;
}
void setCurActivity(const ActivityId activityId)
{
    curActivity = activityId;
}

Logger * logger = makeSimpleLogger(true);

void Logger::warn(const std::string & msg)
{
    log(lvlWarn, ANSI_WARNING "warning:" ANSI_NORMAL " " + msg);
}

void Logger::writeToStdout(std::string_view s)
{
    Descriptor standard_out = getStandardOutput();
    writeFull(standard_out, s);
    writeFull(standard_out, "\n");
}

class SimpleLogger : public Logger
{
public:

    bool systemd, tty;
    bool printBuildLogs;

    SimpleLogger(bool printBuildLogs)
        : printBuildLogs(printBuildLogs)
    {
        systemd = getEnv("IN_SYSTEMD") == "1";
        tty = isTTY();
    }

    bool isVerbose() override {
        return printBuildLogs;
    }

    void log(Verbosity lvl, std::string_view s) override
    {
        if (lvl > verbosity) return;

        std::string prefix;

        if (systemd) {
            char c;
            switch (lvl) {
            case lvlError: c = '3'; break;
            case lvlWarn: c = '4'; break;
            case lvlNotice: case lvlInfo: c = '5'; break;
            case lvlTalkative: case lvlChatty: c = '6'; break;
            case lvlDebug: case lvlVomit: c = '7'; break;
            default: c = '7'; break; // should not happen, and missing enum case is reported by -Werror=switch-enum
            }
            prefix = std::string("<") + c + ">";
        }

        writeToStderr(prefix + filterANSIEscapes(s, !tty) + "\n");
    }

    void logEI(const ErrorInfo & ei) override
    {
        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace.get());

        log(ei.level, toView(oss));
    }

    void startActivity(ActivityId act, Verbosity lvl, ActivityType type,
        const std::string & s, const Fields & fields, ActivityId parent)
        override
    {
        if (lvl <= verbosity && !s.empty())
            log(lvl, s + "...");
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        if (type == resBuildLogLine && printBuildLogs) {
            auto lastLine = fields[0].s;
            printError(lastLine);
        }
        else if (type == resPostBuildLogLine && printBuildLogs) {
            auto lastLine = fields[0].s;
            printError("post-build-hook: " + lastLine);
        }
    }
};

Verbosity verbosity = lvlInfo;

void writeToStderr(std::string_view s)
{
    try {
        writeFull(
            getStandardError(),
            s, false);
    } catch (SystemError & e) {
        /* Ignore failing writes to stderr.  We need to ignore write
           errors to ensure that cleanup code that logs to stderr runs
           to completion if the other side of stderr has been closed
           unexpectedly. */
    }
}

Logger * makeSimpleLogger(bool printBuildLogs)
{
    return new SimpleLogger(printBuildLogs);
}

std::atomic<uint64_t> nextId{0};

static uint64_t getPid()
{
#ifndef _WIN32
    return getpid();
#else
    return GetCurrentProcessId();
#endif
}

Activity::Activity(Logger & logger, Verbosity lvl, ActivityType type,
    const std::string & s, const Logger::Fields & fields, ActivityId parent)
    : logger(logger), id(nextId++ + (((uint64_t) getPid()) << 32))
{
    logger.startActivity(id, lvl, type, s, fields, parent);
}

void to_json(json::value * json, std::shared_ptr<Pos> pos)
{
    if (pos) {
        nix_libutil_json_object_set_integer(json, "line", pos->line);
        nix_libutil_json_object_set_integer(json, "column", pos->column);
        std::ostringstream str;
        pos->print(str, true);
        nix_libutil_json_object_set_string(json, "file", str.str().c_str());
    }
}

struct JSONLogger : Logger {
    Logger & prevLogger;

    JSONLogger(Logger & prevLogger) : prevLogger(prevLogger) { }

    bool isVerbose() override {
        return true;
    }

    void addFields(json::value * json, const Fields & fields)
    {
        if (fields.empty()) return;
        nix_libutil_json_object_set(json, "fields", nix_libutil_json_list_new());
        auto *arr = nix_libutil_json_object_get(json, "fields");
        for (auto & f : fields)
            if (f.type == Logger::Field::tInt)
                nix_libutil_json_list_insert(arr, nix_libutil_json_integer_new(f.i));
            else if (f.type == Logger::Field::tString)
                nix_libutil_json_list_insert(arr, nix_libutil_json_string_new(f.s.c_str()));
            else
                unreachable();
    }

    void write(const json::value * json)
    {
        //FIXME:prevLogger.log(lvlError, "@nix " + json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    void log(Verbosity lvl, std::string_view s) override
    {
        json::value * json = nix_libutil_json_object_new();
        nix_libutil_json_object_set_string(json, "action", "msg");
        nix_libutil_json_object_set_integer(json, "level", lvl);
        nix_libutil_json_object_set_string(json, "msg", s.data());
        write(json);
    }

    void logEI(const ErrorInfo & ei) override
    {
        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace.get());

        json::value *json = nix_libutil_json_object_new();
        nix_libutil_json_object_set_string(json, "action", "msg");
        nix_libutil_json_object_set_integer(json, "level", ei.level);
        nix_libutil_json_object_set_string(json, "msg", oss.str().c_str());
        nix_libutil_json_object_set_string(json, "raw_msg", ei.msg.str().c_str());
        to_json(json, ei.pos);

        if (loggerSettings.showTrace.get() && !ei.traces.empty()) {
            json::value * traces = nix_libutil_json_list_new();
            for (auto iter = ei.traces.rbegin(); iter != ei.traces.rend(); ++iter) {
                json::value * stackFrame = nix_libutil_json_object_new();
                nix_libutil_json_object_set_string(stackFrame, "raw_msg", iter->hint.str().c_str());
                to_json(stackFrame, iter->pos);
                nix_libutil_json_list_insert(traces, stackFrame);
            }

            nix_libutil_json_object_set(json, "trace", traces);
        }

        write(json);
    }

    void startActivity(ActivityId act, Verbosity lvl, ActivityType type,
        const std::string & s, const Fields & fields, ActivityId parent) override
    {
        json::value * json = nix_libutil_json_object_new();
        nix_libutil_json_object_set_string(json, "action", "start");
        nix_libutil_json_object_set_integer(json, "id", act);
        nix_libutil_json_object_set_integer(json, "level", lvl);
        nix_libutil_json_object_set_integer(json, "type", type);
        nix_libutil_json_object_set_string(json, "text", s.c_str());
        nix_libutil_json_object_set_integer(json, "parent", parent);
        addFields(json, fields);
        write(json);
    }

    void stopActivity(ActivityId act) override
    {
        nlohmann::json json;
        json["action"] = "stop";
        json["id"] = act;
        write(json);
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        nlohmann::json json;
        json["action"] = "result";
        json["id"] = act;
        json["type"] = type;
        addFields(json, fields);
        write(json);
    }
};

Logger * makeJSONLogger(Logger & prevLogger)
{
    return new JSONLogger(prevLogger);
}

static Logger::Fields getFields(nlohmann::json & json)
{
    Logger::Fields fields;
    for (auto & f : json) {
        if (f.type() == nlohmann::json::value_t::number_unsigned)
            fields.emplace_back(Logger::Field(f.get<uint64_t>()));
        else if (f.type() == nlohmann::json::value_t::string)
            fields.emplace_back(Logger::Field(f.get<std::string>()));
        else throw Error("unsupported JSON type %d", (int) f.type());
    }
    return fields;
}

std::optional<nlohmann::json> parseJSONMessage(const std::string & msg, std::string_view source)
{
    if (!hasPrefix(msg, "@nix ")) return std::nullopt;
    try {
        return nlohmann::json::parse(std::string(msg, 5));
    } catch (std::exception & e) {
        printError("bad JSON log message from %s: %s",
            Uncolored(source),
            e.what());
    }
    return std::nullopt;
}

bool handleJSONLogMessage(nlohmann::json & json,
    const Activity & act, std::map<ActivityId, Activity> & activities,
    std::string_view source, bool trusted)
{
    try {
        std::string action = json["action"];

        if (action == "start") {
            auto type = (ActivityType) json["type"];
            if (trusted || type == actFileTransfer)
                activities.emplace(std::piecewise_construct,
                    std::forward_as_tuple(json["id"]),
                    std::forward_as_tuple(*logger, (Verbosity) json["level"], type,
                        json["text"], getFields(json["fields"]), act.id));
        }

        else if (action == "stop")
            activities.erase((ActivityId) json["id"]);

        else if (action == "result") {
            auto i = activities.find((ActivityId) json["id"]);
            if (i != activities.end())
                i->second.result((ResultType) json["type"], getFields(json["fields"]));
        }

        else if (action == "setPhase") {
            std::string phase = json["phase"];
            act.result(resSetPhase, phase);
        }

        else if (action == "msg") {
            std::string msg = json["msg"];
            logger->log((Verbosity) json["level"], msg);
        }

        return true;
    } catch (const nlohmann::json::exception &e) {
        warn(
            "Unable to handle a JSON message from %s: %s",
            Uncolored(source),
            e.what()
        );
        return false;
    }
}

bool handleJSONLogMessage(const std::string & msg,
    const Activity & act, std::map<ActivityId, Activity> & activities, std::string_view source, bool trusted)
{
    auto json = parseJSONMessage(msg, source);
    if (!json) return false;

    return handleJSONLogMessage(*json, act, activities, source, trusted);
}

Activity::~Activity()
{
    try {
        logger.stopActivity(id);
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

}
