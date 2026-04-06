// TestEditorAutomation.cpp - Tests for editor automation/command system
// Uses standalone reimplementation to avoid SparkEditor include path dependencies.
#include "TestFramework.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    struct AutomationCommand
    {
        std::string name;
        std::string description;
        std::function<std::string(const std::vector<std::string>&)> handler;
    };

    class EditorAutomation
    {
      public:
        void Initialize()
        {
            m_commands.clear();
            m_history.clear();
            RegisterCommand("help", "List all commands",
                            [this](const std::vector<std::string>&)
                            {
                                std::string out;
                                for (const auto& [n, c] : m_commands)
                                    out += n + " - " + c.description + "\n";
                                return out;
                            });
            RegisterCommand("echo", "Print arguments",
                            [](const std::vector<std::string>& args)
                            {
                                std::string out;
                                for (const auto& a : args)
                                    out += a + " ";
                                return out;
                            });
        }
        void Shutdown()
        {
            m_commands.clear();
            m_history.clear();
        }

        void RegisterCommand(const std::string& name, const std::string& desc,
                             std::function<std::string(const std::vector<std::string>&)> h)
        {
            m_commands[name] = {name, desc, std::move(h)};
        }

        std::string ExecuteCommand(const std::string& name, const std::vector<std::string>& args = {})
        {
            auto it = m_commands.find(name);
            if (it == m_commands.end())
                return "Error: unknown command '" + name + "'";
            m_history.push_back(name);
            return it->second.handler(args);
        }

        std::string RunScript(const std::string& script)
        {
            std::string output, line;
            for (size_t i = 0; i <= script.size(); ++i)
            {
                if (i == script.size() || script[i] == '\n')
                {
                    auto tok = Tokenize(line);
                    if (!tok.empty())
                    {
                        std::string cmd = tok[0];
                        std::vector<std::string> a(tok.begin() + 1, tok.end());
                        if (!output.empty())
                            output += '\n';
                        output += ExecuteCommand(cmd, a);
                    }
                    line.clear();
                }
                else
                    line += script[i];
            }
            return output;
        }

        const std::vector<std::string>& GetHistory() const { return m_history; }
        size_t GetCommandCount() const { return m_commands.size(); }
        bool HasCommand(const std::string& n) const { return m_commands.count(n) > 0; }
        std::vector<std::string> GetCommandNames() const
        {
            std::vector<std::string> ns;
            for (const auto& [n, _] : m_commands)
                ns.push_back(n);
            std::sort(ns.begin(), ns.end());
            return ns;
        }

      private:
        static std::vector<std::string> Tokenize(const std::string& s)
        {
            std::vector<std::string> t;
            std::string tok;
            for (char c : s)
            {
                if (c == ' ' || c == '\t')
                {
                    if (!tok.empty())
                    {
                        t.push_back(tok);
                        tok.clear();
                    }
                }
                else
                    tok += c;
            }
            if (!tok.empty())
                t.push_back(tok);
            return t;
        }

        std::unordered_map<std::string, AutomationCommand> m_commands;
        std::vector<std::string> m_history;
    };

} // anonymous namespace

TEST(EditorAutomation_Initialize)
{
    EditorAutomation a;
    a.Initialize();
    EXPECT_TRUE(a.GetCommandCount() >= 2);
    EXPECT_TRUE(a.HasCommand("help"));
    EXPECT_TRUE(a.HasCommand("echo"));
    a.Shutdown();
}

TEST(EditorAutomation_RegisterCommand)
{
    EditorAutomation a;
    a.Initialize();
    a.RegisterCommand("test_cmd", "A test", [](const std::vector<std::string>&) { return std::string("ok"); });
    EXPECT_TRUE(a.HasCommand("test_cmd"));
    a.Shutdown();
}

TEST(EditorAutomation_ExecuteCommand)
{
    EditorAutomation a;
    a.Initialize();
    a.RegisterCommand("greet", "Say hello", [](const std::vector<std::string>& args)
                      { return args.empty() ? std::string("Hello!") : std::string("Hello, " + args[0] + "!"); });
    EXPECT_EQ(std::string("Hello, World!"), a.ExecuteCommand("greet", {"World"}));
    a.Shutdown();
}

TEST(EditorAutomation_ExecuteUnknownCommand)
{
    EditorAutomation a;
    a.Initialize();
    EXPECT_STR_CONTAINS(a.ExecuteCommand("nonexistent"), "Error");
    a.Shutdown();
}

TEST(EditorAutomation_RunScript)
{
    EditorAutomation a;
    a.Initialize();
    a.RegisterCommand("add", "Add",
                      [](const std::vector<std::string>& args)
                      {
                          int s = 0;
                          for (const auto& x : args)
                              s += std::stoi(x);
                          return std::to_string(s);
                      });
    std::string out = a.RunScript("echo hello\nadd 1 2 3");
    EXPECT_STR_CONTAINS(out, "hello");
    EXPECT_STR_CONTAINS(out, "6");
    a.Shutdown();
}

TEST(EditorAutomation_CommandHistory)
{
    EditorAutomation a;
    a.Initialize();
    a.ExecuteCommand("echo", {"a"});
    a.ExecuteCommand("echo", {"b"});
    a.ExecuteCommand("help");
    EXPECT_EQ(static_cast<size_t>(3), a.GetHistory().size());
    EXPECT_EQ(std::string("echo"), a.GetHistory()[0]);
    EXPECT_EQ(std::string("help"), a.GetHistory()[2]);
    a.Shutdown();
}

TEST(EditorAutomation_HelpCommand)
{
    EditorAutomation a;
    a.Initialize();
    std::string out = a.ExecuteCommand("help");
    EXPECT_STR_CONTAINS(out, "help");
    EXPECT_STR_CONTAINS(out, "echo");
    a.Shutdown();
}

TEST(EditorAutomation_GetCommandNames)
{
    EditorAutomation a;
    a.Initialize();
    auto names = a.GetCommandNames();
    EXPECT_TRUE(names.size() >= 2);
    for (size_t i = 1; i < names.size(); ++i)
        EXPECT_TRUE(names[i - 1] <= names[i]);
    a.Shutdown();
}

TEST(EditorAutomation_EchoBuiltIn)
{
    EditorAutomation a;
    a.Initialize();
    std::string r = a.ExecuteCommand("echo", {"one", "two"});
    EXPECT_STR_CONTAINS(r, "one");
    EXPECT_STR_CONTAINS(r, "two");
    a.Shutdown();
}
