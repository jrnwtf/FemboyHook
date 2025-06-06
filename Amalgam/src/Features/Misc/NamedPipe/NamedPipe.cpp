//
//
//
// TODO: REFACTOR THIS DOGSHIT
// TODO: REFACTOR THIS DOGSHIT
// TODO: REFACTOR THIS DOGSHIT
//
//
//
#include "NamedPipe.h"
#include "../../../SDK/SDK.h"
#include "../../Players/PlayerUtils.h"
#include "../../Configs/Configs.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <sstream>
#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <unordered_map>
#include <filesystem>
#include <ShlObj.h>

namespace F::NamedPipe
{
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> shouldRun(true);
    std::thread pipeThread;
    std::ofstream logFile("C:\\pipe_log.txt", std::ios::app);
    int botId = -1;
    
    const int MAX_RECONNECT_ATTEMPTS = 10;
    const int BASE_RECONNECT_DELAY_MS = 500;
    const int MAX_RECONNECT_DELAY_MS = 10000;
    int currentReconnectAttempts = 0;
    DWORD lastConnectAttemptTime = 0;
    
    std::mutex messageQueueMutex;
    struct PendingMessage {
        std::string type;
        std::string content;
        bool isPriority;
    };
    std::vector<PendingMessage> messageQueue;
    
    std::mutex inboundMutex;
    std::vector<PendingMessage> inboundQueue;
    
    std::unordered_map<uint32_t, bool> localBots;
    

    void ConnectAndMaintainPipe();
    void QueueMessage(const std::string& type, const std::string& content, bool isPriority);
    void ProcessMessageQueue();
    bool SafeWriteToPipe(const std::string& message);
    int GetReconnectDelayMs();
    void QueueIncomingMessage(const std::string& type, const std::string& content);
    void ProcessIncomingQueue();
    
    void Log(const std::string& message)
    {
        if (!logFile.is_open())
        {
            std::cerr << "Failed to open log file" << std::endl;
            return;
        }
        logFile << message << std::endl;
        logFile.flush();
        OutputDebugStringA(("NamedPipe: " + message + "\n").c_str());
    }

    const char* PIPE_NAME = "\\\\.\\pipe\\AwootismBotPipe";

    std::string GetErrorMessage(DWORD error)
    {
        char* messageBuffer = nullptr;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
        std::string message(messageBuffer, size);
        LocalFree(messageBuffer);
        return message;
    }

    std::string GetTF2Folder()
    {
        // Try common Steam install paths first
        std::vector<std::string> commonPaths = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Team Fortress 2",
            "C:\\Program Files\\Steam\\steamapps\\common\\Team Fortress 2",
            "D:\\Steam\\steamapps\\common\\Team Fortress 2",
            "E:\\Steam\\steamapps\\common\\Team Fortress 2"
        };

        for (const auto& path : commonPaths) {
            if (std::filesystem::exists(path)) {
                Log("Found TF2 folder at common path: " + path);
                return path;
            }
        }

        // Try to get from the registry
        char steamPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(steamPath);
        HKEY hKey;
        
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "SteamPath", NULL, NULL, (LPBYTE)steamPath, &pathSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                std::string possiblePath = std::string(steamPath) + "\\steamapps\\common\\Team Fortress 2";
                if (std::filesystem::exists(possiblePath)) {
                    Log("Found TF2 folder from registry: " + possiblePath);
                    return possiblePath;
                }
            }
            RegCloseKey(hKey);
        }
        
        // Try config path as fallback
        std::string configPath = F::Configs.m_sConfigPath;
        Log("Using config path as fallback: " + configPath);
        size_t pos = configPath.find_last_of("\\");
        if (pos != std::string::npos) {
            std::string baseFolder = configPath.substr(0, pos);
            Log("Derived base folder: " + baseFolder);
            return baseFolder;
        }
            
        // Last resort - get exe path
        char modulePath[MAX_PATH];
        GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        
        std::string path = modulePath;
        Log("Module path: " + path);
        pos = path.find_last_of("\\");
        if (pos != std::string::npos)
            path = path.substr(0, pos);
            
        Log("Final TF2 folder determination: " + path);
        return path;
    }

    int ReadBotIdFromFile()
    {
        // Get paths to check
        std::string tf2Folder = GetTF2Folder();
        std::string amalgamFolder = F::Configs.m_sConfigPath;
        
        Log("Starting bot ID file search...");
        Log("TF2 folder: " + tf2Folder);
        Log("Amalgam folder: " + amalgamFolder);
        
        std::regex botFileRegex("bot(\\d+)\\.txt");
        
        // List of folders to check, in order of preference
        std::vector<std::pair<std::string, std::string>> foldersToCheck = {
            {tf2Folder, "TF2"},
            {amalgamFolder, "Amalgam"},
            {std::filesystem::current_path().string(), "Current Working Directory"}
        };
        
        // Try to find in each folder
        for (const auto& [folder, folderName] : foldersToCheck) {
            Log("Searching in " + folderName + " folder: " + folder);
            
            if (!std::filesystem::exists(folder)) {
                Log(folderName + " folder doesn't exist: " + folder);
                continue;
            }
            
            try {
                for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                    if (!entry.is_regular_file()) continue;
                    
                    std::string filename = entry.path().filename().string();
                    Log("Found file: " + filename);
                    
                    std::smatch match;
                    if (std::regex_match(filename, match, botFileRegex)) {
                        int botId = std::stoi(match[1]);
                        Log("Found bot ID " + std::to_string(botId) + " in " + folderName + " folder");
                        return botId;
                    }
                }
            } catch (const std::exception& e) {
                Log("Error searching " + folderName + " folder: " + std::string(e.what()));
                
                // Try old-style FindFirstFile as backup
                WIN32_FIND_DATA findFileData;
                HANDLE hFind = FindFirstFile((folder + "\\*.txt").c_str(), &findFileData);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        std::string filename(findFileData.cFileName);
                        Log("Found file (FindFirstFile): " + filename);
                        std::smatch match;
                        if (std::regex_match(filename, match, botFileRegex)) {
                            int botId = std::stoi(match[1]);
                            Log("Found bot ID " + std::to_string(botId) + " in " + folderName + " folder using FindFirstFile");
                            FindClose(hFind);
                            return botId;
                        }
                    } while (FindNextFile(hFind, &findFileData) != 0);
                    FindClose(hFind);
                } else {
                    DWORD error = GetLastError();
                    Log("FindFirstFile failed in " + folderName + " folder: " + GetErrorMessage(error));
                }
            }
        }
        
        // If no bot ID file was found, create one in the Amalgam folder
        Log("No bot ID file found, attempting to create one");
        
        // Try folders in reverse (write to TF2 folder last as it might require admin permissions)
        for (auto it = foldersToCheck.rbegin(); it != foldersToCheck.rend(); ++it) {
            const auto& [folder, folderName] = *it;
            
            if (!std::filesystem::exists(folder)) {
                Log("Can't create file in " + folderName + " folder - folder doesn't exist");
                continue;
            }
            
            try {
                std::string filepath = folder + "\\bot1.txt";
                Log("Attempting to create " + filepath);
                
                std::ofstream botFile(filepath);
                if (botFile.is_open()) {
                    botFile << "This file is used to identify the bot instance. ID: 1";
                    botFile.close();
                    
                    if (std::filesystem::exists(filepath)) {
                        Log("Successfully created bot ID file in " + folderName + " folder");
                        return 1;
                    } else {
                        Log("File creation reported success but file doesn't exist in " + folderName + " folder");
                    }
                } else {
                    Log("Failed to open file for writing in " + folderName + " folder");
                }
            } catch (const std::exception& e) {
                Log("Exception creating file in " + folderName + " folder: " + std::string(e.what()));
            }
        }
        
        Log("Failed to find or create any bot ID file");
        return -1;
    }

    void Initialize()
    {
        Log("NamedPipe::Initialize() called");
        botId = ReadBotIdFromFile();
        
        std::stringstream ss;
        if (botId == -1)
        {
            ss << "Failed to read bot ID from file";
            Log(ss.str());
        }
        else
        {
            ss << "Bot ID read from file: " << botId;
            Log(ss.str());
        }

        localBots.clear();
        Log("Cleared local bots list on startup");

        pipeThread = std::thread(ConnectAndMaintainPipe);
        Log("Pipe thread started");
    }

    void Shutdown()
    {
        shouldRun = false;
        if (pipeThread.joinable())
        {
            pipeThread.join();
        }
    }

    void SendStatusUpdate(const std::string& status)
    {
        // Queue the status update message with high priority
        QueueMessage("Status", status, true);
        
        // Process immediately if connected
        if (hPipe != INVALID_HANDLE_VALUE) {
            ProcessMessageQueue();
        }
    }

    void ExecuteCommand(const std::string& command)
    {
        Log("ExecuteCommand called with: " + command);
        
        if (command == "debug navbot")
        {
            Log("Debug navbot command received, sending 'kill' to TF2 console");
            if (I::EngineClient)
            {
                I::EngineClient->ClientCmd_Unrestricted("kill");
                Log("'kill' command sent to TF2 console");
                SendStatusUpdate("CommandExecuted:kill");
            }
            else
            {
                Log("Error: EngineClient is not available. 'kill' command not executed");
                SendStatusUpdate("CommandFailed:kill");
            }
            return;
        }
        
        if (command.substr(0, 11) == "loadconfig ")
        {
            std::string configName = command.substr(11);
            Log("Loading config: " + configName);
            
            if (F::Configs.LoadConfig(configName, true))
            {
                Log("Config loaded successfully: " + configName);
                SendStatusUpdate("ConfigLoaded:" + configName);
            }
            else
            {
                Log("Failed to load config: " + configName);
                SendStatusUpdate("ConfigLoadFailed:" + configName);
            }
        }
        else if (I::EngineClient)
        {
            Log("EngineClient is available, sending command to TF2 console");
            I::EngineClient->ClientCmd_Unrestricted(command.c_str());
            Log("Command sent to TF2 console: " + command);
            SendStatusUpdate("CommandExecuted:" + command);
        }
        else
        {
            Log("Error: EngineClient is not available. Command not executed: " + command);
            SendStatusUpdate("CommandFailed:" + command);
        }
    }

    void SendHealthUpdate(int health)
    {
        if (!I::EngineClient || !I::EngineClient->IsInGame())
            return;
        
        auto pLocal = H::Entities.GetLocal();
        if (!pLocal)
            return;
        
        health = pLocal->m_iHealth();
        QueueMessage("Health", std::to_string(health), false);
    }

    int GetCurrentPlayerClass()
    {
        if (!I::EngineClient || !I::EngineClient->IsInGame())
            return -1;
        
        auto pLocal = H::Entities.GetLocal();
        if (!pLocal)
            return -1;
        
        return pLocal->As<CTFPlayer>()->m_iClass();
    }

    void SendPlayerClassUpdate(int playerClass)
    {
        if (!I::EngineClient || !I::EngineClient->IsInGame())
            return;
        
        if (playerClass == -1)
            playerClass = GetCurrentPlayerClass();
        
        if (playerClass == -1)
            return;
        
        std::string className = "Unknown";
        const char* classStr = SDK::GetClassByIndex(playerClass);
        if (classStr)
            className = classStr;
        QueueMessage("PlayerClass", className, false);
    }

    std::string GetCurrentLevelName()
    {
        if (!I::EngineClient || !I::EngineClient->IsInGame())
            return "Unknown";
        
        return SDK::GetLevelName();
    }

    void SendMapUpdate()
    {
        if (!I::EngineClient || !I::EngineClient->IsInGame())
            return;
        
        static std::string lastSentMap = "";
        std::string currentMap = GetCurrentLevelName();
        
        if (currentMap == lastSentMap || currentMap == "Unknown")
            return;
        
        lastSentMap = currentMap;
        QueueMessage("Map", currentMap, false);
    }

    void SendServerInfo()
    {
        if (!I::EngineClient || !I::EngineClient->IsInGame() || I::EngineClient->IsPlayingDemo())
            return;
        
        std::string serverInfo = "Offline";
        
        INetChannelInfo* netInfo = I::EngineClient->GetNetChannelInfo();
        if (netInfo)
        {
            // Format: IP:Port
            const char* address = netInfo->GetAddress();
            if (address && *address)
            {
                serverInfo = address;
            }
        }
        QueueMessage("ServerInfo", serverInfo, false);
    }

    void UpdateBotInfo()
    {
        if (!I::EngineClient)
            return;
        
        SendHealthUpdate(0); // Health param will be ignored in the updated function
        SendPlayerClassUpdate(-1); // Will retrieve class directly
        SendMapUpdate();
        SendServerInfo();
    }

    void BroadcastLocalBotId()
    {
        if (!I::EngineClient)
            return;

        // Only proceed if we have a valid steam ID
        PlayerInfo_t pi{};
        int localIdx = I::EngineClient->GetLocalPlayer();
        if (I::EngineClient->GetPlayerInfo(localIdx, &pi) && pi.friendsID != 0)
        {
            // Use the friends ID from player info
            uint32_t friendsID = pi.friendsID;
            
            // Add ourselves to the local bots map
            localBots[friendsID] = true;
            
            // Queue local bot broadcast with high priority
            QueueMessage("LocalBot", std::to_string(friendsID), true);
            Log("Queued local bot ID broadcast: " + std::to_string(friendsID));
            
            // Process immediately if connected
            if (hPipe != INVALID_HANDLE_VALUE) {
                ProcessMessageQueue();
            }
        }
    }

    void ProcessLocalBotMessage(const std::string& message)
    {
        try {
            // Parse message format expected as: "botNumber:LocalBot:friendsID"
            std::istringstream iss(message);
            std::string botNumber, messageType, friendsIDstr;
            std::getline(iss, botNumber, ':');
            std::getline(iss, messageType, ':');
            std::getline(iss, friendsIDstr);
            
            Log("Processing LocalBot message - botNumber: " + botNumber + ", type: " + messageType + ", ID: " + friendsIDstr);
            
            if (messageType == "LocalBot" && !friendsIDstr.empty())
            {
                try
                {
                    uint32_t friendsID = std::stoull(friendsIDstr);
                    
                    // Don't skip messages from our own bot ID - we might have multiple instances
                    // with the same ID running
                    
                    // Add to local bots map if not already there
                    if (localBots.find(friendsID) == localBots.end()) {
                        localBots[friendsID] = true;
                        Log("Added local bot with friendsID: " + friendsIDstr);
                        
                        // Immediately try to find player info and add tags
                        if (I::EngineClient && I::EngineClient->IsInGame()) {
                            bool tagAdded = false;
                            PlayerInfo_t pi{};
                            
                            for (int i = 1; i <= I::EngineClient->GetMaxClients(); i++)
                            {
                                if (I::EngineClient->GetPlayerInfo(i, &pi) && pi.friendsID == friendsID)
                                {
                                    // Add both IGNORED_TAG and FRIEND_TAG
                                    F::PlayerUtils.AddTag(friendsID, F::PlayerUtils.TagToIndex(IGNORED_TAG), true, pi.name);
                                    F::PlayerUtils.AddTag(friendsID, F::PlayerUtils.TagToIndex(FRIEND_TAG), true, pi.name);
                                    Log("Marked local bot as ignored and friend: " + std::string(pi.name));
                                    tagAdded = true;
                                    break;
                                }
                            }
                            
                            if (!tagAdded)
                            {
                                Log("Could not find player info for friendsID: " + friendsIDstr + " to add tags");
                            }
                        } else {
                            Log("EngineClient unavailable or not in game. Will add tags later.");
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    Log("Error processing LocalBot message: " + std::string(e.what()));
                }
            }
        } catch (const std::exception& e) {
            Log("Exception in ProcessLocalBotMessage: " + std::string(e.what()));
        }
    }

    bool IsLocalBot(uint32_t friendsID)
    {
        if (friendsID == 0)
            return false;
            
        return localBots.find(friendsID) != localBots.end();
    }

    void UpdateLocalBotIgnoreStatus()
    {
        // First, process any pending messages to ensure we have the latest bot info
        ProcessIncomingQueue();
        
        // Broadcast our own ID
        BroadcastLocalBotId();
        
        // Make sure all known local bots are properly tagged
        for (const auto& [friendsID, isLocal] : localBots)
        {
            bool needsUpdate = false;
            
            if (!F::PlayerUtils.HasTag(friendsID, F::PlayerUtils.TagToIndex(IGNORED_TAG)))
            {
                needsUpdate = true;
            }
            
            if (!F::PlayerUtils.HasTag(friendsID, F::PlayerUtils.TagToIndex(FRIEND_TAG)))
            {
                needsUpdate = true;
            }
            
            if (needsUpdate)
            {
                PlayerInfo_t pi{};
                for (int i = 1; i <= I::EngineClient->GetMaxClients(); i++)
                {
                    if (I::EngineClient->GetPlayerInfo(i, &pi) && pi.friendsID == friendsID)
                    {
                        F::PlayerUtils.AddTag(friendsID, F::PlayerUtils.TagToIndex(IGNORED_TAG), true, pi.name);
                        F::PlayerUtils.AddTag(friendsID, F::PlayerUtils.TagToIndex(FRIEND_TAG), true, pi.name);
                        Log("Marked local bot as ignored and friend: " + std::string(pi.name));
                        break;
                    }
                }
            }
        }
    }

    void ClearLocalBots()
    {
        localBots.clear();
        Log("Cleared local bots list");
    }

    void QueueMessage(const std::string& type, const std::string& content, bool isPriority = false)
    {
        std::lock_guard<std::mutex> lock(messageQueueMutex);
        
        if (isPriority || messageQueue.size() < 100) {
            messageQueue.push_back({type, content, isPriority});
        } else {
            for (auto it = messageQueue.begin(); it != messageQueue.end(); ++it) {
                if (!it->isPriority) {
                    messageQueue.erase(it);
                    messageQueue.push_back({type, content, isPriority});
                    break;
                }
            }
        }
    }
    
    void ProcessMessageQueue()
    {
        if (hPipe == INVALID_HANDLE_VALUE) return;
        
        std::lock_guard<std::mutex> lock(messageQueueMutex);
        if (messageQueue.empty()) return;
        
        int processCount = 0;
        auto it = messageQueue.begin();
        while (it != messageQueue.end() && processCount < 10) {
            std::string message;
            if (botId != -1) {
                message = std::to_string(botId) + ":" + it->type + ":" + it->content + "\n";
            } else {
                message = "0:" + it->type + ":" + it->content + "\n";
            }
            
            DWORD bytesWritten = 0;
            BOOL success = WriteFile(hPipe, message.c_str(), static_cast<DWORD>(message.length()), &bytesWritten, NULL);
            
            if (success && bytesWritten == message.length()) {
                it = messageQueue.erase(it);
                processCount++;
            } else {
                Log("Failed to write queued message: " + std::to_string(GetLastError()));
                break;
            }
        }
    }

    bool SafeWriteToPipe(const std::string& message)
    {
        if (hPipe == INVALID_HANDLE_VALUE) {
            QueueMessage("Status", "QueuedMessage", false);
            return false;
        }
        
        DWORD bytesWritten = 0;
        BOOL success = WriteFile(hPipe, message.c_str(), static_cast<DWORD>(message.length()), &bytesWritten, NULL);
        
        if (!success || bytesWritten != message.length()) {
            DWORD error = GetLastError();
            Log("WriteFile failed: " + std::to_string(error) + " - " + GetErrorMessage(error));
            
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                CloseHandle(hPipe);
                hPipe = INVALID_HANDLE_VALUE;
            }
            return false;
        }
        return true;
    }
    
    int GetReconnectDelayMs()
    {
        int delay = std::min(
            BASE_RECONNECT_DELAY_MS * (1 << std::min(currentReconnectAttempts, 10)), 
            MAX_RECONNECT_DELAY_MS
        );
        
        int jitter = delay * 0.2 * (static_cast<double>(rand()) / RAND_MAX - 0.5);
        return delay + jitter;
    }

    void QueueIncomingMessage(const std::string& type, const std::string& content)
    {
        std::lock_guard<std::mutex> lock(inboundMutex);
        inboundQueue.push_back({type, content, false});
    }

    void ProcessIncomingQueue()
    {
        std::lock_guard<std::mutex> lock(inboundMutex);
        
        auto it = inboundQueue.begin();
        while (it != inboundQueue.end()) {
            if (it->type == "Command") {
                ExecuteCommand(it->content);
                it = inboundQueue.erase(it);
            } else if (it->type == "LocalBot") {
                ProcessLocalBotMessage(it->content);
                it = inboundQueue.erase(it);
            } else {
                // Unknown message type, just remove it
                Log("Unknown message type in inbound queue: " + it->type);
                it = inboundQueue.erase(it);
            }
        }
    }

    void ConnectAndMaintainPipe()
    {
        Log("ConnectAndMaintainPipe started");
        srand(static_cast<unsigned int>(time(nullptr)));
        
        // For periodic local bot broadcasting
        DWORD lastBroadcastTime = 0;
        const DWORD BROADCAST_INTERVAL_MS = 5000; // 5 seconds
        
        // Add time tracking for heartbeat and status updates
        DWORD lastHeartbeatTime = 0;
        const DWORD MIN_HEARTBEAT_INTERVAL_MS = 2000; // 2 seconds minimum
        const DWORD MAX_HEARTBEAT_INTERVAL_MS = 4000; // 4 seconds maximum
        DWORD currentHeartbeatInterval = MIN_HEARTBEAT_INTERVAL_MS;
        
        // Time tracking for player updates
        DWORD lastPlayerUpdateTime = 0;
        const DWORD PLAYER_UPDATE_INTERVAL_MS = 3000; // 3 seconds
        
        while (shouldRun)
        {
            DWORD currentTime = GetTickCount();
            
            // Process any pending inbound messages
            ProcessIncomingQueue();
            
            // Periodically broadcast local bot ID when in game
            if (I::EngineClient && I::EngineClient->IsInGame() && 
                currentTime - lastBroadcastTime > BROADCAST_INTERVAL_MS) {
                BroadcastLocalBotId();
                lastBroadcastTime = currentTime;
            }
            
            if (hPipe == INVALID_HANDLE_VALUE)
            {
                int reconnectDelay = GetReconnectDelayMs();
                if (currentTime - lastConnectAttemptTime > static_cast<DWORD>(reconnectDelay) || lastConnectAttemptTime == 0)
                {
                    lastConnectAttemptTime = currentTime;
                    currentReconnectAttempts++;
                    
                    Log("Attempting to connect to pipe (attempt " + std::to_string(currentReconnectAttempts) + 
                        ", delay: " + std::to_string(reconnectDelay) + "ms)");
                        

                    OVERLAPPED overlapped = {0};
                    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                    
                    if (overlapped.hEvent == NULL) {
                        Log("Failed to create connection event: " + std::to_string(GetLastError()));
                        std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelay));
                        continue;
                    }
                    
                    hPipe = CreateFile(
                        PIPE_NAME,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);
    
                    if (hPipe != INVALID_HANDLE_VALUE)
                    {

                        currentReconnectAttempts = 0;
                        Log("Connected to pipe");
                        

                        DWORD pipeMode = PIPE_READMODE_MESSAGE;
                        SetNamedPipeHandleState(hPipe, &pipeMode, NULL, NULL);
                        

                        QueueMessage("Status", "Connected", true);
                        ProcessMessageQueue();

                        if (botId != -1)
                        {
                            Log("Using Bot ID: " + std::to_string(botId));
                        }
                        else
                        {
                            Log("Warning: Bot ID not set");
                        }
                        
                        ClearLocalBots();
                        
                        // Reset time trackers on new connection
                        lastHeartbeatTime = currentTime;
                        lastPlayerUpdateTime = currentTime;
                    }
                    else
                    {
                        DWORD error = GetLastError();
                        Log("Failed to connect to pipe: " + std::to_string(error) + " - " + GetErrorMessage(error));
                    }
                    
                    CloseHandle(overlapped.hEvent);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            if (hPipe != INVALID_HANDLE_VALUE)
            {

                DWORD bytesAvail = 0;
                if (!PeekNamedPipe(hPipe, NULL, 0, NULL, &bytesAvail, NULL)) {
                    DWORD error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
                        Log("Pipe disconnected: " + std::to_string(error) + " - " + GetErrorMessage(error));
                        CloseHandle(hPipe);
                        hPipe = INVALID_HANDLE_VALUE;
                        continue;
                    }
                }
                

                ProcessMessageQueue();
                
                // Send heartbeat only every 2-4 seconds
                if (currentTime - lastHeartbeatTime >= currentHeartbeatInterval)
                {
                    QueueMessage("Status", "Heartbeat", true);
                    ProcessMessageQueue();
                    lastHeartbeatTime = currentTime;
                    
                    // Randomize next heartbeat interval between MIN and MAX
                    currentHeartbeatInterval = MIN_HEARTBEAT_INTERVAL_MS + 
                        rand() % (MAX_HEARTBEAT_INTERVAL_MS - MIN_HEARTBEAT_INTERVAL_MS + 1);
                }
                
                // Send player updates every PLAYER_UPDATE_INTERVAL_MS
                if (currentTime - lastPlayerUpdateTime >= PLAYER_UPDATE_INTERVAL_MS) 
                {
                    QueueMessage("PlayerClass", std::to_string(GetCurrentPlayerClass()), false);
                    QueueMessage("Map", GetCurrentLevelName(), false);
                    QueueMessage("ServerInfo", "Player", false);
                    ProcessMessageQueue();
                    
                    if (I::EngineClient && I::EngineClient->IsInGame()) {
                        UpdateLocalBotIgnoreStatus();
                    }
                    
                    lastPlayerUpdateTime = currentTime;
                }
                

                char buffer[4096] = {0};
                DWORD bytesRead = 0;
                

                OVERLAPPED overlapped = {0};
                overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                
                if (overlapped.hEvent != NULL) {

                    if (bytesAvail > 0) {
                        BOOL readSuccess = ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, &overlapped);
                        
                        if (!readSuccess && GetLastError() == ERROR_IO_PENDING) {
                            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 1000);
                            
                            if (waitResult == WAIT_OBJECT_0) {
                                if (!GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE)) {
                                    Log("GetOverlappedResult failed: " + std::to_string(GetLastError()));
                                    CloseHandle(overlapped.hEvent);
                                    CloseHandle(hPipe);
                                    hPipe = INVALID_HANDLE_VALUE;
                                    continue;
                                }
                            } else if (waitResult == WAIT_TIMEOUT) {
                                CancelIo(hPipe);
                                CloseHandle(overlapped.hEvent);
                                continue;
                            } else {
                                Log("Wait failed: " + std::to_string(GetLastError()));
                                CloseHandle(overlapped.hEvent);
                                CloseHandle(hPipe);
                                hPipe = INVALID_HANDLE_VALUE;
                                continue;
                            }
                        } else if (!readSuccess) {
                            Log("ReadFile failed immediately: " + std::to_string(GetLastError()));
                            CloseHandle(overlapped.hEvent);
                            CloseHandle(hPipe);
                            hPipe = INVALID_HANDLE_VALUE;
                            continue;
                        }
                        

                        if (bytesRead > 0) {
                            buffer[bytesRead] = '\0'; // Ensure null termination
                            std::string message(buffer, bytesRead);
                            Log("Received message: " + message);
                            

                            std::stringstream ss(message);
                            std::string line;
                            
                            while (std::getline(ss, line)) {
                                if (line.empty()) continue;
                                

                                std::istringstream iss(line);
                                std::string botNumber, messageType, content;
                                std::getline(iss, botNumber, ':');
                                std::getline(iss, messageType, ':');
                                std::getline(iss, content);

                                if (messageType == "Command") {
                                    Log("Queued incoming Command: " + content);
                                    QueueIncomingMessage(messageType, content);
                                } else if (messageType == "LocalBot") {
                                    Log("Queued incoming LocalBot: " + line);
                                    QueueIncomingMessage(messageType, line);
                                } else {
                                    Log("Received unknown message type: " + messageType);
                                }
                            }
                        }
                    }
                    
                    CloseHandle(overlapped.hEvent);
                }
            }
            

            if (hPipe == INVALID_HANDLE_VALUE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                // Increase sleep time to reduce CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }


        {
            std::lock_guard<std::mutex> lock(messageQueueMutex);
            messageQueue.clear();
        }
        
        if (hPipe != INVALID_HANDLE_VALUE)
        {

            try {
                if (botId != -1) {
                    std::string message = std::to_string(botId) + ":Status:Disconnecting\n";
                    DWORD bytesWritten = 0;
                    WriteFile(hPipe, message.c_str(), static_cast<DWORD>(message.length()), &bytesWritten, NULL);
                }
            } catch (...) {
                // Ignore any errors during shutdown
            }
            
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
        }
        Log("ConnectAndMaintainPipe ended");
    }
} 