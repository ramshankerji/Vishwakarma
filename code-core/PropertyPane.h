// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

// Compile-time descriptor tables for the right-side object properties pane.
// Design document: website/content/software/propertiesPane.md
//
// Same philosophy as AllUIControls[] in UserInterface.h: one generic pane renderer + one generic
// apply function; supporting a new object type = adding one table. Fields are addressed through
// typed get/set accessor function pointers (NOT offsetof — the shape structs are not
// standard-layout), and there are no virtual functions on the data structs.

#include <cstddef>
#include <cstdint>

#include "CommonNamedNumbers.h"                // VishwakarmaStorage::ObjectType
#include "UserInterfaceTranslationCompiled.h" // UITextID

struct META_DATA; // Accessors take the base pointer; concrete down-casts live in PropertyPane.cpp.

enum class PropertyFieldKind : uint8_t { Float32 /*, Float64, Int, Text, Derived... future*/ };

struct PropertyFieldDescriptor {
    UITextID  labelStringID;         // e.g. UITextID::PropRadius, UITextID::PropCenterX
    float (*get)(const META_DATA*);  // typed accessor; reads the raw stored field
    void  (*set)(META_DATA*, float); // called by the engineering thread only
    PropertyFieldKind kind;          // MVP: Float32 only
    uint8_t   fieldIndex;            // Stable per-type index, used in the edit protocol.
    bool      mustBePositive;        // MVP validation hint (radii, diameters).
};

struct PropertyTypeDescriptor {
    VishwakarmaStorage::ObjectType objectType;
    const PropertyFieldDescriptor* fields;
    uint8_t fieldCount;
    // Optional cross-field rule (nullptr if none) over the post-edit field-value array, e.g.
    // PIPE inside < outside diameter, CYLINDER p1 != p2. The same function serves the UI-thread
    // pre-check (snapshot values) and the engineering-thread commit (live values).
    bool (*validateCrossField)(const float* values, uint8_t count, uint8_t editIndex, float newValue);
};

extern const PropertyTypeDescriptor kPropertyTables[]; // Sphere, Cylinder, Cone, Torus,
                                                       // Ellipsoid, Pipe, FrustumOfCone ...
extern const size_t kPropertyTableCount;

// Returns the table for a type, or nullptr for vertex-list types (Type + ID only in the MVP).
const PropertyTypeDescriptor* FindPropertyTable(VishwakarmaStorage::ObjectType objectType);

// MVP commit-time validator: reject NaN/Inf, reject <= 0 for mustBePositive fields, and run the
// optional per-type cross-field rule. One function, two call sites (UI pre-check + engineering
// authoritative gate), so there is no divergence between "looked valid" and "commit accepts".
bool ValidatePropertyEdit(const PropertyTypeDescriptor& table, const float* values, uint8_t count,
    uint8_t editIndex, float newValue);
