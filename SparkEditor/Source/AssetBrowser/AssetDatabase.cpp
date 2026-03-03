/**
 * @file AssetDatabase.cpp
 * @brief Implementation of the advanced asset management system
 * @author Spark Engine Team
 * @date 2025
 */

#include "AssetDatabase.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <iomanip>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace SparkEditor {

// Extension to asset type mapping
const std::unordered_map<std::string, std::string> AssetDatabase::s_extensionToType = {
    // Textures
    {".png", "Texture"}, {".jpg", "Texture"}, {".jpeg", "Texture"},
    {".bmp", "Texture"}, {".tga", "Texture"}, {".dds", "Texture"},
    {".hdr", "Texture"}, {".exr", "Texture"},
    
    // Models
    {".fbx", "Model"}, {".obj", "Model"}, {".dae", "Model"},
    {".3ds", "Model"}, {".blend", "Model"}, {".gltf", "Model"},
    {".glb", "Model"}, {".ply", "Model"},
    
    // Audio
    {".wav", "Audio"}, {".mp3", "Audio"}, {".ogg", "Audio"},
    {".flac", "Audio"}, {".aac", "Audio"}, {".m4a", "Audio"},
    
    // Shaders
    {".hlsl", "Shader"}, {".glsl", "Shader"}, {".shader", "Shader"},
    {".vert", "Shader"}, {".frag", "Shader"}, {".compute", "Shader"},
    
    // Materials
    {".mat", "Material"}, {".material", "Material"},
    
    // Scenes
    {".scene", "Scene"}, {".level", "Scene"},
    
    // Scripts
    {".cpp", "Script"}, {".h", "Script"}, {".cs", "Script"},
    {".js", "Script"}, {".lua", "Script"}, {".py", "Script"},
    
    // Data
    {".json", "Data"}, {".xml", "Data"}, {".yaml", "Data"},
    {".txt", "Text"}, {".csv", "Data"},
    
    // Fonts
    {".ttf", "Font"}, {".otf", "Font"}, {".woff", "Font"},
    
    // Animations
    {".anim", "Animation"}, {".fbx", "Animation"}
};

AssetDatabase::AssetDatabase()
    : m_lastProcessTime(std::chrono::steady_clock::now())
{
    std::cout << "AssetDatabase constructed\n";
}

AssetDatabase::~AssetDatabase()
{
    std::cout << "AssetDatabase destructor called\n";
    Shutdown();
}

bool AssetDatabase::Initialize(const std::string& assetDirectory)
{
    std::cout << "AssetDatabase::Initialize() - Directory: " << assetDirectory << "\n";
    
    m_assetDirectory = assetDirectory;
    m_metadataDirectory = assetDirectory + "/.metadata";
    
    // Create directories if they don't exist
    std::filesystem::create_directories(m_assetDirectory);
    std::filesystem::create_directories(m_metadataDirectory);
    
    // Initial scan of assets
    RefreshDatabase();
    
    // Start file system monitoring
    m_isMonitoring = true;
    m_monitoringThread = std::thread(&AssetDatabase::FileSystemMonitoringThread, this);
    
    std::cout << "AssetDatabase initialized with " << m_assets.size() << " assets\n";
    return true;
}

void AssetDatabase::Update(float deltaTime)
{
    // Process file system changes at a controlled rate
    auto currentTime = std::chrono::steady_clock::now();
    float timeSinceLastProcess = std::chrono::duration<float>(currentTime - m_lastProcessTime).count();
    
    if (timeSinceLastProcess >= m_processInterval) {
        ProcessFileSystemChanges();
        m_lastProcessTime = currentTime;
    }
}

void AssetDatabase::Shutdown()
{
    std::cout << "AssetDatabase::Shutdown()\n";
    
    // Stop monitoring
    m_isMonitoring = false;
    if (m_monitoringThread.joinable()) {
        m_monitoringThread.join();
    }
    
    // Clear assets
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    m_assets.clear();
    m_assetMap.clear();
    m_guidMap.clear();
}

const AssetInfo* AssetDatabase::GetAssetByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        return &m_assets[it->second];
    }
    return nullptr;
}

const AssetInfo* AssetDatabase::GetAssetByGUID(const std::string& guid) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_guidMap.find(guid);
    if (it != m_guidMap.end()) {
        return &m_assets[it->second];
    }
    return nullptr;
}

bool AssetDatabase::ImportAsset(const std::string& filePath)
{
    if (!IsAssetFile(filePath)) {
        return false;
    }
    
    std::cout << "Importing asset: " << filePath << "\n";
    
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    
    // Check if asset already exists
    auto existingIt = m_assetMap.find(filePath);
    if (existingIt != m_assetMap.end()) {
        // Update existing asset
        AssetInfo& asset = m_assets[existingIt->second];
        UpdateAssetModificationTime(filePath);
        asset.isDirty = true;
        return true;
    }
    
    // Create new asset
    AssetInfo asset;
    asset.path = filePath;
    asset.name = std::filesystem::path(filePath).filename().string();
    asset.type = DetermineAssetType(filePath);
    asset.guid = GenerateGUID(filePath);
    asset.isImported = true;
    asset.lastImported = std::time(nullptr);
    
    try {
        asset.fileSize = std::filesystem::file_size(filePath);
        auto ftime = std::filesystem::last_write_time(filePath);
        asset.lastModified = std::chrono::duration_cast<std::chrono::seconds>(
            ftime.time_since_epoch()).count();
    } catch (...) {
        asset.fileSize = 0;
        asset.lastModified = 0;
    }
    
    size_t index = m_assets.size();
    m_assets.push_back(asset);
    m_assetMap[filePath] = index;
    m_guidMap[asset.guid] = index;
    
    // Load metadata if available
    LoadAssetMetadata(filePath);
    
    return true;
}

bool AssetDatabase::ReimportAsset(const std::string& assetPath)
{
    std::cout << "Reimporting asset: " << assetPath << "\n";
    
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assetMap.find(assetPath);
    if (it != m_assetMap.end()) {
        AssetInfo& asset = m_assets[it->second];
        asset.isDirty = false;
        asset.lastImported = std::time(nullptr);
        UpdateAssetModificationTime(assetPath);
        return true;
    }
    
    return false;
}

bool AssetDatabase::DeleteAsset(const std::string& assetPath)
{
    std::cout << "Deleting asset: " << assetPath << "\n";
    
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assetMap.find(assetPath);
    if (it != m_assetMap.end()) {
        size_t index = it->second;
        const AssetInfo& asset = m_assets[index];
        
        // Remove from GUID map
        m_guidMap.erase(asset.guid);
        
        // Remove from asset vector (swap with last element)
        if (index < m_assets.size() - 1) {
            std::swap(m_assets[index], m_assets.back());
            // Update map for swapped element
            m_assetMap[m_assets[index].path] = index;
            m_guidMap[m_assets[index].guid] = index;
        }
        m_assets.pop_back();
        
        // Remove from path map
        m_assetMap.erase(it);
        
        return true;
    }
    
    return false;
}

std::vector<const AssetInfo*> AssetDatabase::GetAssetsByType(const std::string& type) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    std::vector<const AssetInfo*> result;
    
    for (const auto& asset : m_assets) {
        if (asset.type == type) {
            result.push_back(&asset);
        }
    }
    
    return result;
}

std::vector<const AssetInfo*> AssetDatabase::SearchAssets(const std::string& searchTerm) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    std::vector<const AssetInfo*> result;
    
    std::string lowerSearchTerm = searchTerm;
    std::transform(lowerSearchTerm.begin(), lowerSearchTerm.end(), lowerSearchTerm.begin(), ::tolower);
    
    for (const auto& asset : m_assets) {
        std::string lowerName = asset.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        
        if (lowerName.find(lowerSearchTerm) != std::string::npos) {
            result.push_back(&asset);
        }
    }
    
    return result;
}

std::vector<std::string> AssetDatabase::GetAssetDependencies(const std::string& assetPath) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assetMap.find(assetPath);
    if (it != m_assetMap.end()) {
        return m_assets[it->second].dependencies;
    }
    return {};
}

void AssetDatabase::SetFileSystemChangeCallback(std::function<void(const FileSystemChange&)> callback)
{
    m_fileSystemCallback = callback;
}

AssetImportSettings AssetDatabase::GetImportSettings(const std::string& assetPath) const
{
    auto it = m_importSettings.find(assetPath);
    if (it != m_importSettings.end()) {
        return it->second;
    }
    return AssetImportSettings{}; // Default settings
}

void AssetDatabase::SetImportSettings(const std::string& assetPath, const AssetImportSettings& settings)
{
    m_importSettings[assetPath] = settings;
    SaveAssetMetadata(assetPath);
}

void AssetDatabase::RefreshDatabase()
{
    std::cout << "Refreshing asset database...\n";
    
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    m_assets.clear();
    m_assetMap.clear();
    m_guidMap.clear();
    
    ScanDirectory(m_assetDirectory);
    
    std::cout << "Database refresh complete. Found " << m_assets.size() << " assets\n";
}

AssetDatabase::DatabaseStats AssetDatabase::GetDatabaseStats() const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    DatabaseStats stats;
    
    stats.totalAssets = m_assets.size();
    
    for (const auto& asset : m_assets) {
        stats.totalSize += asset.fileSize;
        
        if (asset.isDirty) {
            stats.dirtyAssets++;
        }
        
        if (asset.type == "Texture") {
            stats.textureAssets++;
        } else if (asset.type == "Model") {
            stats.modelAssets++;
        } else if (asset.type == "Audio") {
            stats.audioAssets++;
        } else if (asset.type == "Shader") {
            stats.shaderAssets++;
        } else if (asset.type == "Scene") {
            stats.sceneAssets++;
        }
    }
    
    return stats;
}

void AssetDatabase::ScanDirectory(const std::string& directoryPath)
{
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && IsAssetFile(entry.path().string())) {
                std::string filePath = entry.path().string();
                
                // Skip metadata files
                if (filePath.find("/.metadata/") != std::string::npos) {
                    continue;
                }
                
                AssetInfo asset;
                asset.path = filePath;
                asset.name = entry.path().filename().string();
                asset.type = DetermineAssetType(filePath);
                asset.guid = GenerateGUID(filePath);
                asset.isImported = true;
                asset.lastImported = std::time(nullptr);
                
                try {
                    asset.fileSize = entry.file_size();
                    auto ftime = entry.last_write_time();
                    asset.lastModified = std::chrono::duration_cast<std::chrono::seconds>(
                        ftime.time_since_epoch()).count();
                } catch (...) {
                    asset.fileSize = 0;
                    asset.lastModified = 0;
                }
                
                size_t index = m_assets.size();
                m_assets.push_back(asset);
                m_assetMap[filePath] = index;
                m_guidMap[asset.guid] = index;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error scanning directory " << directoryPath << ": " << e.what() << "\n";
    }
}

std::string AssetDatabase::DetermineAssetType(const std::string& filePath) const
{
    std::string extension = std::filesystem::path(filePath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    auto it = s_extensionToType.find(extension);
    if (it != s_extensionToType.end()) {
        return it->second;
    }
    
    return "Unknown";
}

std::string AssetDatabase::GenerateGUID(const std::string& filePath) const
{
    // Simple GUID generation based on file path and timestamp
    std::hash<std::string> hasher;
    size_t hash = hasher(filePath + std::to_string(std::time(nullptr)));
    
    std::stringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

bool AssetDatabase::LoadAssetMetadata(const std::string& assetPath)
{
    std::string metadataPath = m_metadataDirectory + "/" + 
                              std::filesystem::path(assetPath).filename().string() + ".meta";
    
    std::ifstream file(metadataPath);
    if (!file.is_open()) {
        return false;
    }
    
    // TODO: Implement proper metadata loading (JSON/XML)
    // For now, just return true to indicate the file exists
    return true;
}

bool AssetDatabase::SaveAssetMetadata(const std::string& assetPath)
{
    std::string metadataPath = m_metadataDirectory + "/" + 
                              std::filesystem::path(assetPath).filename().string() + ".meta";
    
    std::ofstream file(metadataPath);
    if (!file.is_open()) {
        return false;
    }
    
    // TODO: Implement proper metadata saving (JSON/XML)
    file << "# Metadata for " << assetPath << "\n";
    file << "version: 1.0\n";
    
    return true;
}

void AssetDatabase::FileSystemMonitoringThread()
{
    std::cout << "File system monitoring started\n";
    
#ifdef _WIN32
    // Windows-specific file system monitoring using ReadDirectoryChangesW
    HANDLE hDirectory = CreateFileA(
        m_assetDirectory.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    
    if (hDirectory == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open directory for monitoring\n";
        return;
    }
    
    const DWORD bufferSize = 1024 * 16;
    std::vector<BYTE> buffer(bufferSize);
    
    while (m_isMonitoring) {
        DWORD bytesReturned = 0;
        BOOL result = ReadDirectoryChangesW(
            hDirectory,
            buffer.data(),
            bufferSize,
            TRUE, // Watch subdirectories
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned,
            nullptr,
            nullptr
        );
        
        if (!result || bytesReturned == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Process the changes
        DWORD offset = 0;
        FILE_NOTIFY_INFORMATION* info = nullptr;
        
        do {
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            
            std::wstring wFileName(info->FileName, info->FileNameLength / sizeof(WCHAR));
            std::string fileName(wFileName.begin(), wFileName.end());
            std::string fullPath = m_assetDirectory + "/" + fileName;
            
            if (IsAssetFile(fullPath)) {
                FileSystemChange change;
                change.path = fullPath;
                change.timestamp = std::chrono::steady_clock::now();
                
                switch (info->Action) {
                    case FILE_ACTION_ADDED:
                        change.event = FileSystemEvent::Created;
                        break;
                    case FILE_ACTION_REMOVED:
                        change.event = FileSystemEvent::Deleted;
                        break;
                    case FILE_ACTION_MODIFIED:
                        change.event = FileSystemEvent::Modified;
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        // Store old name for next iteration
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        change.event = FileSystemEvent::Renamed;
                        break;
                }
                
                {
                    std::lock_guard<std::mutex> lock(m_changesMutex);
                    m_pendingChanges.push_back(change);
                }
            }
            
            offset += info->NextEntryOffset;
        } while (info->NextEntryOffset != 0);
    }
    
    CloseHandle(hDirectory);
#else
    // Fallback polling for non-Windows platforms
    std::unordered_map<std::string, std::time_t> lastModificationTimes;
    
    // Initialize modification times
    for (const auto& entry : std::filesystem::recursive_directory_iterator(m_assetDirectory)) {
        if (entry.is_regular_file() && IsAssetFile(entry.path().string())) {
            try {
                auto ftime = entry.last_write_time();
                lastModificationTimes[entry.path().string()] = 
                    std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            } catch (...) {
                // Ignore errors
            }
        }
    }
    
    while (m_isMonitoring) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Check for changes
        std::unordered_map<std::string, std::time_t> currentTimes;
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(m_assetDirectory)) {
            if (entry.is_regular_file() && IsAssetFile(entry.path().string())) {
                try {
                    auto ftime = entry.last_write_time();
                    std::time_t currentTime = 
                        std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
                    currentTimes[entry.path().string()] = currentTime;
                    
                    auto it = lastModificationTimes.find(entry.path().string());
                    if (it == lastModificationTimes.end()) {
                        // New file
                        FileSystemChange change;
                        change.path = entry.path().string();
                        change.event = FileSystemEvent::Created;
                        change.timestamp = std::chrono::steady_clock::now();
                        
                        {
                            std::lock_guard<std::mutex> lock(m_changesMutex);
                            m_pendingChanges.push_back(change);
                        }
                    } else if (it->second != currentTime) {
                        // Modified file
                        FileSystemChange change;
                        change.path = entry.path().string();
                        change.event = FileSystemEvent::Modified;
                        change.timestamp = std::chrono::steady_clock::now();
                        
                        {
                            std::lock_guard<std::mutex> lock(m_changesMutex);
                            m_pendingChanges.push_back(change);
                        }
                    }
                } catch (...) {
                    // Ignore errors
                }
            }
        }
        
        // Check for deleted files
        for (const auto& [path, time] : lastModificationTimes) {
            if (currentTimes.find(path) == currentTimes.end()) {
                // File was deleted
                FileSystemChange change;
                change.path = path;
                change.event = FileSystemEvent::Deleted;
                change.timestamp = std::chrono::steady_clock::now();
                
                {
                    std::lock_guard<std::mutex> lock(m_changesMutex);
                    m_pendingChanges.push_back(change);
                }
            }
        }
        
        lastModificationTimes = std::move(currentTimes);
    }
#endif
    
    std::cout << "File system monitoring stopped\n";
}

void AssetDatabase::ProcessFileSystemChanges()
{
    std::vector<FileSystemChange> changes;
    
    {
        std::lock_guard<std::mutex> lock(m_changesMutex);
        changes = std::move(m_pendingChanges);
        m_pendingChanges.clear();
    }
    
    if (changes.empty()) {
        return;
    }
    
    std::cout << "Processing " << changes.size() << " file system changes\n";
    
    for (const auto& change : changes) {
        HandleFileSystemChange(change);
        
        if (m_fileSystemCallback) {
            m_fileSystemCallback(change);
        }
    }
}

void AssetDatabase::HandleFileSystemChange(const FileSystemChange& change)
{
    switch (change.event) {
        case FileSystemEvent::Created:
            ImportAsset(change.path);
            break;
            
        case FileSystemEvent::Modified:
            if (GetAssetByPath(change.path)) {
                ReimportAsset(change.path);
            } else {
                ImportAsset(change.path);
            }
            break;
            
        case FileSystemEvent::Deleted:
            DeleteAsset(change.path);
            break;
            
        case FileSystemEvent::Renamed:
            // TODO: Handle file renames
            break;
    }
}

bool AssetDatabase::IsAssetFile(const std::string& filePath) const
{
    std::string extension = std::filesystem::path(filePath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    return s_extensionToType.find(extension) != s_extensionToType.end();
}

void AssetDatabase::UpdateAssetModificationTime(const std::string& assetPath)
{
    try {
        auto ftime = std::filesystem::last_write_time(assetPath);
        std::time_t modTime = std::chrono::duration_cast<std::chrono::seconds>(
            ftime.time_since_epoch()).count();
            
        auto it = m_assetMap.find(assetPath);
        if (it != m_assetMap.end()) {
            m_assets[it->second].lastModified = modTime;
        }
    } catch (...) {
        // Ignore errors
    }
}

} // namespace SparkEditor