#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <FS.h>
#include <time.h>
#include "USBHostMSC.h"

// --- Network & Destination Config ---
const char* WIFI_SSID     = "Virus";
const char* WIFI_PASSWORD = "abcd1234@358x";

const char* WEBHOOK_HOST  = "derola.app.n8n.cloud";
const int   WEBHOOK_PORT  = 443;
const char* WEBHOOK_PATH  = "/webhook/audio-diary";

#define CHUNK_SIZE 4096 // 4 KB RAM streaming buffer

Preferences prefs;
USBHostMSC msc;

// Scan root directory of the mounted filesystem for the newest .wav file
bool findLatestWavFile(fs::FS &fs, String &latestPath, size_t &fileSize, time_t &modTime) {
    File root = fs.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("[FS] Failed to open root directory.");
        return false;
    }

    File file = root.openNextFile();
    time_t newestTimestamp = 0;
    String targetFile = "";
    size_t targetSize = 0;

    while (file) {
        if (!file.isDirectory()) {
            String filename = String(file.name());
            if (filename.endsWith(".wav") || filename.endsWith(".WAV")) {
                time_t fTime = file.getLastWrite();
                if (fTime >= newestTimestamp) {
                    newestTimestamp = fTime;
                    targetFile = filename.startsWith("/") ? filename : ("/" + filename);
                    targetSize = file.size();
                }
            }
        }
        file = root.openNextFile();
    }

    if (targetFile.length() > 0) {
        latestPath = targetFile;
        fileSize = targetSize;
        modTime = newestTimestamp;
        return true;
    }
    return false;
}

// Single HTTPS POST streaming in 4 KB socket chunks
bool uploadFileToN8N(fs::FS &fs, const String &filePath, size_t fileSize) {
    WiFiClientSecure client;
    client.setInsecure(); // Bypass TLS root CA validation

    Serial.printf("[HTTP] Connecting to https://%s%s...\n", WEBHOOK_HOST, WEBHOOK_PATH);
    if (!client.connect(WEBHOOK_HOST, WEBHOOK_PORT)) {
        Serial.println("[HTTP] TLS connection failed.");
        return false;
    }

    File audioFile = fs.open(filePath.c_str(), FILE_READ);
    if (!audioFile) {
        Serial.println("[FS] Failed to open audio file.");
        client.stop();
        return false;
    }

    String filenameOnly = filePath.substring(filePath.lastIndexOf('/') + 1);
    String boundary = "----ESP32Boundary" + String(millis());

    // Build MIME multipart boundaries
    String header = "--" + boundary + "\r\n";
    header += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filenameOnly + "\"\r\n";
    header += "Content-Type: audio/wav\r\n\r\n";

    String footer = "\r\n--" + boundary + "--\r\n";

    // Set precise Content-Length so n8n recognizes payload completion
    size_t totalContentLength = header.length() + fileSize + footer.length();

    // Send HTTP Headers
    client.print(String("POST ") + WEBHOOK_PATH + " HTTP/1.1\r\n");
    client.print(String("Host: ") + WEBHOOK_HOST + "\r\n");
    client.print("User-Agent: ESP32-S3-WROOM-1\r\n");
    client.print("Accept: */*\r\n");
    client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
    client.print(String("Content-Length: ") + totalContentLength + "\r\n");
    client.print("Connection: close\r\n\r\n");

    // Send MIME part header
    client.print(header);

    // Allocate 4 KB chunk buffer
    uint8_t *chunkBuffer = (uint8_t *)malloc(CHUNK_SIZE);
    if (!chunkBuffer) {
        Serial.println("[MEM] Failed to allocate 4KB chunk buffer!");
        audioFile.close();
        client.stop();
        return false;
    }

    size_t bytesSent = 0;
    Serial.printf("[HTTP] Streaming %u bytes in 4KB chunks...\n", (unsigned int)fileSize);

    while (bytesSent < fileSize && audioFile.available()) {
        size_t toRead = ((fileSize - bytesSent) < CHUNK_SIZE) ? (fileSize - bytesSent) : CHUNK_SIZE;
        size_t bytesRead = audioFile.read(chunkBuffer, toRead);
        if (bytesRead == 0) break;

        client.write(chunkBuffer, bytesRead);
        bytesSent += bytesRead;

        Serial.printf("[HTTP] Progress: %u / %u bytes (%.1f%%)\r", 
                      (unsigned int)bytesSent, (unsigned int)fileSize, 
                      ((float)bytesSent / (float)fileSize) * 100.0);
        delay(1); 
    }
    Serial.println();

    free(chunkBuffer);
    audioFile.close();

    // Send closing boundary
    client.print(footer);

    // Await response from n8n
    unsigned long timeout = millis() + 15000;
    while (client.available() == 0) {
        if (millis() > timeout) {
            Serial.println("[HTTP] Server response timed out.");
            client.stop();
            return false;
        }
        delay(50);
    }

    String statusLine = client.readStringUntil('\n');
    Serial.print("[HTTP] Server Response: ");
    Serial.println(statusLine);

    bool success = statusLine.indexOf("200") > 0 || statusLine.indexOf("201") > 0;
    client.stop();
    return success;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n=== ESP32-S3 Audio Sync Initializing ===");

    prefs.begin("audio_sync", false);

    // 1. Wi-Fi Setup
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());

    // 2. Initialize Built-in Native USB Host
    Serial.println("[USB] Initializing USB Host...");
    msc.begin();
    Serial.println("[USB] Insert USB Flash Drive via OTG...");
}

void loop() {
    // Check if USB MSC is mounted and ready
    if (msc.isMounted()) {
        if (WiFi.status() == WL_CONNECTED) {
            String latestFile = "";
            size_t fileSize = 0;
            time_t modTime = 0;

            if (findLatestWavFile(msc, latestFile, fileSize, modTime)) {
                String lastUploaded = prefs.getString("last_file", "");
                time_t lastModTime = (time_t)prefs.getULong("last_mtime", 0);

                if (latestFile != lastUploaded || modTime != lastModTime) {
                    Serial.printf("\n[JOB] Found new recording: %s (%u bytes)\n", latestFile.c_str(), (unsigned int)fileSize);

                    if (uploadFileToN8N(msc, latestFile, fileSize)) {
                        Serial.println("[JOB] Upload succeeded! State saved.");
                        prefs.putString("last_file", latestFile);
                        prefs.putULong("last_mtime", (uint32_t)modTime);
                    } else {
                        Serial.println("[JOB] Upload failed. Retrying next cycle.");
                    }
                } else {
                    delay(3000);
                }
            } else {
                Serial.println("[FS] No .wav files discovered on USB drive.");
                delay(5000);
            }
        } else {
            Serial.println("[WiFi] Reconnecting...");
            WiFi.reconnect();
            delay(5000);
        }
    }

    delay(2000);
}
