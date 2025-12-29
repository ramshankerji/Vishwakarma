// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// Please do not use this way of storing global variables together. They are better placed where they are used!
// Let compiler report all the global variables etc. Temporarily we could define few variable here, latter moved out.
#pragma once // Further to this, Global variables defined here need to be defined with "inline" prefix.
#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <any>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>
#include <new> // Required for std::align_val_t
#include "ID.h"
#include "MemoryManagerCPU.h"
#include "MemoryManagerGPU.h"
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <DirectXMath.h> //Where from? https://github.com/Microsoft/DirectXMath ?
#include <unordered_map>

// There will be only one instance of this struct in the entire application. Hence unnamed struct type.
struct {
    //***** Installation Details. *****
    // Installation details are only loaded at application startup time. Not continuously monitored on disc.
    bool isInstallationIDGenerated = false;
    char installationPublicKey[57] = "";      //ED448 Public Key
    char installationPrivateKey[57] = "";     //ED448 Private Key
    char installationID[16] = ""; //SHA256 of Public Key truncated to 1st 128 bits.

    //***** Centralized Application Variables. *****
    //***** Centralized Application Variables. *****
} globals;

//***** Distinct Unique Datafile/source *****

int activeDataSetNo; //The one current visible on windows.