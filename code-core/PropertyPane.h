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

// No 3D type needs more than two point triples today (an axis has two ends). Fixed slots rather
// than a vector: these tables are compile-time constants read on the UI and engineering threads.
constexpr uint8_t kMaxPointGroups = 2;
constexpr uint8_t kNoPointGroup = 0xFF;

struct PropertyTypeDescriptor {
    VishwakarmaStorage::ObjectType objectType;
    const PropertyFieldDescriptor* fields;
    uint8_t fieldCount;
    // Optional cross-field rule (nullptr if none) over the post-edit field-value array, e.g.
    // PIPE inside < outside diameter, CYLINDER p1 != p2. The same function serves the UI-thread
    // pre-check (snapshot values) and the engineering-thread commit (live values).
    bool (*validateCrossField)(const float* values, uint8_t count, uint8_t editIndex, float newValue);

    /* Which fields form POINTS. Fields [pointGroupFirstField[g], +3) are the X/Y/Z of one point in
    the object's AUTHORED space, so they - and only they - are converted to and from world space by
    the two functions below. Scalars (radii, diameters, section parameters) are unaffected by a
    rigid placement and pass straight through.

    This lives on the TYPE rather than on each field so a table declares its points in one line
    instead of annotating every component, and so the grouping is stated rather than inferred from
    "the first three fields happen to be a point" - which is true of every table today and exactly
    the kind of thing that breaks silently when the first new table puts a scalar first. */
    uint8_t pointGroupFirstField[kMaxPointGroups];
    uint8_t pointGroupCount;
};

extern const PropertyTypeDescriptor kPropertyTables[]; // Sphere, Cylinder, Cone, Torus,
                                                       // Ellipsoid, Pipe, FrustumOfCone ...
extern const size_t kPropertyTableCount;

// Returns the table for a type, or nullptr for vertex-list types (Type + ID only in the MVP).
const PropertyTypeDescriptor* FindPropertyTable(VishwakarmaStorage::ObjectType objectType);

// MVP commit-time validator: reject NaN/Inf, reject <= 0 for mustBePositive fields, and run the
// optional per-type cross-field rule. One function, two call sites (UI pre-check + engineering
// authoritative gate), so there is no divergence between "looked valid" and "commit accepts".
//
// It sees the same WORLD values the user does. Every rule today is placement-invariant - the two
// point rules only ask whether two points coincide, which a rigid transform preserves, and the rest
// compare scalars a placement never touches - so world and authored space agree on every verdict.
bool ValidatePropertyEdit(const PropertyTypeDescriptor& table, const float* values, uint8_t count,
    uint8_t editIndex, float newValue);

/* The pane shows and accepts WORLD coordinates, while the object goes on storing AUTHORED ones
(website/software/graphics.md, 10M plan Step 4). Without this pair, moving an object would leave its
Properties Pane reading the position it was drawn at rather than the position it is displayed at.

Read: fills `out` (>= table.fieldCount entries) with point components carried to world space and
scalars copied verbatim.

Write: takes ONE edited value in world space. For a scalar it is a plain store; for a point
component it rebuilds that point in world space, inverse-transforms it, and writes back ALL THREE
authored components - because under a rotation every authored component depends on all three world
ones, so storing only the edited axis would silently skew the object. */
void ReadPropertyValuesForDisplay(const PropertyTypeDescriptor& table, const META_DATA* object,
    float* out);
void ApplyPropertyValueFromDisplay(const PropertyTypeDescriptor& table, META_DATA* object,
    uint8_t fieldIndex, float newValue);
