// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
// Force-included (/FI) into every pythoncore translation unit by CPythonStatic.props.
// The stock PCbuild project passes Py_ENABLE_SHARED on the command line; undoing it
// here (before PC/pyconfig.h is preprocessed) makes pyconfig.h configure a fully
// static CPython: no MS_COREDLL, no dllimport/dllexport, no python313.dll.
#undef Py_ENABLE_SHARED
#ifndef Py_NO_ENABLE_SHARED
#define Py_NO_ENABLE_SHARED 1
#endif
