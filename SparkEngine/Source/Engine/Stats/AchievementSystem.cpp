/**
 * @file AchievementSystem.cpp
 * @brief Implementation of the achievement and statistics system
 */

#include "AchievementSystem.h"

#include <fstream>
#include <sstream>
#include <algorithm>

namespace Spark
{

    AchievementSystem& AchievementSystem::Get()
    {
        static AchievementSystem instance;
        return instance;
    }

    void AchievementSystem::DefineAchievement(const std::string& id, const std::string& name,
                                              const std::string& description, bool hidden)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Achievement achievement;
        achievement.id = id;
        achievement.name = name;
        achievement.description = description;
        achievement.hidden = hidden;
        m_achievements[id] = std::move(achievement);
    }

    void AchievementSystem::SetCondition(const std::string& achievementId, const std::string& statName,
                                         int64_t requiredValue)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_achievements.find(achievementId);
        if (it != m_achievements.end())
        {
            it->second.conditionStat = statName;
            it->second.requiredValue = requiredValue;
        }
    }

    bool AchievementSystem::UnlockAchievement(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_achievements.find(id);
        if (it == m_achievements.end() || it->second.unlocked)
        {
            return false;
        }
        it->second.unlocked = true;
        it->second.progress = 1.0f;

        for (const auto& callback : m_unlockCallbacks)
        {
            callback(id);
        }
        return true;
    }

    const Achievement* AchievementSystem::GetAchievement(const std::string& id) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_achievements.find(id);
        return it != m_achievements.end() ? &it->second : nullptr;
    }

    std::vector<Achievement> AchievementSystem::GetAllAchievements() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Achievement> result;
        result.reserve(m_achievements.size());
        for (const auto& [id, achievement] : m_achievements)
        {
            result.push_back(achievement);
        }
        return result;
    }

    size_t AchievementSystem::GetUnlockedCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t count = 0;
        for (const auto& [id, achievement] : m_achievements)
        {
            if (achievement.unlocked)
            {
                ++count;
            }
        }
        return count;
    }

    size_t AchievementSystem::GetTotalCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_achievements.size();
    }

    void AchievementSystem::DefineStatistic(const std::string& name, StatType type)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Statistic stat;
        stat.name = name;
        stat.type = type;
        m_statistics[name] = std::move(stat);
    }

    void AchievementSystem::IncrementStat(const std::string& name, int64_t delta)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_statistics.find(name);
            if (it == m_statistics.end())
            {
                return;
            }
            it->second.intValue += delta;
        }
        CheckConditions(name);
    }

    void AchievementSystem::AddStatFloat(const std::string& name, double delta)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_statistics.find(name);
            if (it == m_statistics.end())
            {
                return;
            }
            it->second.floatValue += delta;
        }
        CheckConditions(name);
    }

    void AchievementSystem::SetStat(const std::string& name, int64_t value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_statistics.find(name);
            if (it == m_statistics.end())
            {
                return;
            }
            if (it->second.type == StatType::Maximum)
            {
                it->second.intValue = (std::max)(it->second.intValue, value);
            }
            else if (it->second.type == StatType::Minimum)
            {
                if (it->second.intValue == 0)
                {
                    it->second.intValue = value;
                }
                else
                {
                    it->second.intValue = (std::min)(it->second.intValue, value);
                }
            }
            else
            {
                it->second.intValue = value;
            }
        }
        CheckConditions(name);
    }

    int64_t AchievementSystem::GetStatInt(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_statistics.find(name);
        return it != m_statistics.end() ? it->second.intValue : 0;
    }

    double AchievementSystem::GetStatFloat(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_statistics.find(name);
        return it != m_statistics.end() ? it->second.floatValue : 0.0;
    }

    std::vector<Statistic> AchievementSystem::GetAllStatistics() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Statistic> result;
        result.reserve(m_statistics.size());
        for (const auto& [name, stat] : m_statistics)
        {
            result.push_back(stat);
        }
        return result;
    }

    void AchievementSystem::CheckConditions(const std::string& statName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto statIt = m_statistics.find(statName);
        if (statIt == m_statistics.end())
        {
            return;
        }

        for (auto& [id, achievement] : m_achievements)
        {
            if (achievement.unlocked || achievement.conditionStat != statName)
            {
                continue;
            }

            int64_t current = statIt->second.intValue;
            int64_t required = achievement.requiredValue;

            if (required > 0)
            {
                achievement.progress = static_cast<float>((std::min)(current, required)) / static_cast<float>(required);
            }

            if (current >= required)
            {
                achievement.unlocked = true;
                achievement.progress = 1.0f;
                for (const auto& callback : m_unlockCallbacks)
                {
                    callback(id);
                }
            }
        }
    }

    bool AchievementSystem::SaveToFile(const std::string& filePath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        file << "{\n  \"statistics\": {\n";
        bool first = true;
        for (const auto& [name, stat] : m_statistics)
        {
            if (!first)
            {
                file << ",\n";
            }
            first = false;
            file << "    \"" << name << "\": {" << "\"int\": " << stat.intValue << ", "
                 << "\"float\": " << stat.floatValue << "}";
        }
        file << "\n  },\n  \"unlocked\": [";

        first = true;
        for (const auto& [id, achievement] : m_achievements)
        {
            if (achievement.unlocked)
            {
                if (!first)
                {
                    file << ", ";
                }
                first = false;
                file << "\"" << id << "\"";
            }
        }
        file << "]\n}\n";
        return true;
    }

    bool AchievementSystem::LoadFromFile(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Parse statistics
        std::lock_guard<std::mutex> lock(m_mutex);
        // Simple parsing — look for stat entries
        for (auto& [name, stat] : m_statistics)
        {
            // Find the stat entry in JSON
            std::string pattern = "\"" + name + "\": {\"int\": ";
            size_t pos = content.find(pattern);
            if (pos != std::string::npos)
            {
                pos += pattern.length();
                size_t end = content.find(',', pos);
                if (end != std::string::npos)
                {
                    stat.intValue = std::stoll(content.substr(pos, end - pos));
                }
                std::string floatPattern = "\"float\": ";
                size_t fPos = content.find(floatPattern, pos);
                if (fPos != std::string::npos)
                {
                    fPos += floatPattern.length();
                    size_t fEnd = content.find('}', fPos);
                    if (fEnd != std::string::npos)
                    {
                        stat.floatValue = std::stod(content.substr(fPos, fEnd - fPos));
                    }
                }
            }
        }

        // Parse unlocked achievements
        size_t unlockedPos = content.find("\"unlocked\":");
        if (unlockedPos != std::string::npos)
        {
            size_t arrStart = content.find('[', unlockedPos);
            size_t arrEnd = content.find(']', arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos)
            {
                std::string arr = content.substr(arrStart, arrEnd - arrStart + 1);
                for (auto& [id, achievement] : m_achievements)
                {
                    if (arr.find("\"" + id + "\"") != std::string::npos)
                    {
                        achievement.unlocked = true;
                        achievement.progress = 1.0f;
                    }
                }
            }
        }

        return true;
    }

    void AchievementSystem::ResetAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, achievement] : m_achievements)
        {
            achievement.unlocked = false;
            achievement.progress = 0.0f;
        }
        for (auto& [name, stat] : m_statistics)
        {
            stat.intValue = 0;
            stat.floatValue = 0.0;
        }
    }

    void AchievementSystem::OnAchievementUnlocked(std::function<void(const std::string&)> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_unlockCallbacks.push_back(std::move(callback));
    }

    std::string AchievementSystem::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "=== Achievement System ===\n";
        oss << "Achievements: " << GetUnlockedCount() << "/" << m_achievements.size() << " unlocked\n";
        for (const auto& [id, achievement] : m_achievements)
        {
            oss << "  [" << (achievement.unlocked ? "X" : " ") << "] " << achievement.name;
            if (!achievement.unlocked && !achievement.conditionStat.empty())
            {
                oss << " (" << static_cast<int>(achievement.progress * 100.0f) << "%)";
            }
            oss << "\n";
        }
        return oss.str();
    }

    std::string AchievementSystem::Console_ListStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "=== Player Statistics ===\n";
        for (const auto& [name, stat] : m_statistics)
        {
            oss << "  " << name << ": ";
            if (stat.type == StatType::Float)
            {
                oss << stat.floatValue;
            }
            else
            {
                oss << stat.intValue;
            }
            oss << "\n";
        }
        return oss.str();
    }

} // namespace Spark
