// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include <cstdint>
#include <vector>

#pragma once //It prevents multiple inclusions of the same header file.

struct NETWORK_INTERFACE {
    uint16_t type; // 0: IPv6, 1: IPv4
    uint16_t port; // The port we are either accespting connection or connecting to.
    char* ipAddress[16]; // IPv6 are 128 byte (=16 Byte), IPv4 will 32 bits i.e. 1st 4 Byte Only. 
};

struct DATASET {
    // Each opened dataSet is considered / shown as a TAB. It could consist of multiple .yyy & .zzz file.
    // It could either load from local disc attached to OS OR loaded from remote network share OR loaded from same application running on other computer on network.
    // Network share is different because we don't want to submit overburden remote shared server with design calculation transient files.
    
    /* Tab Codes.
    0 : Unsaved Tab. All data is in memory only.
    1 : Directly Opened from local file storage.
    2 : Directly Opened from Network file storage.
    3 : Subscribed to peers
    Tab codes.*/
    int mode = 0;

    /* In general, unless explicitely tuned, windows file full path including the "c:\" and terminating NULL character is 3+256+1=260
    character long. If long path is enabled, it is 2^16-1=32767
    https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
    Linux also has 255 Character as file system Limit.  https://en.wikipedia.org/wiki/Ext4
    */
    int isShortPath = 0; //0: when it's a short path, 1 when it is long.
    char shortFileName[256]; // Example: "DesignFile.bha"
    char shortFilePath[256]; // Example: "C:\Folder1\Folder2\Folder3\"
    char* longFileName = NULL;
    char* longFilePath = NULL;
    NETWORK_INTERFACE networkFile; //If we are loading from same application running on another network computer.

    // Encryption Keys and ID of the file.
    char* filePublicKey[57]; //ED448 Public Key
    char* fileSecretKey[57]; //ED448 Private Key
    char* fileNounce[16]; //Internal AES encryption key of the file.
    char* fileID[16]; //SHA256 of Public Key trauncated to 1st 128 bits.

};

// Following is the main application. It is THE global variable. 
// There will be only one instance of this class in the entire application. Hence unnamed struct type.
struct {
    //***** Installation Details. *****
    // Installatio details are only loaded at application startup time. Not continusly monitored on disc.
    bool isInstallationIDGenerated = false;
    char installationPublicKey[57];      //ED448 Public Key
    char installationPrivateKey[57];     //ED448 Private Key
    char installationID[16]; //SHA256 of Public Key trauncated to 1st 128 bits.

    
    
    
    /*
    
    */

    //***** Distinct Unique Datfile/source *****
    // Different tabs represent different files opened in the software.Just like different website links open in different internet browser tab.
    // Tab No. 0 Show the opening screen.i.e.Not associated with any particular opened file. 1 DATASET = 1 TAB visible to user / to website.
    uint8_t noOfOpenedDataset = 0;
    // We will allow user to open as many files simultaneously as system RAM allows.
    // Particularly, enterprise central repositary may have thousands of projects.
    // Hence this is one of the rate location where we allow dynamic allocation done by std:vector.
    std::vector<DATASET> dataSet; //Graws expnentially. 1.5x for GCC/Clang, 2x for MSVC.
    int activeDataSetNo; //The one current visible on windows.

    //***** Centralized Application Variables. *****
    //***** Centralized Application Variables. *****
    //***** Centralized Application Variables. *****
    //***** Centralized Application Variables. *****
    //***** Centralized Application Variables. *****
}राम;