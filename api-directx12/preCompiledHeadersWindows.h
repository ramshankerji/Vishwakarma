#pragma once //Compile this file only once. No matter how large it is.

/*
This set of header files correspond to system include files which are used frequenty
but rarelly change. So we can pre-compile this headers using this preCompiledHeadersWindows.h
file. This will enable faster Re-Compilation in future.
The default name of this file in Visual Studio is "stdafx.h", but we want a bit more discriptive
name. Similarly, the list of external libraries which needs to be pre-compile shall be stored
in preCompiledHeadersCore.h file.
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN //Exclude rarely-used stuff from Windows headers. Such as like cryptography, DDE, etc.
#endif
#include <windows.h>
