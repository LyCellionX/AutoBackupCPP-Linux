#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <ctime>
#include <filesystem>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <memory>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

class BackupSystem {
private:
    std::string folderToBackup;
    std::string backupFolder;
    std::vector<std::string> webhooks;
    int cooldownMinutes;
    bool isBackupInProgress;
    const double MAX_DIRECT_UPLOAD_SIZE = 23.0; // MB

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string getRandomWebhook() {
        if (webhooks.empty()) return "";
        int randomIndex = rand() % webhooks.size();
        return webhooks[randomIndex];
    }

    double getFileSizeInMB(const std::string& filepath) {
        std::error_code ec;
        uintmax_t size = fs::file_size(filepath, ec);
        if (ec) return 0.0;
        return static_cast<double>(size) / (1024 * 1024);
    }

    bool uploadToFileIO(const std::string& filepath, std::string& fileioLink) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        struct curl_httppost* formpost = nullptr;
        struct curl_httppost* lastptr = nullptr;

        curl_formadd(&formpost, &lastptr,
            CURLFORM_COPYNAME, "file",
            CURLFORM_FILE, filepath.c_str(),
            CURLFORM_END);

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "https://file.io/?expires=1w");
        curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        curl_formfree(formpost);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) return false;

        try {
            json responseJson = json::parse(response);
            fileioLink = responseJson["link"];
            return true;
        } catch (...) {
            return false;
        }
    }

    bool sendToWebhook(const std::string& webhookUrl, const std::string& filepath, bool isFileIOLink = false) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        struct curl_httppost* formpost = nullptr;
        struct curl_httppost* lastptr = nullptr;

        if (isFileIOLink) {
            std::string message = "autobackup by <@1025369998438453298>\n(" + filepath + ")";
            json webhookData = {
                {"content", message}
            };

            std::string jsonStr = webhookData.dump();
            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        } else {
            curl_formadd(&formpost, &lastptr,
                CURLFORM_COPYNAME, "file",
                CURLFORM_FILE, filepath.c_str(),
                CURLFORM_END);
            curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
        }

        curl_easy_setopt(curl, CURLOPT_URL, webhookUrl.c_str());
        
        CURLcode res = curl_easy_perform(curl);
        
        if (formpost) curl_formfree(formpost);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    bool createBackup() {
        if (isBackupInProgress) {
            std::cout << "Backup is already in progress.\n";
            return false;
        }

        isBackupInProgress = true;
        
        std::string backupFileName = "backup.7z";
        std::string backupFilePath = backupFolder + "/" + backupFileName;

        // Create backup command
        std::string cmd = "7z a \"" + backupFilePath + "\" \"" + folderToBackup + "\" -mx=9";
        
        int result = std::system(cmd.c_str());
        
        isBackupInProgress = false;

        if (result != 0) {
            std::cout << "Error creating backup.\n";
            return false;
        }

        std::cout << "Backup created successfully.\n";

        double fileSize = getFileSizeInMB(backupFilePath);
        std::string webhookUrl = getRandomWebhook();

        if (fileSize >= MAX_DIRECT_UPLOAD_SIZE) {
            std::string fileioLink;
            if (uploadToFileIO(backupFilePath, fileioLink)) {
                return sendToWebhook(webhookUrl, fileioLink, true);
            }
            return false;
        } else {
            return sendToWebhook(webhookUrl, backupFilePath);
        }
    }

public:
    BackupSystem() : isBackupInProgress(false) {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~BackupSystem() {
        curl_global_cleanup();
    }

    bool loadConfig(const std::string& configPath) {
        try {
            std::ifstream configFile(configPath);
            json config;
            configFile >> config;

            folderToBackup = config["folderToBackup"];
            backupFolder = config.value("backupFolder", "./backups");
            webhooks = config["webhooks"].get<std::vector<std::string>>();
            
            std::string cooldownStr = config.value("cooldownDuration", "*/60 * * * *");
            // Parse cron-like string to get minutes (simplified for this example)
            cooldownMinutes = 60; // Default to 60 minutes
            if (cooldownStr.find("*/") == 0) {
                cooldownMinutes = std::stoi(cooldownStr.substr(2));
            }

            // Create backup folder if it doesn't exist
            fs::create_directories(backupFolder);
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error loading config: " << e.what() << std::endl;
            return false;
        }
    }

    void start() {
        std::cout << "Backup system started. Running every " << cooldownMinutes << " minutes.\n";
        
        while (true) {
            createBackup();
            std::this_thread::sleep_for(std::chrono::minutes(cooldownMinutes));
        }
    }
};

int main() {
    BackupSystem backupSystem;
    
    if (!backupSystem.loadConfig("config.json")) {
        std::cerr << "Failed to load configuration.\n";
        return 1;
    }
    
    backupSystem.start();
    return 0;
}
