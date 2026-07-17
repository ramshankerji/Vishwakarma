// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>

/* Compile-time embedded copy of the hot-rolled steel section catalog
(Catalog/profiles_hot_*.csv). Design doc: website/content/civil/SteelTable.md.
The row data is generated into SteelProfileCatalogEmbedded.generated.h by
code-miscellaneous/steel_profile_embedder.py as a pre-build step; this header
owns the record layout and the lookup helpers. */

enum class SteelProfileFamily : uint8_t {
    Angle,
    Bar,
    Bulb,
    Channel,
    CHS,
    I,
    Rail,
    RHS,
    Tee,
    Parametric, // RC beam/column sections; outline picked by `series` (RECT/CIRC/OCT/HEX),
                // dimensions from LINE_MEMBER::userParameter1/2 (CSV row = defaults).
};

struct SteelProfileRecord {
    uint64_t id;                 // Permanent catalog id, [2^32, 2^40) band (id.md).
    uint64_t supersededBy;       // Replacement row id; 0 when not superseded.
    SteelProfileFamily family;
    uint8_t statusActive;        // 1 = active, 0 = superseded.
    uint8_t availabilityCurrent; // 1 = current (still rolled), 0 = legacy.
    const char* key;             // Unique "CODE:DESIGNATION" key.
    const char* country;
    const char* code;            // Governing standard.
    const char* series;          // ISMB, IPE, EA, FLAT, ... (BAR: doubles as the shape).
    const char* designation;     // Canonical name: "ISMB 400", "W12X26", ...
    const char* altDesignation;  // Alias in another system; "" otherwise.
    // Geometry straight from the CSVs, millimeters. Fields a family does not use stay 0.
    // PARAMETRIC defaults reuse h,b (RECT), d (CIRC) and a (OCT/HEX across flats).
    float h, b, tw, tf, r1, r2, flangeSlope; // I / CHANNEL / TEE (tf at gauge point (b-tw)/4)
    float a, t, t2;                          // ANGLE legs a,b x t (t2: JIS L-UT); BAR dims a,b
    float d;                                 // CHS outside diameter
    float rOut, rIn;                         // RHS corner radii
    float bulbH;                             // BULB bulb height
    float bHead, bFoot;                      // RAIL head / foot widths
    float mass;                              // kg/m, tabulated identity check only
};

#include "SteelProfileCatalogEmbedded.generated.h"

// Binary search over the id-sorted embedded table. Returns nullptr when absent.
inline const SteelProfileRecord* FindSteelProfileById(uint64_t id) {
    uint32_t low = 0, high = kSteelProfileCount;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        if (kSteelProfiles[mid].id < id) low = mid + 1;
        else high = mid;
    }
    if (low < kSteelProfileCount && kSteelProfiles[low].id == id) return &kSteelProfiles[low];
    return nullptr;
}
