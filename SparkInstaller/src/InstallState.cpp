#include "InstallState.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace SparkInstaller
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string Escape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += c;
                }
            }
            return out;
        }

        // Tiny, tolerant key-value reader for the fields we write. Treats the JSON object
        // as a flat bag of "key": value pairs and a nested "options" object of booleans.
        // Does not validate full JSON syntax — this file is only ever produced by us.
        bool ReadFile(const std::string& path, std::string& contents)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            contents = ss.str();
            return true;
        }

        bool FindStringField(const std::string& json, const std::string& key, std::string& out)
        {
            std::string needle = "\"" + key + "\"";
            size_t k = json.find(needle);
            if (k == std::string::npos)
                return false;
            size_t colon = json.find(':', k + needle.size());
            if (colon == std::string::npos)
                return false;
            size_t q1 = json.find('"', colon);
            if (q1 == std::string::npos)
                return false;
            size_t q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos)
                return false;
            out = json.substr(q1 + 1, q2 - q1 - 1);
            return true;
        }

        bool FindIntField(const std::string& json, const std::string& key, int& out)
        {
            std::string needle = "\"" + key + "\"";
            size_t k = json.find(needle);
            if (k == std::string::npos)
                return false;
            size_t colon = json.find(':', k + needle.size());
            if (colon == std::string::npos)
                return false;
            size_t start = colon + 1;
            while (start < json.size() && (json[start] == ' ' || json[start] == '\t'))
                ++start;
            size_t end = start;
            while (end < json.size() && (isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-'))
                ++end;
            if (end == start)
                return false;
            out = std::atoi(json.substr(start, end - start).c_str());
            return true;
        }

        void ParseOptions(const std::string& json, std::map<std::string, bool>& options)
        {
            size_t k = json.find("\"options\"");
            if (k == std::string::npos)
                return;
            size_t brace = json.find('{', k);
            if (brace == std::string::npos)
                return;
            size_t endBrace = json.find('}', brace);
            if (endBrace == std::string::npos)
                return;
            std::string body = json.substr(brace + 1, endBrace - brace - 1);
            size_t pos = 0;
            while (pos < body.size())
            {
                size_t q1 = body.find('"', pos);
                if (q1 == std::string::npos)
                    break;
                size_t q2 = body.find('"', q1 + 1);
                if (q2 == std::string::npos)
                    break;
                std::string name = body.substr(q1 + 1, q2 - q1 - 1);
                size_t colon = body.find(':', q2);
                if (colon == std::string::npos)
                    break;
                size_t valStart = colon + 1;
                while (valStart < body.size() && (body[valStart] == ' ' || body[valStart] == '\t'))
                    ++valStart;
                bool value = body.compare(valStart, 4, "true") == 0;
                options[name] = value;
                size_t comma = body.find(',', valStart);
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
        }

        std::string NowUtcIso8601()
        {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        }
    } // namespace

    bool InstallState::Exists(const std::string& destination)
    {
        return fs::exists(fs::path(destination) / FileName());
    }

    bool InstallState::Load(const std::string& destination, InstallState& out)
    {
        fs::path path = fs::path(destination) / FileName();
        std::string json;
        if (!ReadFile(path.string(), json))
            return false;

        FindIntField(json, "schema", out.schema);
        FindStringField(json, "ref", out.ref);
        FindStringField(json, "commit", out.commit);
        FindStringField(json, "destination", out.destination);
        FindStringField(json, "generator", out.generator);
        FindStringField(json, "build_type", out.buildType);
        FindStringField(json, "built_at", out.builtAt);
        FindStringField(json, "installer_version", out.installerVersion);
        ParseOptions(json, out.options);
        return true;
    }

    bool InstallState::Save(const std::string& destination) const
    {
        fs::path path = fs::path(destination) / FileName();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;

        std::string timestamp = builtAt.empty() ? NowUtcIso8601() : builtAt;

        out << "{\n";
        out << "  \"schema\": " << schema << ",\n";
        out << "  \"ref\": \"" << Escape(ref) << "\",\n";
        out << "  \"commit\": \"" << Escape(commit) << "\",\n";
        out << "  \"destination\": \"" << Escape(destination) << "\",\n";
        out << "  \"generator\": \"" << Escape(generator) << "\",\n";
        out << "  \"build_type\": \"" << Escape(buildType) << "\",\n";
        out << "  \"built_at\": \"" << Escape(timestamp) << "\",\n";
        out << "  \"installer_version\": \"" << Escape(installerVersion) << "\",\n";
        out << "  \"options\": {\n";
        size_t i = 0;
        for (const auto& kv : options)
        {
            out << "    \"" << Escape(kv.first) << "\": " << (kv.second ? "true" : "false");
            if (++i < options.size())
                out << ",";
            out << "\n";
        }
        out << "  }\n";
        out << "}\n";
        return true;
    }
} // namespace SparkInstaller
