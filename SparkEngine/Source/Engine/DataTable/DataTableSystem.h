/**
 * @file DataTableSystem.h
 * @brief Data-driven table system for loading and querying structured game data.
 * @author Spark Engine Team
 * @date 2026
 *
 * @details CSV/JSON parsing, typed column access, row queries, hot-reload, and a
 * singleton registry. CSV parsing is RFC 4180 compliant.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Data
{

    /// @brief Supported column value types.
    enum class ColumnType : uint8_t
    {
        String,
        Int,
        Float,
        Bool,
        Vector3
    };

    /// @brief Describes a named column with type info and a default value.
    struct DataColumn
    {
        std::string name;
        ColumnType type = ColumnType::String;
        std::string defaultValue;
    };

    /// @brief A single row of values keyed by column name. Stored as strings, converted on access.
    class DataRow
    {
      public:
        /// @brief Get string value, or empty if absent.
        std::string GetString(const std::string& c) const
        {
            auto it = m_values.find(c);
            return (it != m_values.end()) ? it->second : std::string{};
        }
        /// @brief Get int value, or 0 if absent/unparseable.
        int GetInt(const std::string& c) const
        {
            auto it = m_values.find(c);
            if (it == m_values.end() || it->second.empty())
                return 0;
            try
            {
                return std::stoi(it->second);
            }
            catch (...)
            {
                return 0;
            }
        }
        /// @brief Get float value, or 0.0f if absent/unparseable.
        float GetFloat(const std::string& c) const
        {
            auto it = m_values.find(c);
            if (it == m_values.end() || it->second.empty())
                return 0.0f;
            try
            {
                return std::stof(it->second);
            }
            catch (...)
            {
                return 0.0f;
            }
        }
        /// @brief Get bool value ("true"/"1" = true, case-insensitive).
        bool GetBool(const std::string& c) const
        {
            auto it = m_values.find(c);
            if (it == m_values.end())
                return false;
            auto lv = LCase(it->second);
            return lv == "true" || lv == "1";
        }
        /// @brief Set a value for a column.
        void SetValue(const std::string& col, const std::string& val) { m_values[col] = val; }
        /// @brief Check whether this row has a value for the given column.
        bool HasColumn(const std::string& col) const { return m_values.contains(col); }
        /// @brief Direct access to the underlying value map.
        const std::unordered_map<std::string, std::string>& GetValues() const { return m_values; }

      private:
        friend class DataTable;
        static std::string LCase(const std::string& s)
        {
            std::string r = s;
            std::transform(r.begin(), r.end(), r.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return r;
        }
        std::unordered_map<std::string, std::string> m_values;
    };

    /// @brief A structured, queryable table of rows and typed columns.
    class DataTable
    {
      public:
        /// @brief Construct an empty table with an optional ID column name.
        explicit DataTable(const std::string& idColumn = "") : m_idColumn(idColumn) {}

        /// @brief Add a column definition.
        void AddColumn(const DataColumn& col) { m_columns.push_back(col); }
        /// @brief Return column count.
        size_t GetColumnCount() const { return m_columns.size(); }
        /// @brief Return column definitions.
        const std::vector<DataColumn>& GetColumns() const { return m_columns; }

        /// @brief Add a row (fills missing columns with defaults, indexes by ID).
        DataRow* AddRow(DataRow row)
        {
            for (const auto& c : m_columns)
                if (!row.HasColumn(c.name))
                    row.SetValue(c.name, c.defaultValue);
            m_rows.push_back(std::move(row));
            auto& s = m_rows.back();
            if (!m_idColumn.empty() && s.HasColumn(m_idColumn))
                m_index[s.GetString(m_idColumn)] = m_rows.size() - 1;
            return &s;
        }

        /// @brief Remove a row by its ID.
        bool RemoveRow(const std::string& rowId)
        {
            auto it = m_index.find(rowId);
            if (it == m_index.end())
                return false;
            m_rows.erase(m_rows.begin() + static_cast<ptrdiff_t>(it->second));
            RebuildIndex();
            return true;
        }

        /// @brief Look up a row by its ID column value.
        DataRow* GetRow(const std::string& rowId)
        {
            auto it = m_index.find(rowId);
            return (it != m_index.end()) ? &m_rows[it->second] : nullptr;
        }
        /// @brief Const overload.
        const DataRow* GetRow(const std::string& rowId) const
        {
            auto it = m_index.find(rowId);
            return (it != m_index.end()) ? &m_rows[it->second] : nullptr;
        }

        /// @brief Find all rows where a column matches a value (exact string match).
        std::vector<const DataRow*> FindRows(const std::string& colName, const std::string& value) const
        {
            std::vector<const DataRow*> r;
            for (const auto& row : m_rows)
                if (row.GetString(colName) == value)
                    r.push_back(&row);
            return r;
        }

        /// @brief Return total row count.
        size_t GetRowCount() const { return m_rows.size(); }
        /// @brief Direct access to all rows.
        const std::vector<DataRow>& GetRows() const { return m_rows; }

        /// @brief Parse CSV content (header + data). Returns true if rows were parsed.
        bool LoadFromCSV(const std::string& csv)
        {
            m_columns.clear();
            m_rows.clear();
            m_index.clear();
            auto lines = SplitCSVLines(csv);
            if (lines.empty())
                return false;
            auto hdrs = SplitCSVFields(lines[0]);
            if (hdrs.empty())
                return false;
            if (m_idColumn.empty())
                m_idColumn = hdrs[0];
            std::vector<std::vector<std::string>> raw;
            for (size_t i = 1; i < lines.size(); ++i)
            {
                auto f = SplitCSVFields(lines[i]);
                if (!f.empty())
                    raw.push_back(std::move(f));
            }
            for (size_t c = 0; c < hdrs.size(); ++c)
                m_columns.push_back({hdrs[c], GuessType(raw, c), ""});
            for (const auto& fields : raw)
            {
                DataRow row;
                for (size_t c = 0; c < hdrs.size() && c < fields.size(); ++c)
                    row.SetValue(hdrs[c], fields[c]);
                AddRow(std::move(row));
            }
            return !m_rows.empty();
        }

        /// @brief Export as CSV string.
        std::string SaveToCSV() const
        {
            std::ostringstream o;
            for (size_t i = 0; i < m_columns.size(); ++i)
            {
                if (i)
                    o << ',';
                o << QuoteCSV(m_columns[i].name);
            }
            o << '\n';
            for (const auto& row : m_rows)
            {
                for (size_t i = 0; i < m_columns.size(); ++i)
                {
                    if (i)
                        o << ',';
                    o << QuoteCSV(row.GetString(m_columns[i].name));
                }
                o << '\n';
            }
            return o.str();
        }

        /// @brief Parse a JSON array of objects. Returns true if rows were parsed.
        bool LoadFromJSON(const std::string& json)
        {
            m_columns.clear();
            m_rows.clear();
            m_index.clear();
            size_t p = WS(json, 0);
            if (p >= json.size() || json[p] != '[')
                return false;
            ++p;
            std::vector<std::unordered_map<std::string, std::string>> objs;
            std::vector<std::string> colOrd;
            while (p < json.size())
            {
                p = WS(json, p);
                if (p >= json.size() || json[p] == ']')
                    break;
                if (json[p] == ',')
                {
                    ++p;
                    continue;
                }
                if (json[p] != '{')
                    return false;
                ++p;
                std::unordered_map<std::string, std::string> obj;
                while (p < json.size())
                {
                    p = WS(json, p);
                    if (p >= json.size())
                        return false;
                    if (json[p] == '}')
                    {
                        ++p;
                        break;
                    }
                    if (json[p] == ',')
                    {
                        ++p;
                        continue;
                    }
                    auto [k, ke] = JStr(json, p);
                    p = WS(json, ke);
                    if (p >= json.size() || json[p] != ':')
                        return false;
                    p = WS(json, p + 1);
                    auto [v, ve] = JVal(json, p);
                    p = ve;
                    obj[k] = v;
                    if (std::find(colOrd.begin(), colOrd.end(), k) == colOrd.end())
                        colOrd.push_back(k);
                }
                objs.push_back(std::move(obj));
            }
            if (objs.empty())
                return false;
            if (m_idColumn.empty() && !colOrd.empty())
                m_idColumn = colOrd[0];
            for (const auto& cn : colOrd)
                m_columns.push_back({cn, GuessJType(objs, cn), ""});
            for (const auto& obj : objs)
            {
                DataRow row;
                for (const auto& [k, v] : obj)
                    row.SetValue(k, v);
                AddRow(std::move(row));
            }
            return true;
        }

        /// @brief Export as JSON array of objects.
        std::string SaveToJSON() const
        {
            std::ostringstream o;
            o << "[\n";
            for (size_t r = 0; r < m_rows.size(); ++r)
            {
                o << "  {";
                for (size_t c = 0; c < m_columns.size(); ++c)
                {
                    if (c)
                        o << ',';
                    o << ' ';
                    const auto& col = m_columns[c];
                    const std::string v = m_rows[r].GetString(col.name);
                    o << '"' << JEsc(col.name) << "\": ";
                    if (col.type == ColumnType::Int || col.type == ColumnType::Float)
                        o << (v.empty() ? "0" : v);
                    else if (col.type == ColumnType::Bool)
                    {
                        auto lv = LCase(v);
                        o << ((lv == "true" || lv == "1") ? "true" : "false");
                    }
                    else
                        o << '"' << JEsc(v) << '"';
                }
                o << " }" << ((r + 1 < m_rows.size()) ? ",\n" : "\n");
            }
            o << "]\n";
            return o.str();
        }

        /// @brief Validate rows against schema. Returns error messages (empty = valid).
        std::vector<std::string> Validate() const
        {
            std::vector<std::string> errs;
            for (size_t r = 0; r < m_rows.size(); ++r)
                for (const auto& col : m_columns)
                {
                    auto v = m_rows[r].GetString(col.name);
                    if (v.empty())
                    {
                        errs.push_back("Row " + std::to_string(r) + ": missing '" + col.name + "'");
                        continue;
                    }
                    if (col.type == ColumnType::Int)
                    {
                        try
                        {
                            std::stoi(v);
                        }
                        catch (...)
                        {
                            errs.push_back("Row " + std::to_string(r) + ": '" + col.name + "' not int");
                        }
                    }
                    if (col.type == ColumnType::Float)
                    {
                        try
                        {
                            std::stof(v);
                        }
                        catch (...)
                        {
                            errs.push_back("Row " + std::to_string(r) + ": '" + col.name + "' not float");
                        }
                    }
                }
            return errs;
        }

        const std::string& GetIdColumn() const { return m_idColumn; }
        const std::string& GetSourcePath() const { return m_sourcePath; }
        void SetSourcePath(const std::string& p) { m_sourcePath = p; }

      private:
        static std::string LCase(const std::string& s) { return DataRow::LCase(s); }
        static std::vector<std::string> SplitCSVLines(const std::string& t)
        {
            std::vector<std::string> ls;
            std::string cur;
            bool q = false;
            for (char c : t)
            {
                if (c == '"')
                {
                    q = !q;
                    cur += c;
                }
                else if (c == '\n' && !q)
                {
                    if (!cur.empty() && cur.back() == '\r')
                        cur.pop_back();
                    if (!cur.empty())
                        ls.push_back(std::move(cur));
                    cur.clear();
                }
                else
                    cur += c;
            }
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            if (!cur.empty())
                ls.push_back(std::move(cur));
            return ls;
        }

        static std::vector<std::string> SplitCSVFields(const std::string& line)
        {
            std::vector<std::string> fs;
            std::string f;
            bool q = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                char c = line[i];
                if (q)
                {
                    if (c == '"')
                    {
                        if (i + 1 < line.size() && line[i + 1] == '"')
                        {
                            f += '"';
                            ++i;
                        }
                        else
                            q = false;
                    }
                    else
                        f += c;
                }
                else if (c == '"')
                    q = true;
                else if (c == ',')
                {
                    fs.push_back(std::move(f));
                    f.clear();
                }
                else
                    f += c;
            }
            fs.push_back(std::move(f));
            return fs;
        }

        static std::string QuoteCSV(const std::string& f)
        {
            bool need = false;
            for (char c : f)
                if (c == ',' || c == '"' || c == '\n')
                {
                    need = true;
                    break;
                }
            if (!need)
                return f;
            std::string o = "\"";
            for (char c : f)
            {
                if (c == '"')
                    o += "\"\"";
                else
                    o += c;
            }
            return o + '"';
        }

        static ColumnType GuessType(const std::vector<std::vector<std::string>>& rows, size_t ci)
        {
            bool aI = true, aF = true, aB = true;
            int n = 0;
            for (const auto& r : rows)
            {
                if (ci >= r.size() || r[ci].empty())
                    continue;
                const auto& v = r[ci];
                ++n;
                if (aB)
                {
                    auto l = LCase(v);
                    if (l != "true" && l != "false" && l != "0" && l != "1")
                        aB = false;
                }
                if (aI)
                {
                    try
                    {
                        size_t p = 0;
                        std::stoi(v, &p);
                        if (p != v.size())
                            aI = false;
                    }
                    catch (...)
                    {
                        aI = false;
                    }
                }
                if (aF && !aI)
                {
                    try
                    {
                        size_t p = 0;
                        std::stof(v, &p);
                        if (p != v.size())
                            aF = false;
                    }
                    catch (...)
                    {
                        aF = false;
                    }
                }
            }
            if (!n)
                return ColumnType::String;
            return aB ? ColumnType::Bool : aI ? ColumnType::Int : aF ? ColumnType::Float : ColumnType::String;
        }

        static size_t WS(const std::string& s, size_t p)
        {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
                ++p;
            return p;
        }

        static std::pair<std::string, size_t> JStr(const std::string& s, size_t p)
        {
            if (p >= s.size() || s[p] != '"')
                return {"", p};
            ++p;
            std::string r;
            while (p < s.size() && s[p] != '"')
            {
                if (s[p] == '\\' && p + 1 < s.size())
                {
                    ++p;
                    if (s[p] == 'n')
                        r += '\n';
                    else if (s[p] == 't')
                        r += '\t';
                    else if (s[p] == 'r')
                        r += '\r';
                    else
                        r += s[p];
                }
                else
                    r += s[p];
                ++p;
            }
            if (p < s.size())
                ++p;
            return {r, p};
        }

        static std::pair<std::string, size_t> JVal(const std::string& s, size_t p)
        {
            if (p >= s.size())
                return {"", p};
            if (s[p] == '"')
                return JStr(s, p);
            if (s.compare(p, 4, "true") == 0)
                return {"true", p + 4};
            if (s.compare(p, 5, "false") == 0)
                return {"false", p + 5};
            if (s.compare(p, 4, "null") == 0)
                return {"", p + 4};
            size_t st = p;
            if (s[p] == '-')
                ++p;
            while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.' || s[p] == 'e' ||
                                    s[p] == 'E' || s[p] == '+' || s[p] == '-'))
            {
                if ((s[p] == '+' || s[p] == '-') && p > st && s[p - 1] != 'e' && s[p - 1] != 'E')
                    break;
                ++p;
            }
            return {s.substr(st, p - st), p};
        }

        static std::string JEsc(const std::string& s)
        {
            std::string o;
            o.reserve(s.size());
            for (char c : s)
            {
                if (c == '"')
                    o += "\\\"";
                else if (c == '\\')
                    o += "\\\\";
                else if (c == '\n')
                    o += "\\n";
                else if (c == '\t')
                    o += "\\t";
                else
                    o += c;
            }
            return o;
        }

        static ColumnType GuessJType(const std::vector<std::unordered_map<std::string, std::string>>& objs,
                                     const std::string& key)
        {
            bool aI = true, aF = true, aB = true;
            int n = 0;
            for (const auto& o : objs)
            {
                auto it = o.find(key);
                if (it == o.end() || it->second.empty())
                    continue;
                const auto& v = it->second;
                ++n;
                if (aB && v != "true" && v != "false")
                    aB = false;
                if (aI)
                {
                    try
                    {
                        size_t p = 0;
                        std::stoi(v, &p);
                        if (p != v.size())
                            aI = false;
                    }
                    catch (...)
                    {
                        aI = false;
                    }
                }
                if (aF && !aI)
                {
                    try
                    {
                        size_t p = 0;
                        std::stof(v, &p);
                        if (p != v.size())
                            aF = false;
                    }
                    catch (...)
                    {
                        aF = false;
                    }
                }
            }
            if (!n)
                return ColumnType::String;
            return aB ? ColumnType::Bool : aI ? ColumnType::Int : aF ? ColumnType::Float : ColumnType::String;
        }

        void RebuildIndex()
        {
            m_index.clear();
            if (m_idColumn.empty())
                return;
            for (size_t i = 0; i < m_rows.size(); ++i)
                if (m_rows[i].HasColumn(m_idColumn))
                    m_index[m_rows[i].GetString(m_idColumn)] = i;
        }

        std::string m_idColumn;
        std::string m_sourcePath;
        std::vector<DataColumn> m_columns;
        std::vector<DataRow> m_rows;
        std::unordered_map<std::string, size_t> m_index;
    };

    /// @brief Singleton manager for named DataTable instances with file loading and hot-reload.
    class DataTableRegistry
    {
      public:
        /// @brief Access the singleton instance (Meyer's singleton).
        static DataTableRegistry& GetInstance()
        {
            static DataTableRegistry inst;
            return inst;
        }

        DataTableRegistry(const DataTableRegistry&) = delete;
        DataTableRegistry& operator=(const DataTableRegistry&) = delete;

        /// @brief Initialize the registry. Call once at engine startup.
        bool Initialize()
        {
            m_tables.clear();
            m_initialized = true;
            return true;
        }
        /// @brief Shut down and release all tables.
        void Shutdown()
        {
            m_tables.clear();
            m_initialized = false;
        }

        /// @brief Register a pre-built table under a name.
        void RegisterTable(const std::string& name, DataTable table) { m_tables[name] = std::move(table); }

        /// @brief Look up a table by name, or nullptr if not found.
        DataTable* GetTable(const std::string& name)
        {
            auto it = m_tables.find(name);
            return (it != m_tables.end()) ? &it->second : nullptr;
        }
        /// @brief Const overload.
        const DataTable* GetTable(const std::string& name) const
        {
            auto it = m_tables.find(name);
            return (it != m_tables.end()) ? &it->second : nullptr;
        }

        /// @brief Remove a table from the registry.
        bool UnloadTable(const std::string& name) { return m_tables.erase(name) > 0; }

        /// @brief Load a table from file, auto-detecting CSV vs JSON by extension.
        bool LoadTableFromFile(const std::string& name, const std::string& filePath)
        {
            std::ifstream file(filePath);
            if (!file.is_open())
                return false;
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            DataTable table;
            table.SetSourcePath(filePath);
            bool ok = EndsWith(filePath, ".json") ? table.LoadFromJSON(content) : table.LoadFromCSV(content);
            if (ok)
                m_tables[name] = std::move(table);
            return ok;
        }

        /// @brief Reload a table from its original source file (hot-reload).
        bool ReloadTable(const std::string& name)
        {
            auto it = m_tables.find(name);
            if (it == m_tables.end())
                return false;
            auto path = it->second.GetSourcePath();
            return path.empty() ? false : LoadTableFromFile(name, path);
        }

        /// @brief Return the names of all registered tables.
        std::vector<std::string> GetTableNames() const
        {
            std::vector<std::string> ns;
            ns.reserve(m_tables.size());
            for (const auto& [k, v] : m_tables)
                ns.push_back(k);
            return ns;
        }

        /// @brief Return the number of registered tables.
        size_t GetTableCount() const { return m_tables.size(); }

        /// @brief Return a status string for the debug console.
        std::string Console_GetStatus() const
        {
            std::ostringstream o;
            o << "DataTableRegistry: " << m_tables.size() << " table(s)\n";
            for (const auto& [name, tbl] : m_tables)
            {
                o << "  " << name << ": " << tbl.GetRowCount() << " rows, " << tbl.GetColumnCount() << " cols";
                if (!tbl.GetSourcePath().empty())
                    o << " [" << tbl.GetSourcePath() << "]";
                o << '\n';
            }
            return o.str();
        }

      private:
        DataTableRegistry() = default;
        ~DataTableRegistry() = default;

        static bool EndsWith(const std::string& s, const std::string& sfx)
        {
            if (sfx.size() > s.size())
                return false;
            std::string end = s.substr(s.size() - sfx.size());
            std::transform(end.begin(), end.end(), end.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return end == sfx;
        }

        bool m_initialized = false;
        std::unordered_map<std::string, DataTable> m_tables;
    };

} // namespace Spark::Data
