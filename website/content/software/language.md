---
title: "Programming Language"
weight: 100103
---

C++ is our language of choice. The same language in which DirectX12 and Vulkan Graphics API are primarily available.

For scripting we have choose python as our language for it's near native performance and small footprint. To be integrated latter when we implement block / template definition and API sub-system.


We target c++23 language with some caveats.
* Do not define any new template. However templates defined in standard library std:: are permitted.
* We use inheritance feature very minimally of c++. Always strive to keep the data structure flatter, to reduce mental overhead of developers.


Some important C++ Concepts:
* Minimum c++ knowledge required: Primitive Variable Types, If/Else/Switch, Loops (For, While), Array, Pointers, Function Definition, Struct definition, Class Definition.
* From the standard library: std::vector<> , ?
* std::vector<> is guaranteed to be in contiguous memory and is accessible using arrayName[index] notation. Support internal inserts with O(N) complexity.
* Defining a function declaration as "inline" inside a .h file ensure that including this .h file in multiple .cpp file does not lead to duplicate declaration error.
* Struct and Class are basically same these days, except that struct has by default all variable public, and class has default private. Both can have public / private variables and function declaration.
* Compile time reinterpret_cast<> vs Runtime static_cast<> distinction.
* Template Programming; Standard Library (std::) heavily uses this. In our code, try to avoid Template. Template-heavy code can slow compilation.
* std::mutex is necessary for better co-ordination between different threads.
* inline keyword (c++17 onwards) for global variable defined in header only files. Allows the header to be included in multiple .cpp files and at the time of linking, linker considers all instances as just one.
